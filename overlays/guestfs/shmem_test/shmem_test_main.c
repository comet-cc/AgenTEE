#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

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

/* ---------------- Crypto backend selection ---------------- */
#if (defined(USE_MBEDTLS) + defined(USE_KCAPI) + defined(USE_OPENSSL)) > 1
#error "Select only one crypto backend"
#endif

#if defined(USE_MBEDTLS) || defined(USE_KCAPI) || defined(USE_OPENSSL)
#define CRYPTO_ENABLED 1
#else
#define CRYPTO_ENABLED 0
#endif

#if defined(USE_MBEDTLS)
#include <mbedtls/gcm.h>
#elif defined(USE_KCAPI)
#include <kcapi.h>
#elif defined(USE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/provider.h>
#endif

/* ---------------- Tunables ---------------- */
static const size_t MSG_SIZES[] = {64, 128, 256, 512, 1024, 2048,
                                   4096, 8192, 16384, 32768};
#define NUM_SIZES (sizeof(MSG_SIZES) / sizeof(MSG_SIZES[0]))
#define ITERS 1000

#if CRYPTO_ENABLED
#define TAG_LEN 16
#define HDR_LEN 16     /* 8 B session_id + 8 B seq */
#else
#define TAG_LEN 0
#define HDR_LEN 0
#endif

#define MAX_MSG       1048576
#define MAX_PLAINTEXT (HDR_LEN + MAX_MSG)
#define BUF_LEN       (MAX_PLAINTEXT + TAG_LEN)

static unsigned char tmp[MAX_PLAINTEXT];

/* ---------------- Logging / utils ---------------- */
static int g_debug = 1;
static int g_trace = 0;

#define LOG(...)   do { fprintf(stderr, __VA_ARGS__); } while (0)
#define DBG(...)   do { if (g_debug) fprintf(stderr, __VA_ARGS__); } while (0)
#define TRACE(...) do { if (g_trace) fprintf(stderr, __VA_ARGS__); } while (0)

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

static void fatal(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static void log_open_context(const char *path)
{
    struct stat st;
    LOG("[OPEN] target=\"%s\"\n", path);
    LOG("[OPEN] uid=%ld gid=%ld\n", (long)getuid(), (long)getgid());
    if (access(path, F_OK) != 0) {
        LOG("[OPEN] exists: NO (errno=%d: %s)\n", errno, strerror(errno));
        return;
    }
    LOG("[OPEN] exists: yes\n");
    if (stat(path, &st) == 0) {
        char t = S_ISCHR(st.st_mode)?'c':
                 S_ISBLK(st.st_mode)?'b':
                 S_ISREG(st.st_mode)?'-':
                 S_ISLNK(st.st_mode)?'l':'?';
        LOG("[OPEN] mode=%c%o owner=%ld:%ld size=%lld\n",
            t, (unsigned)(st.st_mode & 0777),
            (long)st.st_uid, (long)st.st_gid,
            (long long)st.st_size);
    }
}

/* ---------------- Timing helpers ---------------- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#if defined(__aarch64__)
static inline uint64_t rdcycle_ticks(void)
{ uint64_t c; asm volatile("mrs %0, pmccntr_el0":"=r"(c)); return c; }
#elif defined(__x86_64__)
static inline uint64_t rdcycle_ticks(void)
{ uint32_t lo,hi; asm volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
#else
static inline uint64_t rdcycle_ticks(void) { return now_ns(); }
#endif

/* ---------------- Shared memory layout (normal cacheable) ---------------- */
struct shm_region {
    _Atomic uint32_t seq  __attribute__((aligned(64)));
    _Atomic uint32_t ack  __attribute__((aligned(64)));
    unsigned char    buf[BUF_LEN] __attribute__((aligned(64)));
};

/* ---------------- Warm-up ---------------- */
static inline void warmup_pages(volatile unsigned char *p, size_t len)
{
    const size_t ps = 4096;
    for (size_t off = 0; off < len; off += ps) (void)p[off];
}

/* ---------------- Crypto helpers ---------------- */
#if CRYPTO_ENABLED
static inline void write_header(unsigned char *dst,
                                uint64_t session_id, uint64_t seq)
{
    memcpy(dst,             &session_id, sizeof(session_id));
    memcpy(dst + 8,         &seq,        sizeof(seq));
}
static inline void read_header(const unsigned char *src,
                               uint64_t *session_id, uint64_t *seq)
{
    memcpy(session_id, src,     sizeof(*session_id));
    memcpy(seq,        src + 8, sizeof(*seq));
}
#endif

static inline void make_gcm_iv(uint8_t iv[12],
                               uint64_t session_id, uint64_t seq)
{
    uint32_t sid    = (uint32_t)(session_id ^ 0xA5A5A5A5u);
    uint32_t sid_be = htobe32(sid);
    uint64_t seq_be = htobe64(seq);
    memcpy(iv, &sid_be, 4);
    memcpy(iv + 4, &seq_be, 8);
}

#if CRYPTO_ENABLED
static const unsigned char key[32] = {
    0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,
    0x81,0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
};
#endif

/* API (implemented per backend below) */
static void crypto_init(void);
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq);
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq);
static void crypto_cleanup(void);

#if defined(USE_MBEDTLS)
/* ---- mbedTLS AES-256-GCM ---- */
static mbedtls_gcm_context gcm_enc, gcm_dec;
static void crypto_init(void)
{
    mbedtls_gcm_init(&gcm_enc);
    mbedtls_gcm_init(&gcm_dec);
    if (mbedtls_gcm_setkey(&gcm_enc, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) fatal("gcm_setkey(enc)");
    if (mbedtls_gcm_setkey(&gcm_dec, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) fatal("gcm_setkey(dec)");
    DBG("[DBG] Backend: MbedTLS AES-256-GCM (IV=f(session,seq))\n");
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    unsigned char *ct  = dst;
    unsigned char *tag = dst + len;
    if (mbedtls_gcm_crypt_and_tag(&gcm_enc, MBEDTLS_GCM_ENCRYPT, len,
                                 iv, sizeof(iv), NULL, 0, src, ct, TAG_LEN, tag) != 0)
        fatal("gcm_encrypt");
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    const unsigned char *ct  = src;
    const unsigned char *tag = src + len;
    if (mbedtls_gcm_auth_decrypt(&gcm_dec, len, iv, sizeof(iv), NULL, 0, tag, TAG_LEN, ct, dst) != 0) {
        fprintf(stderr, "Auth failure\n"); exit(EXIT_FAILURE);
    }
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
    if (kcapi_aead_init(&aead, "gcm(aes)", 0) < 0) fatal("kcapi_init");
    if (kcapi_aead_setkey(aead, key, sizeof key) < 0) fatal("kcapi_setkey");
    DBG("[DBG] Backend: libkcapi AES-256-GCM (IV=f(session,seq))\n");
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    ssize_t r = kcapi_aead_encrypt(aead, src, len, iv, dst, len + TAG_LEN, KCAPI_ACCESS_HEURISTIC);
    if (r < 0 || (size_t)r != len + TAG_LEN) fatal("kcapi_encrypt");
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    uint8_t iv[12]; make_gcm_iv(iv, session_id, seq);
    ssize_t r = kcapi_aead_decrypt(aead, src, len + TAG_LEN, iv, dst, len, KCAPI_ACCESS_HEURISTIC);
    if (r < 0 || (size_t)r != len) { fprintf(stderr, "kcapi decrypt fail (%zd)\n", r); exit(EXIT_FAILURE); }
}
static void crypto_cleanup(void) { kcapi_aead_destroy(aead); }

#elif defined(USE_OPENSSL)
/* ---- OpenSSL (prefers GCM-SIV/SIV; falls back to GCM) ---- */
static EVP_CIPHER_CTX *enc = NULL, *dec = NULL;
static EVP_CIPHER *cipher_fetch = NULL;
static const EVP_CIPHER *cipher_gcm = NULL;
static OSSL_PROVIDER *prov_default = NULL, *prov_legacy = NULL;
static int use_gcm = 0;
static const unsigned char CONST_IV[12] = {0};

static void crypto_init(void)
{
    prov_default = OSSL_PROVIDER_load(NULL, "default");
    if (!prov_default) { fprintf(stderr, "OpenSSL: load default provider failed\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE); }
    prov_legacy = OSSL_PROVIDER_load(NULL, "legacy"); /* ok if NULL */

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    cipher_fetch = EVP_CIPHER_fetch(NULL, "AES-256-GCM-SIV", NULL);
    if (!cipher_fetch) cipher_fetch = EVP_CIPHER_fetch(NULL, "AES-256-SIV", NULL);
#endif
    if (!cipher_fetch) {
        cipher_gcm = EVP_aes_256_gcm();
        if (!cipher_gcm) { fprintf(stderr, "OpenSSL: need SIV or GCM\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE); }
        use_gcm = 1;
        DBG("[DBG] Backend: OpenSSL AES-256-GCM (fallback)\n");
    } else {
        DBG("[DBG] Backend: OpenSSL AES-256-GCM-SIV/SIV\n");
    }
    enc = EVP_CIPHER_CTX_new(); dec = EVP_CIPHER_CTX_new();
    if (!enc || !dec) { fprintf(stderr, "OpenSSL ctx alloc failed\n"); exit(EXIT_FAILURE); }
}
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    int outl = 0, tmp2 = 0;
    if (!use_gcm) {
        if (EVP_EncryptInit_ex(enc, cipher_fetch, NULL, NULL, NULL) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(CONST_IV), NULL) != 1) goto err;
        if (EVP_EncryptInit_ex(enc, NULL, NULL, key, CONST_IV) != 1) goto err;
        if (EVP_EncryptUpdate(enc, dst, &outl, src, (int)len) != 1) goto err;
        if (EVP_EncryptFinal_ex(enc, dst + outl, &tmp2) != 1) goto err;
        outl += tmp2;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, dst + len) != 1) goto err;
        if (outl != (int)len) goto err;
        return;
    } else {
        unsigned char iv[12]; make_gcm_iv(iv, session_id, seq);
        if (EVP_EncryptInit_ex(enc, cipher_gcm, NULL, NULL, NULL) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(iv), NULL) != 1) goto err;
        if (EVP_EncryptInit_ex(enc, NULL, NULL, key, iv) != 1) goto err;
        if (EVP_EncryptUpdate(enc, dst, &outl, src, (int)len) != 1) goto err;
        if (EVP_EncryptFinal_ex(enc, dst + outl, &tmp2) != 1) goto err;
        outl += tmp2;
        if (EVP_CIPHER_CTX_ctrl(enc, EVP_CTRL_AEAD_GET_TAG, TAG_LEN, dst + len) != 1) goto err;
        if (outl != (int)len) goto err;
        return;
    }
err:
    fprintf(stderr, "OpenSSL encrypt error\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE);
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           uint64_t session_id, uint64_t seq)
{
    int outl = 0, tmp2 = 0;
    if (!use_gcm) {
        if (EVP_DecryptInit_ex(dec, cipher_fetch, NULL, NULL, NULL) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(CONST_IV), NULL) != 1) goto err;
        if (EVP_DecryptInit_ex(dec, NULL, NULL, key, CONST_IV) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, (void*)(src + len)) != 1) goto err;
        if (EVP_DecryptUpdate(dec, dst, &outl, src, (int)len) != 1) goto authfail;
        if (EVP_DecryptFinal_ex(dec, dst + outl, &tmp2) != 1) goto authfail;
        outl += tmp2;
        if (outl != (int)len) goto err;
        return;
    } else {
        unsigned char iv[12]; make_gcm_iv(iv, session_id, seq);
        if (EVP_DecryptInit_ex(dec, cipher_gcm, NULL, NULL, NULL) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_IVLEN, (int)sizeof(iv), NULL) != 1) goto err;
        if (EVP_DecryptInit_ex(dec, NULL, NULL, key, iv) != 1) goto err;
        if (EVP_CIPHER_CTX_ctrl(dec, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, (void*)(src + len)) != 1) goto err;
        if (EVP_DecryptUpdate(dec, dst, &outl, src, (int)len) != 1) goto authfail;
        if (EVP_DecryptFinal_ex(dec, dst + outl, &tmp2) != 1) goto authfail;
        outl += tmp2;
        if (outl != (int)len) goto err;
        return;
    }
authfail:
    fprintf(stderr, "OpenSSL auth failure\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE);
err:
    fprintf(stderr, "OpenSSL decrypt error\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE);
}
static void crypto_cleanup(void)
{
    if (enc) EVP_CIPHER_CTX_free(enc);
    if (dec) EVP_CIPHER_CTX_free(dec);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (cipher_fetch) EVP_CIPHER_free(cipher_fetch);
    if (prov_legacy) OSSL_PROVIDER_unload(prov_legacy);
    if (prov_default) OSSL_PROVIDER_unload(prov_default);
#endif
}
#else /* RAW / no crypto */
static void crypto_init(void) { DBG("[DBG] Backend: RAW (no crypto)\n"); }
static void crypto_encrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           __attribute__((unused)) uint64_t session_id,
                           __attribute__((unused)) uint64_t seq) {
    memcpy(dst, src, len);
}
static void crypto_decrypt(unsigned char *dst, const unsigned char *src, size_t len,
                           __attribute__((unused)) uint64_t session_id,
                           __attribute__((unused)) uint64_t seq) {
    memcpy(dst, src, len);
}
static void crypto_cleanup(void) {}
#endif

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* ---------------- Main ---------------- */
int main(int argc, char *argv[])
{
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <sender|receiver> <shm_device> [-v]\n", argv[0]);
        return 1;
    }

    bool  is_sender = (strcmp(argv[1], "sender") == 0);
    const char *shm_path = argv[2];
    if (argc >= 4 && strcmp(argv[3], "-v") == 0) g_trace = 1;

    LOG("[INFO] opening device...\n");
    log_open_context(shm_path);
    int fd = open(shm_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG("[OPEN] open(\"%s\") failed: %s\n", shm_path, strerror(errno));
        fatal("open");
    }
    LOG("[OPEN] success: fd=%d\n", fd);

    /* Determine window size via llseek (driver supports it) */
    off_t devsz = lseek(fd, 0, SEEK_END);
    if (devsz < 0) fatal("lseek(SEEK_END)");
    if (lseek(fd, 0, SEEK_SET) < 0) fatal("lseek(SEEK_SET)");

    size_t map_len = sizeof(struct shm_region);
    if ((off_t)map_len > devsz) {
        fprintf(stderr, "[ERR] device window too small: need %zu bytes, have %lld\n",
                map_len, (long long)devsz);
        return 1;
    }

    LOG("[INFO] mmap %zu bytes...\n", map_len);
    struct shm_region *shm = mmap(NULL, map_len,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  fd, 0);
    if (shm == MAP_FAILED) fatal("mmap");
    LOG("[MMAP] addr=%p len=%zu prot=RW shared\n", (void *)shm, map_len);

    crypto_init();

    /* Touch pages to fault them in (only within mapped length) */
    warmup_pages(shm->buf, BUF_LEN);
    memset(tmp, 0, MAX_PLAINTEXT);

#if CRYPTO_ENABLED
    const uint64_t session_id       = 0x1122334455667788ULL;
    const uint64_t expected_session = session_id;
#else
    const uint64_t session_id       = 0;
    const uint64_t expected_session = 0;
#endif

    if (is_sender) {
        puts("size_bytes,median_lat_us,throughput_MBps,median_cycles");
        fflush(stdout);
    }

    for (size_t idx = 0; idx < NUM_SIZES; ++idx) {
        size_t msg_len = MSG_SIZES[idx];
        size_t pt_len  = HDR_LEN + msg_len;

        /* ------------- PHASE 1: throughput ---------------- */
        /* Reset doorbells */
        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->ack, 0, memory_order_relaxed);

        double mbps = 0.0;
        if (is_sender) {
            uint64_t t_start = now_ns();
            for (uint32_t i = 1; i <= ITERS; ++i) {
#if CRYPTO_ENABLED
                write_header(tmp, session_id, (uint64_t)i);
#endif
                memset(tmp + HDR_LEN, 0xA5, msg_len);
                crypto_encrypt(shm->buf, tmp, pt_len, session_id, (uint64_t)i);

                /* Publish payload */
                atomic_store_explicit(&shm->seq, i, memory_order_release);

                /* Wait for consumer */
                while (atomic_load_explicit(&shm->ack, memory_order_acquire) != i)
                    cpu_relax();
            }
            uint64_t t_end = now_ns();
            double sec = (double)(t_end - t_start) / 1e9;
            mbps = ((double)msg_len * ITERS) / (1024.0 * 1024.0 * sec);
        } else { /* receiver */
            for (uint32_t i = 1; i <= ITERS; ++i) {
                while (atomic_load_explicit(&shm->seq, memory_order_acquire) != i)
                    cpu_relax();

                crypto_decrypt(tmp, shm->buf, pt_len, expected_session, (uint64_t)i);

                atomic_store_explicit(&shm->ack, i, memory_order_release);
            }
        }

        /* ------------- PHASE 2: latency ------------------- */
        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->ack, 0, memory_order_relaxed);

        if (is_sender) {
            uint64_t lat_ns[ITERS], lat_cycles[ITERS];

            for (uint32_t i = 1; i <= ITERS; ++i) {
                uint32_t seq = i;

                uint64_t t0 = now_ns();
                uint64_t c0 = rdcycle_ticks();

#if CRYPTO_ENABLED
                write_header(tmp, session_id, (uint64_t)seq);
#endif
                memset(tmp + HDR_LEN, 0xA5, msg_len);
                crypto_encrypt(shm->buf, tmp, pt_len, session_id, (uint64_t)seq);

                atomic_store_explicit(&shm->seq, seq, memory_order_release);

                while (atomic_load_explicit(&shm->ack, memory_order_acquire) != seq)
                    cpu_relax();

                uint64_t t1 = now_ns();
                uint64_t c1 = rdcycle_ticks();
                lat_ns[i-1]     = t1 - t0;
                lat_cycles[i-1] = c1 - c0;
            }

            qsort(lat_ns,     ITERS, sizeof(uint64_t), compare_u64);
            qsort(lat_cycles, ITERS, sizeof(uint64_t), compare_u64);

            uint64_t med_ns =
                (ITERS & 1) ? lat_ns[ITERS/2]
                            : (lat_ns[ITERS/2-1] + lat_ns[ITERS/2]) / 2;
            uint64_t med_cy =
                (ITERS & 1) ? lat_cycles[ITERS/2]
                            : (lat_cycles[ITERS/2-1] + lat_cycles[ITERS/2]) / 2;

            printf("%zu,%.3f,%.2f,%.0f\n",
                   msg_len,
                   (double)med_ns / 1000.0,  /* µs */
                   mbps,
                   (double)med_cy);
            fflush(stdout);
        } else { /* receiver */
            for (uint32_t i = 1; i <= ITERS; ++i) {
                while (atomic_load_explicit(&shm->seq, memory_order_acquire) != i)
                    cpu_relax();

                crypto_decrypt(tmp, shm->buf, pt_len, expected_session, (uint64_t)i);

                atomic_store_explicit(&shm->ack, i, memory_order_release);
            }
        }
    }

    crypto_cleanup();
    munmap(shm, map_len);
    close(fd);
    return 0;
}
