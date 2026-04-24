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

static unsigned char tmp[MAX_PLAINTEXT];

/* ---------------- Debug helpers ---------------- */
/* Logs are enabled by default; set SHM_DEBUG=0 to suppress. */
static int g_debug = 1;
static int g_trace = 0;
static uint64_t g_log_period_ns = 500ULL * 1000ULL * 1000ULL; /* default 500ms */

#define LOG(...)   do { fprintf(stderr, __VA_ARGS__); } while (0)
#define DBG(...)   do { if (g_debug) fprintf(stderr, __VA_ARGS__); } while (0)
#define TRACE(...) do { if (g_trace) fprintf(stderr, __VA_ARGS__); } while (0)

static void hexdump(const char* label, const void* buf, size_t len, size_t maxbytes)
{
    if (!g_debug) return;
    const unsigned char* p = (const unsigned char*)buf;
    size_t n = len < maxbytes ? len : maxbytes;
    fprintf(stderr, "%s (%zu bytes):", label, len);
    for (size_t i = 0; i < n; ++i) fprintf(stderr, " %02x", p[i]);
    if (n < len) fprintf(stderr, " ...");
    fputc('\n', stderr);
}

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

/* ---------------- Shared memory layout ---------------- */
struct shm_region {
    _Atomic uint64_t seq  __attribute__((aligned(64)));
    _Atomic uint64_t ack  __attribute__((aligned(64)));
    unsigned char    buf[BUF_LEN] __attribute__((aligned(64)));
};

/* ---------------- Timing helpers ---------------- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#if defined(__aarch64__)
static inline uint64_t rdcycle_ticks(void) { uint64_t c; asm volatile("mrs %0, cntvct_el0" : "=r"(c)); return c; }
#elif defined(__x86_64__)
static inline uint64_t rdcycle_ticks(void) { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
#else
static inline uint64_t rdcycle_ticks(void) { return now_ns(); }
#endif

static void fatal(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

/* ---------------- Warm-up ---------------- */
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

/* ---------------- IV derivation (unconditional) ---------------- */
static inline void make_gcm_iv(uint8_t iv[12], uint64_t session_id, uint64_t seq)
{
    uint32_t sid = (uint32_t)(session_id ^ 0xA5A5A5A5u);
    uint32_t sid_be = htobe32(sid);
    uint64_t seq_be = htobe64(seq);
    memcpy(iv,     &sid_be, 4);
    memcpy(iv + 4, &seq_be, 8);
}

/* ---------------- Wait with NO timeout (periodic logging) ---------------- */
static void wait_u64_eq(_Atomic uint64_t *addr,
                        uint64_t want,
                        const char *name,
                        const struct shm_region *shm,
                        int iter /* -1 for handshake */)
{
    uint64_t start = now_ns(), last = start;
    for (;;) {
        uint64_t cur = atomic_load_explicit(addr, memory_order_acquire);
        if (cur == want) return;
        uint64_t now = now_ns();
        if (g_debug && (now - last >= g_log_period_ns)) {
            fprintf(stderr, "[WAIT] iter=%d %s: want=%llu cur=%llu elapsed=%.3f ms  (seq=%llu ack=%llu)\n",
                    iter, name,
                    (unsigned long long)want,
                    (unsigned long long)cur,
                    (now - start) / 1e6,
                    (unsigned long long)atomic_load_explicit(&shm->seq, memory_order_relaxed),
                    (unsigned long long)atomic_load_explicit(&shm->ack, memory_order_relaxed));
            last = now;
        }
        cpu_relax();
    }
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
/* ---- OpenSSL with provider load + SIV/GCMSIV preferred; fallback to GCM ---- */
static EVP_CIPHER_CTX *enc = NULL, *dec = NULL;
static EVP_CIPHER     *cipher_fetch = NULL;
static const EVP_CIPHER *cipher_gcm = NULL;
static OSSL_PROVIDER  *prov_default = NULL, *prov_legacy = NULL;
static int use_gcm = 0;
static const unsigned char CONST_IV[12] = {0};

static void crypto_init(void)
{
    prov_default = OSSL_PROVIDER_load(NULL, "default");
    if (!prov_default) { fprintf(stderr, "OpenSSL: load default provider failed\n"); ERR_print_errors_fp(stderr); exit(EXIT_FAILURE); }
    prov_legacy  = OSSL_PROVIDER_load(NULL, "legacy"); /* ok if NULL */

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
    if (prov_legacy)  OSSL_PROVIDER_unload(prov_legacy);
    if (prov_default) OSSL_PROVIDER_unload(prov_default);
#endif
}

#else /* RAW / no crypto */
static void crypto_init(void) { DBG("[DBG] Backend: RAW (no crypto)\n"); }
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
    /* Unbuffer debug stream so logs appear immediately */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Allow disabling logs / enabling per-iteration via env */
    if (getenv("SHM_DEBUG") && atoi(getenv("SHM_DEBUG")) == 0) g_debug = 0;
    if (getenv("SHM_TRACE"))  g_trace = atoi(getenv("SHM_TRACE")) != 0;
    if (getenv("SHM_LOG_MS")) {
        long ms = strtol(getenv("SHM_LOG_MS"), NULL, 10);
        if (ms > 0) g_log_period_ns = (uint64_t)ms * 1000000ULL;
    }

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <sender|receiver> <shm_device> [-v]\n", argv[0]);
        return 1;
    }
    if (argc >= 4 && strcmp(argv[3], "-v") == 0) g_debug = 1;

    bool is_sender = (strcmp(argv[1], "sender") == 0);
    const char *shm_path = argv[2];

    int cpu = -1;
#if defined(__linux__)
#ifdef __GLIBC__
    cpu = sched_getcpu();
#endif
#endif
    LOG("[INFO] role=%s pid=%ld cpu=%d device=%s\n",
        is_sender ? "sender" : "receiver", (long)getpid(), cpu, shm_path);

    LOG("[INFO] opening device...\n");
    int fd = open(shm_path, O_RDWR);
    if (fd < 0) fatal("open");
    LOG("[INFO] open OK (fd=%d)\n", fd);

    LOG("[INFO] fstat...\n");
    struct stat st;
    if (fstat(fd, &st) < 0) fatal("fstat");
    LOG("[INFO] fstat mode=0%o size=%zu\n", st.st_mode & 0777, (size_t)st.st_size);

    long ps = sysconf(_SC_PAGESIZE);
    size_t need = sizeof(struct shm_region);
    size_t need_rounded = (need + (size_t)ps - 1) & ~((size_t)ps - 1);

    if (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)) {
        if ((size_t)st.st_size < need_rounded) {
            fprintf(stderr,
                    "Shared-memory region too small: have=%zu need=%zu (rounded=%zu)\n",
                    (size_t)st.st_size, need, need_rounded);
            return 1;
        }
    } else {
        LOG("[INFO] char device: st_size=%zu, will map %zu\n", (size_t)st.st_size, need_rounded);
    }

    LOG("[INFO] mmap %zu bytes...\n", need_rounded);
    struct shm_region *shm =
        mmap(NULL, need_rounded, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) fatal("mmap");
    LOG("[INFO] mmap OK: seq@%p ack@%p buf@%p\n", (void*)&shm->seq, (void*)&shm->ack, (void*)shm->buf);

    crypto_init();

    LOG("[INFO] warmup start\n");
    warmup_pages(shm->buf, BUF_LEN);
    memset(tmp, 0, MAX_PLAINTEXT);
    __sync_synchronize();
    LOG("[INFO] warmup done (BUF_LEN=%u)\n", (unsigned)BUF_LEN);

    /* ---- Session handshake (crypto only) ---- */
#if CRYPTO_ENABLED
    uint64_t session_id = 0, expected_session = 0;

    if (is_sender) {
        session_id = ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL);
        if (session_id == 0) session_id = 1;
        LOG("[HSK-SND] publish session_id=0x%016llx\n", (unsigned long long)session_id);
        memcpy(shm->buf, &session_id, sizeof(session_id));
        hexdump("[HSK-SND] buf[0..16]", shm->buf, 16, 16);
        LOG("[HSK-SND] set seq=UINT64_MAX and wait ack...\n");
        atomic_store_explicit(&shm->seq, UINT64_MAX, memory_order_release);
        wait_u64_eq(&shm->ack, UINT64_MAX, "ack(handshake)", shm, -1);
        LOG("[HSK-SND] ack received, reset seq/ack\n");
        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->ack, 0, memory_order_relaxed);
    } else {
        LOG("[HSK-RCV] wait for seq=UINT64_MAX...\n");
        wait_u64_eq(&shm->seq, UINT64_MAX, "seq(handshake)", shm, -1);
        memcpy(&expected_session, shm->buf, sizeof(expected_session));
        LOG("[HSK-RCV] latched session_id=0x%016llx\n", (unsigned long long)expected_session);
        hexdump("[HSK-RCV] buf[0..16]", shm->buf, 16, 16);
        LOG("[HSK-RCV] set ack=UINT64_MAX\n");
        atomic_store_explicit(&shm->ack, UINT64_MAX, memory_order_release);
        LOG("[HSK-RCV] reset seq/ack\n");
        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->ack, 0, memory_order_relaxed);
    }
    __sync_synchronize();
#else
    uint64_t session_id = 0, expected_session = 0; /* unused in raw */
    LOG("[INFO] RAW mode (no handshake)\n");
#endif

    if (is_sender) {
        puts("size_bytes,avg_lat_us,throughput_MBps,avg_cycles");
        fflush(stdout);
    }

    for (size_t idx = 0; idx < NUM_SIZES; ++idx) {
        size_t msg_len = MSG_SIZES[idx];
        size_t pt_len  = HDR_LEN + msg_len;

        LOG("[LOOP-%s] start size=%zu pt_len=%zu ITERS=%d\n",
            is_sender ? "SND" : "RCV", msg_len, pt_len, ITERS);

        atomic_store_explicit(&shm->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&shm->ack, 0, memory_order_relaxed);
        __sync_synchronize();

        uint64_t t_start = 0, c_start = 0;

        if (is_sender) {
            t_start = now_ns();
            c_start = rdcycle_ticks();

            for (int i = 1; i <= ITERS; ++i) {
#if CRYPTO_ENABLED
                write_header(tmp, session_id, (uint64_t)i);
#endif
                memset(tmp + HDR_LEN, 0xA5, msg_len);

                TRACE("[SND] i=%d: PT ready, enc/copy -> shm\n", i);
                crypto_encrypt(shm->buf, tmp, pt_len, session_id, (uint64_t)i);

                if (g_debug && i == 1) {
                    hexdump("[SND] CT first bytes", shm->buf, pt_len, 48);
                    hexdump("[SND] TAG", shm->buf + pt_len, TAG_LEN, TAG_LEN);
                }

                atomic_store_explicit(&shm->seq, i, memory_order_release);
                TRACE("[SND] i=%d: seq=%d set, waiting ack...\n", i, i);

                if (g_trace || i == 1) {
                    wait_u64_eq(&shm->ack, (uint64_t)i, "ack(loop)", shm, i);
                    TRACE("[SND] i=%d: ack received\n", i);
                } else {
                    while (atomic_load_explicit(&shm->ack, memory_order_acquire) != (uint64_t)i)
                        cpu_relax();
                }
            }

            uint64_t t_end = now_ns();
            uint64_t c_end = rdcycle_ticks();
            double sec     = (double)(t_end - t_start) / 1e9;
            double lat_us  = (sec / ITERS) * 1e6;
            double mbps    = ((double)msg_len * ITERS) / (1024.0 * 1024.0 * sec);
            double cycles  = (double)(c_end - c_start) / ITERS;
            LOG("[LOOP-SND] done size=%zu: lat_us=%.3f MBps=%.2f cycles=%.0f\n",
                msg_len, lat_us, mbps, cycles);
            printf("%zu,%.3f,%.2f,%.0f\n", msg_len, lat_us, mbps, cycles);
            fflush(stdout);

        } else { /* receiver */
            for (int i = 1; i <= ITERS; ++i) {
                if (g_trace || i == 1) {
                    wait_u64_eq(&shm->seq, (uint64_t)i, "seq(loop)", shm, i);
                    TRACE("[RCV] i=%d: seq observed\n", i);
                } else {
                    while (atomic_load_explicit(&shm->seq, memory_order_acquire) != (uint64_t)i)
                        cpu_relax();
                }

                crypto_decrypt(tmp, shm->buf, pt_len, expected_session, (uint64_t)i);
                TRACE("[RCV] i=%d: decrypted/copied\n", i);

                if (g_debug && i == 1) hexdump("[RCV] PT first bytes", tmp, pt_len, 48);

#if CRYPTO_ENABLED
                uint64_t rcv_session = 0, rcv_seq = 0;
                read_header(tmp, &rcv_session, &rcv_seq);
                if (rcv_seq != (uint64_t)i) {
                    fprintf(stderr, "Sequence mismatch (got %llu, want %d)\n",
                            (unsigned long long)rcv_seq, i);
                    return 5;
                }
                TRACE("[RCV] i=%d: header ok (session=%llx seq=%llu)\n",
                      i, (unsigned long long)rcv_session, (unsigned long long)rcv_seq);
#endif
                atomic_store_explicit(&shm->ack, i, memory_order_release);
                TRACE("[RCV] i=%d: ack set\n", i);
            }
            LOG("[LOOP-RCV] done size=%zu\n", msg_len);
        }
    }

    LOG("[INFO] cleanup\n");
    crypto_cleanup();
    munmap(shm, need_rounded);
    close(fd);
    LOG("[INFO] exit\n");
    return 0;
}
