#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <endian.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>

#if (defined(USE_MBEDTLS) + defined(USE_KCAPI) + defined(USE_OPENSSL)) > 1
#error "Select only one crypto backend"
#endif

#if defined(USE_MBEDTLS) || defined(USE_KCAPI) || defined(USE_OPENSSL)
#  define CRYPTO_ENABLED 1
#else
#  define CRYPTO_ENABLED 0
#endif

#if defined(USE_MBEDTLS)
#  include <mbedtls/gcm.h>
#elif defined(USE_KCAPI)
#  include <kcapi.h>
#elif defined(USE_OPENSSL)
#  include <openssl/evp.h>
#  include <openssl/err.h>
#  include <openssl/provider.h>
#endif

/* ---------------- Tunables ---------------- */

static const size_t MSG_SIZES[] = { 64, 256, 1024, 4096, 16384, 65536, 262144, 1048576 };
#define NUM_SIZES     (sizeof(MSG_SIZES) / sizeof(MSG_SIZES[0]))
#define ITERS         1000

#if CRYPTO_ENABLED
#  define TAG_LEN     16
#  define HDR_LEN     16               /* 8B session_id + 8B seq */
#else
#  define TAG_LEN     0
#  define HDR_LEN     0
#endif

#define MAX_MSG       1048576
#define MAX_PLAINTEXT (HDR_LEN + MAX_MSG)
#define BUF_LEN       (MAX_PLAINTEXT + TAG_LEN)

/* Handshake sentinels (do not collide with normal seq=1..ITERS) */
#define HS_SESSION    (UINT64_MAX)       /* session handshake (crypto only) */
#define SIZE_START    (UINT64_MAX - 2)   /* start-of-size barrier */
#define SIZE_DONE     (UINT64_MAX - 1)   /* end-of-size barrier */

static unsigned char tmp[MAX_PLAINTEXT];

/* ---------------- Shared memory layout ---------------- */
struct shm_region {
    _Atomic uint64_t seq  __attribute__((aligned(64)));
    _Atomic uint64_t ack  __attribute__((aligned(64)));
    unsigned char    buf[BUF_LEN] __attribute__((aligned(64)));
};

/* ---------------- Helpers ---------------- */

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#else
    sched_yield();
#endif
}

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#if defined(__aarch64__)
static inline uint64_t rd_cntvct(void) { uint64_t v; asm volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static inline uint64_t rd_cntfrq(void) { uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#else
static inline uint64_t rd_cntvct(void) { return now_ns(); }
static inline uint64_t rd_cntfrq(void) { return 1000000000ULL; }
#endif

/* PMU safe read (AArch64 PMCCNTR_EL0). If not allowed in EL0, fall back. */
static sigjmp_buf g_ill_jmp;
static void sigill_handler(int signo) { (void)signo; siglongjmp(g_ill_jmp, 1); }

static bool pmu_read_cycle(uint64_t *val_out)
{
#if !defined(__aarch64__)
    (void)val_out;
    return false;
#else
    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = sigill_handler;
    sigemptyset(&sa_new.sa_mask);
    if (sigaction(SIGILL, &sa_new, &sa_old) != 0) return false;

    if (sigsetjmp(g_ill_jmp, 1) != 0) {
        sigaction(SIGILL, &sa_old, NULL);
        return false;
    }

    uint64_t v = 0;
    asm volatile("mrs %0, pmccntr_el0" : "=r"(v));
    sigaction(SIGILL, &sa_old, NULL);
    *val_out = v;
    return true;
#endif
}

/* Busy wait for *addr to equal want. */
static inline void wait_u64_eq(_Atomic uint64_t *addr, uint64_t want)
{
    for (;;) {
        if (atomic_load_explicit(addr, memory_order_acquire) == want) return;
        cpu_relax();
    }
}

/* Warm-up: fault in pages so first access doesn’t skew timing. */
static inline void warmup_pages(volatile unsigned char *p, size_t len)
{
    const size_t ps = 4096;
    for (size_t off = 0; off < len; off += ps) (void)p[off];
}

/* ---------------- Header helpers (crypto modes) ---------------- */
#if CRYPTO_ENABLED
static inline void write_header(unsigned char *dst, uint64_t session_id, uint64_t seq)
{
    memcpy(dst + 0, &session_id, sizeof(session_id));
    memcpy(dst + 8, &seq,        sizeof(seq));
}
static inline void read_header(const unsigned char *src, uint64_t *session_id, uint64_t *seq)
{
    memcpy(session_id, src + 0, sizeof(*session_id));
    memcpy(seq,        src + 8, sizeof(*seq));
}
#endif

/* 96-bit IV derived from (session_id, seq) for GCM backends. */
static inline void make_gcm_iv(uint8_t iv[12], uint64_t session_id, uint64_t seq)
{
    uint32_t sid = (uint32_t)(session_id ^ 0xA5A5A5A5u);
    uint32_t sid_be = htobe32(sid);
    uint64_t seq_be = htobe64(seq);
    memcpy(iv,     &sid_be, 4);
    memcpy(iv + 4, &seq_be, 8);
}

/* ---------------- Crypto backends ---------------- */
#if CRYPTO_ENABLED
static const unsigned char key[32] = {
    0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,
    0x81,0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
};
#endif

static void  crypto_init(void);
static void  crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                            uint64_t session_id, uint64_t seq);
static void  crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                            uint64_t session_id, uint64_t seq);
static void  crypto_cleanup(void);

#if defined(USE_MBEDTLS)
/* ---- mbedTLS AES-256-GCM ---- */
static mbedtls_gcm_context gcm_enc, gcm_dec;
static void crypto_init(void)
{
    mbedtls_gcm_init(&gcm_enc);
    mbedtls_gcm_init(&gcm_dec);
    if (mbedtls_gcm_setkey(&gcm_enc, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) exit(2);
    if (mbedtls_gcm_setkey(&gcm_dec, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) exit(2);
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    unsigned char *ct  = dst;
    unsigned char *tag = dst + len;
    if (mbedtls_gcm_crypt_and_tag(&gcm_enc, MBEDTLS_GCM_ENCRYPT, len,
                                  iv, sizeof(iv), NULL, 0, src, ct, TAG_LEN, tag) != 0)
        exit(3);
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    const unsigned char *ct  = src;
    const unsigned char *tag = src + len;
    if (mbedtls_gcm_auth_decrypt(&gcm_dec, len, iv, sizeof(iv), NULL, 0, tag, TAG_LEN, ct, dst) != 0)
        exit(4);
}
static void crypto_cleanup(void)
{
    mbedtls_gcm_free(&gcm_enc); mbedtls_gcm_free(&gcm_dec);
}

#elif defined(USE_KCAPI)
/* ---- libkcapi AES-256-GCM ---- */
static struct kcapi_handle *aead;
static void crypto_init(void)
{
    if (kcapi_aead_init(&aead, "gcm(aes)", 0) < 0) exit(2);
    if (kcapi_aead_setkey(aead, key, sizeof key) < 0) exit(2);
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    ssize_t r = kcapi_aead_encrypt(aead, src, len, iv, dst, len + TAG_LEN, KCAPI_ACCESS_HEURISTIC);
    if (r < 0 || (size_t)r != len + TAG_LEN) exit(3);
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    ssize_t r = kcapi_aead_decrypt(aead, src, len + TAG_LEN, iv, dst, len, KCAPI_ACCESS_HEURISTIC);
    if (r < 0 || (size_t)r != len) exit(4);
}
static void crypto_cleanup(void) { kcapi_aead_destroy(aead); }

#elif defined(USE_OPENSSL)
/* ---- OpenSSL: prefer AES-256-GCM-SIV/SIV; fallback to AES-256-GCM ---- */
static EVP_CIPHER_CTX *enc = NULL, *dec = NULL;
static EVP_CIPHER     *cipher_fetch = NULL;
static const EVP_CIPHER *cipher_gcm = NULL;
static OSSL_PROVIDER  *prov_default = NULL, *prov_legacy = NULL;
static int use_gcm = 0;
static const unsigned char CONST_IV[12] = {0};

static void crypto_init(void)
{
    prov_default = OSSL_PROVIDER_load(NULL, "default");
    if (!prov_default) exit(2);
    prov_legacy  = OSSL_PROVIDER_load(NULL, "legacy");

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    cipher_fetch = EVP_CIPHER_fetch(NULL, "AES-256-GCM-SIV", NULL);
    if (!cipher_fetch) cipher_fetch = EVP_CIPHER_fetch(NULL, "AES-256-SIV", NULL);
#endif
    if (!cipher_fetch) {
        cipher_gcm = EVP_aes_256_gcm();
        if (!cipher_gcm) exit(2);
        use_gcm = 1;
    }
    enc = EVP_CIPHER_CTX_new(); dec = EVP_CIPHER_CTX_new();
    if (!enc || !dec) exit(2);
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    int outl = 0, tmp2 = 0;
    if (!use_gcm) {
        if (EVP_EncryptInit_ex(enc, cipher_fetch, NULL, NULL, NULL) != 1) exit(3);
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(CONST_IV), NULL) != 1) exit(3);
        if (EVP_EncryptInit_ex(enc, NULL, NULL, key, CONST_IV) != 1) exit(3);
        if (EVP_EncryptUpdate(enc, dst, &outl, src, (int)len) != 1) exit(3);
        if (EVP_EncryptFinal_ex(enc, dst + outl, &tmp2) != 1) exit(3);
        outl += tmp2;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, dst + len) != 1) exit(3);
        if (outl != (int)len) exit(3);
    } else {
        unsigned char iv[12]; make_gcm_iv(iv, session_id, seq);
        if (EVP_EncryptInit_ex(enc, cipher_gcm, NULL, NULL, NULL) != 1) exit(3);
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(iv), NULL) != 1) exit(3);
        if (EVP_EncryptInit_ex(enc, NULL, NULL, key, iv) != 1) exit(3);
        if (EVP_EncryptUpdate(enc, dst, &outl, src, (int)len) != 1) exit(3);
        if (EVP_EncryptFinal_ex(enc, dst + outl, &tmp2) != 1) exit(3);
        outl += tmp2;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, dst + len) != 1) exit(3);
        if (outl != (int)len) exit(3);
    }
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    int outl = 0, tmp2 = 0;
    if (!use_gcm) {
        if (EVP_DecryptInit_ex(dec, cipher_fetch, NULL, NULL, NULL) != 1) exit(4);
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(CONST_IV), NULL) != 1) exit(4);
        if (EVP_DecryptInit_ex(dec, NULL, NULL, key, CONST_IV) != 1) exit(4);
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, (void*)(src + len)) != 1) exit(4);
        if (EVP_DecryptUpdate(dec, dst, &outl, src, (int)len) != 1) exit(4);
        if (EVP_DecryptFinal_ex(dec, dst + outl, &tmp2) != 1) exit(4);
        outl += tmp2;
        if (outl != (int)len) exit(4);
    } else {
        unsigned char iv[12]; make_gcm_iv(iv, session_id, seq);
        if (EVP_DecryptInit_ex(dec, cipher_gcm, NULL, NULL, NULL) != 1) exit(4);
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(iv), NULL) != 1) exit(4);
        if (EVP_DecryptInit_ex(dec, NULL, NULL, key, iv) != 1) exit(4);
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, (void*)(src + len)) != 1) exit(4);
        if (EVP_DecryptUpdate(dec, dst, &outl, src, (int)len) != 1) exit(4);
        if (EVP_DecryptFinal_ex(dec, dst + outl, &tmp2) != 1) exit(4);
        outl += tmp2;
        if (outl != (int)len) exit(4);
    }
}
static void crypto_cleanup(void)
{
    if (enc) EVP_CIPHER_CTX_free(enc);
    if (dec) EVP_CIPHER_CTX_free(dec);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (cipher_fetch) EVP_CIPHER_free(cipher_fetch);
    if (prov_legacy)  OSSL_PROVIDER_unload(prov_legacy);
    if (prov_default) OSSL_PROVIDER_unload(prov_default);
#endif
}

#else /* RAW / no crypto */
static void crypto_init(void) {}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{ (void)session_id; (void)seq; memcpy(dst, src, len); }
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{ (void)session_id; (void)seq; memcpy(dst, src, len); } /* ensures a read */
static void crypto_cleanup(void) {}
#endif /* backend */

/* ---------------- Main ---------------- */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <sender|receiver> <shm_device>\n", argv[0]);
        return 1;
    }

    bool is_sender = (strcmp(argv[1], "sender") == 0);
    const char *shm_path = argv[2];

    int fd = open(shm_path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }

    long ps = sysconf(_SC_PAGESIZE);
    size_t need = sizeof(struct shm_region);
    size_t need_rounded = (need + (size_t)ps - 1) & ~((size_t)ps - 1);

    if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) {
        if ((size_t)st.st_size < need_rounded) {
            fprintf(stderr, "Shared-memory region too small: have=%zu need=%zu\n",
                    (size_t)st.st_size, need_rounded);
            return 1;
        }
    }

    struct shm_region *shm =
        mmap(NULL, need_rounded, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    crypto_init();

    /* Warm-up both sides. */
    warmup_pages(shm->buf, BUF_LEN);
    memset(tmp, 0, MAX_PLAINTEXT);
    __sync_synchronize();

    /* Handshake only for crypto modes to share session_id (not timed). */
#if CRYPTO_ENABLED
    uint64_t session_id = 0, expected_session = 0;

    if (is_sender) {
        session_id = ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL);
        if (session_id == 0) session_id = 1;
        memcpy(shm->buf, &session_id, sizeof(session_id));
        atomic_store_explicit(&shm->seq, HS_SESSION, memory_order_release);
        wait_u64_eq(&shm->ack, HS_SESSION);
    } else {
        wait_u64_eq(&shm->seq, HS_SESSION);
        memcpy(&expected_session, shm->buf, sizeof(expected_session));
        if (expected_session == 0) expected_session = 1;
        atomic_store_explicit(&shm->ack, HS_SESSION, memory_order_release);
    }
    __sync_synchronize();
#else
    uint64_t session_id = 0, expected_session = 0; /* unused in raw */
#endif

    if (is_sender) {
        puts("size_bytes,avg_lat_us,throughput_MBps,avg_ticks,avg_pmu_cycles");
        fflush(stdout);
    }

    /* Detect PMU availability once. */
    uint64_t dummy;
    bool pmu_ok = pmu_read_cycle(&dummy);
    (void)rd_cntfrq(); /* warm path */

    for (size_t idx = 0; idx < NUM_SIZES; ++idx) {
        size_t msg_len = MSG_SIZES[idx];
        size_t pt_len  = HDR_LEN + msg_len;

        /* ---- start-of-size barrier (sender-driven) ---- */
        if (is_sender) {
            atomic_store_explicit(&shm->seq, SIZE_START, memory_order_release);
            wait_u64_eq(&shm->ack, SIZE_START);
        } else {
            wait_u64_eq(&shm->seq, SIZE_START);
            atomic_store_explicit(&shm->ack, SIZE_START, memory_order_release);
        }
        __sync_synchronize();

        uint64_t t_start = 0, t_end = 0;
        uint64_t tick_start = 0, tick_end = 0;
        uint64_t pmu_start = 0, pmu_end = 0;

        if (is_sender) {
            t_start    = now_ns();
            tick_start = rd_cntvct();
            if (pmu_ok) pmu_ok = pmu_read_cycle(&pmu_start);

            for (int i = 1; i <= ITERS; ++i) {
#if CRYPTO_ENABLED
                write_header(tmp, session_id, (uint64_t)i);
#endif
                memset(tmp + HDR_LEN, 0xA5, msg_len);

                crypto_encrypt(shm->buf, tmp, pt_len, session_id, (uint64_t)i);

                atomic_store_explicit(&shm->seq, i, memory_order_release);

                while (atomic_load_explicit(&shm->ack, memory_order_acquire) != (uint64_t)i)
                    cpu_relax();
            }

            t_end    = now_ns();
            tick_end = rd_cntvct();
            if (pmu_ok) pmu_ok = pmu_read_cycle(&pmu_end);

            /* ---- end-of-size barrier ---- */
            atomic_store_explicit(&shm->seq, SIZE_DONE, memory_order_release);
            wait_u64_eq(&shm->ack, SIZE_DONE);

            double sec       = (double)(t_end - t_start) / 1e9;
            double lat_us    = (sec / ITERS) * 1e6;
            double mbps      = ((double)msg_len * ITERS) / (1024.0 * 1024.0 * sec);
            double avg_ticks = (double)(tick_end - tick_start) / ITERS;
            double avg_pmu   = pmu_ok ? (double)(pmu_end - pmu_start) / ITERS : -1.0;

            printf("%zu,%.3f,%.2f,%.0f,%.0f\n",
                   msg_len, lat_us, mbps, avg_ticks, avg_pmu);
            fflush(stdout);

        } else { /* receiver */
            for (int i = 1; i <= ITERS; ++i) {
                while (atomic_load_explicit(&shm->seq, memory_order_acquire) != (uint64_t)i)
                    cpu_relax();

                crypto_decrypt(tmp, shm->buf, pt_len, expected_session, (uint64_t)i);

#if CRYPTO_ENABLED
                uint64_t rcv_session = 0, rcv_seq = 0;
                read_header(tmp, &rcv_session, &rcv_seq);
                if (rcv_seq != (uint64_t)i) return 5;
#endif
                atomic_store_explicit(&shm->ack, i, memory_order_release);
            }

            /* ---- end-of-size barrier ---- */
            atomic_store_explicit(&shm->ack, SIZE_DONE, memory_order_release);
            wait_u64_eq(&shm->seq, SIZE_DONE);
        }
    }

    crypto_cleanup();
    munmap(shm, need_rounded);
    close(fd);
    return 0;
}
