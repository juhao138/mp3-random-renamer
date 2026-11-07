/*
 * mp3rn.c —— MP3 随机数字改名器（C 实现，单文件、无第三方依赖）
 *
 * 与 Python 版行为一致，并且共用同一个状态文件
 *   .mp3_random_renamer_state.json
 * 所以两个版本可以在同一个音乐库上混着用。
 *
 * 功能
 *   · 把 mp3 文件名加上不重复的随机数字（原名保留 / 只留数字两种模式）
 *   · 已改过名的文件默认跳过；--reprocess 可以重新编号
 *   · --restore 恢复原名：优先用原名记录，其次用内容指纹，最后才按前缀推断
 *
 * 构建
 *   zig cc -O2 -std=c11 -o mp3rn.exe mp3rn.c            (Windows)
 *   cc -O2 -std=c11 -o mp3rn mp3rn.c                     (Linux / Termux / Android NDK)
 *
 * 用法见 --help
 */

/* 特性宏必须在任何头文件之前定义：-std=c11 下 localtime_r / pread / nanosleep
   属于 POSIX 扩展，不声明这三个宏会报 implicit declaration。 */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define MP3_SUFFIX      ".mp3"
#define STATE_NAME      ".mp3_random_renamer_state.json"
#define DEFAULT_DIGITS  9
#define DEFAULT_INTERVAL 1.0
#define DEFAULT_SEP     "_"
#define RENAME_RETRIES  6
#define MAX_NAME_LEN    255
#define MIN_LEGACY_DIGITS 6
#define FP_WINDOW       (512 * 1024)

/* ------------------------------------------------------------------ 基础工具 */

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("错误: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("内存不足（%zu 字节）", n);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("内存不足（%zu 字节）", n);
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *fmt(const char *f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return xstrdup(""); }
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, f, ap2);
    va_end(ap2);
    return p;
}

/* 可增长字符串缓冲 */
typedef struct {
    char  *data;
    size_t len, cap;
} Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

static void buf_putn(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
static void buf_putc(Buf *b, char c) { buf_putn(b, &c, 1); }
static void buf_printf(Buf *b, const char *f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n > 0) {
        buf_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, f, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

static void buf_free(Buf *b) { free(b->data); buf_init(b); }

/* ------------------------------------------------------------------ 输出 */

static int g_quiet = 0;

/* 图形界面会把日志接走：level 0=普通 1=警告，text 是已经格式化好的正文 */
static void (*g_log_sink)(int level, const char *text) = NULL;

static void logmsg(const char *level, const char *fmt2, ...) {
    char stamp[16];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tmv);

    int is_warn = strcmp(level, "warn") == 0;

    Buf b;
    buf_init(&b);
    buf_puts(&b, "");
    va_list ap;
    va_start(ap, fmt2);
    {
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(NULL, 0, fmt2, ap);
        if (n > 0) {
            buf_reserve(&b, (size_t)n);
            vsnprintf(b.data + b.len, (size_t)n + 1, fmt2, ap2);
            b.len += (size_t)n;
        }
        va_end(ap2);
    }
    va_end(ap);

    if (g_log_sink) {
        g_log_sink(is_warn, b.data ? b.data : "");
        buf_free(&b);
        return;
    }

    FILE *out = is_warn ? stderr : stdout;
    fprintf(out, "[%s] %s%s\n", stamp, is_warn ? "[警告] " : "", b.data ? b.data : "");
    fflush(out);
    buf_free(&b);
}

static void loginfo(const char *f, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n < 0) return;
    char *s = xmalloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(s, (size_t)n + 1, f, ap);
    va_end(ap);
    logmsg("info", "%s", s);
    free(s);
}

static void logwarn(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n < 0) return;
    char *s = xmalloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(s, (size_t)n + 1, f, ap);
    va_end(ap);
    logmsg("warn", "%s", s);
    free(s);
}

/* ------------------------------------------------------------------ 集合 */

static uint64_t hash_u64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return hash_u64(h);
}

/* 数字集合（开放寻址） */
typedef struct {
    uint64_t *keys;
    unsigned char *st;   /* 0 空, 1 占用, 2 墓碑 */
    size_t cap, n;
} U64Set;

static void u64_init(U64Set *s, size_t cap) {
    if (cap < 16) cap = 16;
    s->cap = cap; s->n = 0;
    s->keys = xmalloc(sizeof(uint64_t) * cap);
    s->st = calloc(cap, 1);
    if (!s->st) die("内存不足");
}

static void u64_grow(U64Set *s);

static int u64_add(U64Set *s, uint64_t k) {
    if ((s->n + 1) * 10 >= s->cap * 7) u64_grow(s);
    size_t i = (size_t)(hash_u64(k) & (s->cap - 1));
    size_t first_tomb = (size_t)-1;
    for (;;) {
        if (s->st[i] == 0) {
            if (first_tomb != (size_t)-1) i = first_tomb;
            s->keys[i] = k; s->st[i] = 1; s->n++;
            return 0;
        }
        if (s->st[i] == 2) {
            if (first_tomb == (size_t)-1) first_tomb = i;
        } else if (s->keys[i] == k) {
            return 1;
        }
        i = (i + 1) & (s->cap - 1);
    }
}

static int u64_has(const U64Set *s, uint64_t k) {
    size_t i = (size_t)(hash_u64(k) & (s->cap - 1));
    for (;;) {
        if (s->st[i] == 0) return 0;
        if (s->st[i] == 1 && s->keys[i] == k) return 1;
        i = (i + 1) & (s->cap - 1);
    }
}

static void u64_del(U64Set *s, uint64_t k) {
    size_t i = (size_t)(hash_u64(k) & (s->cap - 1));
    for (;;) {
        if (s->st[i] == 0) return;
        if (s->st[i] == 1 && s->keys[i] == k) { s->st[i] = 2; s->n--; return; }
        i = (i + 1) & (s->cap - 1);
    }
}

static void u64_grow(U64Set *s) {
    U64Set ns;
    u64_init(&ns, s->cap * 2);
    for (size_t i = 0; i < s->cap; i++)
        if (s->st[i] == 1) u64_add(&ns, s->keys[i]);
    free(s->keys); free(s->st);
    *s = ns;
}

static void u64_free(U64Set *s) { free(s->keys); free(s->st); s->keys = NULL; s->st = NULL; s->cap = s->n = 0; }

/* 字符串集合 */
typedef struct {
    char **keys;
    unsigned char *st;
    size_t cap, n;
} StrSet;

static void ss_init(StrSet *s, size_t cap) {
    if (cap < 16) cap = 16;
    s->cap = cap; s->n = 0;
    s->keys = xmalloc(sizeof(char *) * cap);
    s->st = calloc(cap, 1);
    if (!s->st) die("内存不足");
}

static void ss_grow(StrSet *s);

static int ss_add(StrSet *s, const char *k) {
    if ((s->n + 1) * 10 >= s->cap * 7) ss_grow(s);
    size_t i = (size_t)(hash_str(k) & (s->cap - 1));
    size_t first_tomb = (size_t)-1;
    for (;;) {
        if (s->st[i] == 0) {
            if (first_tomb != (size_t)-1) i = first_tomb;
            s->keys[i] = xstrdup(k); s->st[i] = 1; s->n++;
            return 0;
        }
        if (s->st[i] == 2) {
            if (first_tomb == (size_t)-1) first_tomb = i;
        } else if (strcmp(s->keys[i], k) == 0) {
            return 1;
        }
        i = (i + 1) & (s->cap - 1);
    }
}

static int ss_has(const StrSet *s, const char *k) {
    size_t i = (size_t)(hash_str(k) & (s->cap - 1));
    for (;;) {
        if (s->st[i] == 0) return 0;
        if (s->st[i] == 1 && strcmp(s->keys[i], k) == 0) return 1;
        i = (i + 1) & (s->cap - 1);
    }
}

static void ss_del(StrSet *s, const char *k) {
    size_t i = (size_t)(hash_str(k) & (s->cap - 1));
    for (;;) {
        if (s->st[i] == 0) return;
        if (s->st[i] == 1 && strcmp(s->keys[i], k) == 0) {
            free(s->keys[i]); s->keys[i] = NULL; s->st[i] = 2; s->n--;
            return;
        }
        i = (i + 1) & (s->cap - 1);
    }
}

static void ss_grow(StrSet *s) {
    StrSet ns;
    ss_init(&ns, s->cap * 2);
    for (size_t i = 0; i < s->cap; i++)
        if (s->st[i] == 1) { ss_add(&ns, s->keys[i]); free(s->keys[i]); }
    free(s->keys); free(s->st);
    *s = ns;
}

static void ss_free(StrSet *s) {
    for (size_t i = 0; i < s->cap; i++)
        if (s->st[i] == 1) free(s->keys[i]);
    free(s->keys); free(s->st);
    s->keys = NULL; s->st = NULL; s->cap = s->n = 0;
}

/* 字符串 -> 字符串映射（orig / fps 用） */
typedef struct {
    char **keys, **vals;
    unsigned char *st;
    size_t cap, n;
} StrMap;

static void sm_init(StrMap *m, size_t cap) {
    if (cap < 16) cap = 16;
    m->cap = cap; m->n = 0;
    m->keys = xmalloc(sizeof(char *) * cap);
    m->vals = xmalloc(sizeof(char *) * cap);
    m->st = calloc(cap, 1);
    if (!m->st) die("内存不足");
}

static void sm_grow(StrMap *m);

static void sm_put(StrMap *m, const char *k, const char *v) {
    if ((m->n + 1) * 10 >= m->cap * 7) sm_grow(m);
    size_t i = (size_t)(hash_str(k) & (m->cap - 1));
    size_t first_tomb = (size_t)-1;
    for (;;) {
        if (m->st[i] == 0) {
            if (first_tomb != (size_t)-1) i = first_tomb;
            m->keys[i] = xstrdup(k); m->vals[i] = xstrdup(v);
            m->st[i] = 1; m->n++;
            return;
        }
        if (m->st[i] == 2) {
            if (first_tomb == (size_t)-1) first_tomb = i;
        } else if (strcmp(m->keys[i], k) == 0) {
            if (strcmp(m->vals[i], v) != 0) {
                free(m->vals[i]);
                m->vals[i] = xstrdup(v);
            }
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static const char *sm_get(const StrMap *m, const char *k) {
    size_t i = (size_t)(hash_str(k) & (m->cap - 1));
    for (;;) {
        if (m->st[i] == 0) return NULL;
        if (m->st[i] == 1 && strcmp(m->keys[i], k) == 0) return m->vals[i];
        i = (i + 1) & (m->cap - 1);
    }
}

static void sm_del(StrMap *m, const char *k) {
    size_t i = (size_t)(hash_str(k) & (m->cap - 1));
    for (;;) {
        if (m->st[i] == 0) return;
        if (m->st[i] == 1 && strcmp(m->keys[i], k) == 0) {
            free(m->keys[i]); free(m->vals[i]);
            m->keys[i] = NULL; m->vals[i] = NULL; m->st[i] = 2; m->n--;
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static void sm_grow(StrMap *m) {
    StrMap nm;
    sm_init(&nm, m->cap * 2);
    for (size_t i = 0; i < m->cap; i++)
        if (m->st[i] == 1) { sm_put(&nm, m->keys[i], m->vals[i]);
                             free(m->keys[i]); free(m->vals[i]); }
    free(m->keys); free(m->vals); free(m->st);
    *m = nm;
}

static void sm_free(StrMap *m) {
    for (size_t i = 0; i < m->cap; i++)
        if (m->st[i] == 1) { free(m->keys[i]); free(m->vals[i]); }
    free(m->keys); free(m->vals); free(m->st);
    m->keys = m->vals = NULL; m->st = NULL; m->cap = m->n = 0;
}

/* ------------------------------------------------------------------ SHA-256 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t buflen;
} Sha256;

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_init(Sha256 *s) {
    s->h[0]=0x6a09e667u; s->h[1]=0xbb67ae85u; s->h[2]=0x3c6ef372u; s->h[3]=0xa54ff53au;
    s->h[4]=0x510e527fu; s->h[5]=0x9b05688cu; s->h[6]=0x1f83d9abu; s->h[7]=0x5be0cd19u;
    s->len = 0; s->buflen = 0;
}

static void sha256_block(Sha256 *s, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha256_update(Sha256 *s, const void *data, size_t n) {
    const unsigned char *p = data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->buflen;
        if (take > n) take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take; p += take; n -= take;
        if (s->buflen == 64) { sha256_block(s, s->buf); s->buflen = 0; }
    }
}

static void sha256_final(Sha256 *s, char out[65]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha256_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->buflen != 56) sha256_update(s, &zero, 1);
    unsigned char lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (unsigned char)(bits >> (56 - i*8));
    sha256_update(s, lenbuf, 8);
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
            unsigned char byte = (unsigned char)(s->h[i] >> (24 - j*8));
            out[i*8 + j*2]     = hex[byte >> 4];
            out[i*8 + j*2 + 1] = hex[byte & 15];
        }
    out[64] = '\0';
}

/* ------------------------------------------------------------------ 文件系统 */

#ifdef _WIN32

static wchar_t *u8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static char *w_to_u8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc((size_t)(n > 0 ? n : 1));
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    return s;
}

typedef struct { char *name; int is_dir; } DirEntry;

static int fs_list(const char *dir, DirEntry **out, size_t *count) {
    char *pattern = fmt("%s\\*", dir);
    wchar_t *wpat = u8_to_w(pattern);
    free(pattern);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) return -1;

    DirEntry *arr = NULL;
    size_t n = 0, cap = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; arr = xrealloc(arr, sizeof(DirEntry) * cap); }
        arr[n].name = w_to_u8(fd.cFileName);
        arr[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    *out = arr; *count = n;
    return 0;
}

static void fs_list_free(DirEntry *arr, size_t n) {
    for (size_t i = 0; i < n; i++) free(arr[i].name);
    free(arr);
}

static int fs_is_dir(const char *p) {
    wchar_t *w = u8_to_w(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int fs_exists(const char *p) {
    wchar_t *w = u8_to_w(p);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

static int fs_rename(const char *src, const char *dst) {
    wchar_t *ws = u8_to_w(src), *wd = u8_to_w(dst);
    BOOL ok = MoveFileExW(ws, wd, 0);
    DWORD err = ok ? 0 : GetLastError();
    free(ws); free(wd);
    if (ok) return 0;
    return err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS ? EEXIST : (int)err;
}

static int fs_write_atomic(const char *path, const char *data, size_t len) {
    char *tmp = fmt("%s.tmp", path);
    wchar_t *wt = u8_to_w(tmp);
    HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(wt); free(tmp); return -1; }
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, (DWORD)len, &wrote, NULL);
    CloseHandle(h);
    if (!ok || wrote != len) { DeleteFileW(wt); free(wt); free(tmp); return -1; }

    wchar_t *wp = u8_to_w(path);
    BOOL moved = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING);
    free(wt); free(wp); free(tmp);
    return moved ? 0 : -1;
}

typedef struct { HANDLE h; } FileHandle;

static int fs_open_read(const char *path, FileHandle *fh, uint64_t *size) {
    wchar_t *w = u8_to_w(path);
    fh->h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (fh->h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(fh->h, &sz)) { CloseHandle(fh->h); return -1; }
    *size = (uint64_t)sz.QuadPart;
    return 0;
}

static int fs_read_at(FileHandle *fh, uint64_t off, void *buf, size_t n) {
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(fh->h, pos, NULL, FILE_BEGIN)) return -1;
    DWORD got = 0;
    if (!ReadFile(fh->h, buf, (DWORD)n, &got, NULL)) return -1;
    return (int)got;
}

static void fs_close(FileHandle *fh) { CloseHandle(fh->h); }

static void fs_unlink(const char *p) {
    wchar_t *w = u8_to_w(p);
    DeleteFileW(w);
    free(w);
}

static void sleep_ms(int ms) { Sleep((DWORD)ms); }

#else /* POSIX */

typedef struct { char *name; int is_dir; } DirEntry;

static int fs_list(const char *dir, DirEntry **out, size_t *count) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    DirEntry *arr = NULL;
    size_t n = 0, cap = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; arr = xrealloc(arr, sizeof(DirEntry) * cap); }
        arr[n].name = xstrdup(e->d_name);
        char *full = fmt("%s/%s", dir, e->d_name);
        struct stat sb;
        arr[n].is_dir = (stat(full, &sb) == 0 && S_ISDIR(sb.st_mode)) ? 1 : 0;
        free(full);
        n++;
    }
    closedir(d);
    *out = arr; *count = n;
    return 0;
}

static void fs_list_free(DirEntry *arr, size_t n) {
    for (size_t i = 0; i < n; i++) free(arr[i].name);
    free(arr);
}

static int fs_is_dir(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static int fs_exists(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0;
}

static int fs_rename(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    return errno;
}

static int fs_write_atomic(const char *path, const char *data, size_t len) {
    char *tmp = fmt("%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    if (wrote != len) { remove(tmp); free(tmp); return -1; }
    int rc = rename(tmp, path) == 0 ? 0 : -1;
    free(tmp);
    return rc;
}

typedef struct { int fd; } FileHandle;

static int fs_open_read(const char *path, FileHandle *fh, uint64_t *size) {
    fh->fd = open(path, O_RDONLY);
    if (fh->fd < 0) return -1;
    struct stat sb;
    if (fstat(fh->fd, &sb) != 0) { close(fh->fd); return -1; }
    *size = (uint64_t)sb.st_size;
    return 0;
}

static int fs_read_at(FileHandle *fh, uint64_t off, void *buf, size_t n) {
    ssize_t got = pread(fh->fd, buf, n, (off_t)off);
    return got < 0 ? -1 : (int)got;
}

static void fs_close(FileHandle *fh) { close(fh->fd); }

static void fs_unlink(const char *p) { remove(p); }

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

#endif

static char *path_join(const char *a, const char *b) {
#ifdef _WIN32
    size_t la = strlen(a);
    if (la && (a[la-1] == '\\' || a[la-1] == '/')) return fmt("%s%s", a, b);
    return fmt("%s\\%s", a, b);
#else
    size_t la = strlen(a);
    if (la && a[la-1] == '/') return fmt("%s%s", a, b);
    return fmt("%s/%s", a, b);
#endif
}

/* 真刀真枪试一次“建文件 → 改名 → 删除”，返回失败原因（NULL 表示可以写） */
/* ---------------------------------------------------------------- PowerShell 代劳 */

/* 有些环境（受控文件夹访问、应用白名单）会拦未签名程序写特定目录，
   但 PowerShell 是微软签名、通常放行。直接写失败时改由它执行。 */
static int g_use_shell = 0;      /* 1 = 已切换到 PowerShell 模式 */

static char *b64_encode(const unsigned char *in, size_t n) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = xmalloc(((n + 2) / 3) * 4 + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

/* 单引号转义（PowerShell 里 '' 表示一个 '） */
static char *ps_quote(const char *s) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') buf_puts(&b, "''");
        else buf_putc(&b, *p);
    }
    return b.data;
}

static int ps_run(const char *script_utf8) {
#ifdef _WIN32
    wchar_t *ws = u8_to_w(script_utf8);
    size_t wlen = wcslen(ws);
    char *enc = b64_encode((const unsigned char *)ws, wlen * sizeof(wchar_t));
    free(ws);

    char *cmd = fmt("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass"
                    " -EncodedCommand %s", enc);
    free(enc);
    wchar_t *wcmd = u8_to_w(cmd);
    free(cmd);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        free(wcmd);
        return -1;
    }
    free(wcmd);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    (void)script_utf8;
    return -1;
#endif
}

/* 用 PowerShell 把 src 改名成 dst（两者都是完整路径） */
static int shell_rename(const char *src, const char *dst) {
#ifdef _WIN32
    const char *slash = strrchr(dst, '\\');
    if (!slash) slash = strrchr(dst, '/');
    if (!slash) return -1;
    char *dirname = xstrndup(dst, (size_t)(slash - dst));
    char *newname = xstrdup(slash + 1);

    char *qs = ps_quote(src);
    char *qn = ps_quote(newname);
    char *script = fmt(
        "$ErrorActionPreference = 'Stop'\n"
        "try {\n"
        "  Rename-Item -LiteralPath '%s' -NewName '%s' -ErrorAction Stop\n"
        "  exit 0\n"
        "} catch { exit 1 }\n", qs, qn);
    int rc = ps_run(script);
    free(qs); free(qn); free(script);
    free(dirname); free(newname);
    return rc == 0 ? 0 : -1;
#else
    (void)src; (void)dst;
    return -1;
#endif
}

/* 在目录里用 PowerShell 完整走一遍“建文件 → 改名 → 删除” */
static int shell_probe(const char *root) {
#ifdef _WIN32
    char *probe = path_join(root, ".mp3rn_probe_ps.tmp");
    char *moved = path_join(root, ".mp3rn_probe_ps.moved");
    char *qp = ps_quote(probe);
    char *qm = ps_quote(moved);
    char *script = fmt(
        "$ErrorActionPreference = 'Stop'\n"
        "try {\n"
        "  [IO.File]::WriteAllText('%s', 'x')\n"
        "  Rename-Item -LiteralPath '%s' -NewName '%s' -ErrorAction Stop\n"
        "  Remove-Item -LiteralPath '%s' -Force -ErrorAction Stop\n"
        "  exit 0\n"
        "} catch {\n"
        "  try { Remove-Item -LiteralPath '%s' -Force -ErrorAction SilentlyContinue } catch { }\n"
        "  try { Remove-Item -LiteralPath '%s' -Force -ErrorAction SilentlyContinue } catch { }\n"
        "  exit 1\n"
        "}\n", qp, qp, qm, qm, qp, qm);
    int rc = ps_run(script);
    free(qp); free(qm); free(script);
    free(probe); free(moved);
    return rc == 0 ? 0 : -1;
#else
    (void)root;
    return -1;
#endif
}

/* 检查目录能否创建/改名文件：返回 NULL 表示可以，否则返回原因。
   直接写被拒绝时，会再试一次 PowerShell 通道（见 shell_probe）。 */
static char *check_writable(const char *root) {
    char *probe = path_join(root, ".mp3rn_write_test.tmp");
    char *moved = path_join(root, ".mp3rn_write_test.moved");
    char *err = NULL;
    int rc = 0, rc2 = 0;

    if ((rc = fs_write_atomic(probe, "x", 1)) != 0) {
        err = fmt("无法在这个目录创建文件（错误码 %d）", rc);
    } else if ((rc2 = fs_rename(probe, moved)) != 0) {
        err = fmt("无法在这个目录改名（错误码 %d）", rc2);
    }
    fs_unlink(probe);
    fs_unlink(moved);
    free(probe);
    free(moved);

    if (!err) {
        g_use_shell = 0;
        return NULL;
    }

    /* 直连被拒：试试用 PowerShell 代劳（受控文件夹访问会拦未签名程序，
       但不拦微软签名的 powershell.exe） */
    if (shell_probe(root) == 0) {
        free(err);
        g_use_shell = 1;
        logwarn("直接写入被系统拒绝，已切换为「PowerShell 代劳」模式（能用，但每次改名会慢一点）");
        return NULL;
    }

    free(err);
    err = xstrdup("这个目录既不能直接写、也无法用 PowerShell 代劳。\n\n"
                  "最可能的原因：本程序是被一个受限环境（沙箱 / 作业对象 / 受限账户）"
                  "启动的，连它拉起的子进程都继承了限制。\n"
                  "解决办法：在资源管理器里直接双击 exe 运行，不要从沙箱、"
                  "终端包装器或某些启动器里启动。\n\n"
                  "其次：Windows「受控文件夹访问」拦了未签名程序"
                  "（安全中心 → 病毒和威胁防护 → 勒索软件防护 → "
                  "受控文件夹访问 → 允许应用），或目录本身只读。");
    return err;
}

/* 内容指纹：大小 + 首尾各 FP_WINDOW 字节 */
static int fingerprint(const char *path, char out[65]) {
    FileHandle fh;
    uint64_t size = 0;
    if (fs_open_read(path, &fh, &size) != 0) return -1;

    Sha256 s;
    sha256_init(&s);
    char szbuf[32];
    int szlen = snprintf(szbuf, sizeof szbuf, "size:%llu|", (unsigned long long)size);
    sha256_update(&s, szbuf, (size_t)szlen);

    unsigned char *buf = xmalloc(FP_WINDOW);
    int got = fs_read_at(&fh, 0, buf, FP_WINDOW);
    if (got < 0) { free(buf); fs_close(&fh); return -1; }
    sha256_update(&s, buf, (size_t)got);

    if (size > FP_WINDOW) {
        uint64_t off = size - FP_WINDOW;
        got = fs_read_at(&fh, off, buf, FP_WINDOW);
        if (got < 0) { free(buf); fs_close(&fh); return -1; }
        sha256_update(&s, buf, (size_t)got);
    }
    free(buf);
    fs_close(&fh);
    sha256_final(&s, out);
    return 0;
}

/* ------------------------------------------------------------------ 小工具 */

static int suffix_is_mp3(const char *name, size_t *stem_len) {
    size_t n = strlen(name);
    if (n < 5) return 0;
    size_t dot = n;
    for (size_t i = n; i > 0; i--) {
        if (name[i-1] == '.') { dot = i - 1; break; }
    }
    if (dot == 0 || dot == n) return 0;            /* ".mp3" / "abc" 不算 */
    if (n - dot != 4) return 0;
    if (!(name[dot+1] == 'm' || name[dot+1] == 'M')) return 0;
    if (!(name[dot+2] == 'p' || name[dot+2] == 'P')) return 0;
    if (name[dot+3] != '3') return 0;
    *stem_len = dot;
    return 1;
}

static uint64_t rng_state;

static void rng_seed(void) {
    uint64_t t = (uint64_t)time(NULL);
    uint64_t p = (uint64_t)(uintptr_t)&rng_state;
#ifdef _WIN32
    p ^= (uint64_t)GetCurrentProcessId() << 17;
#else
    p ^= (uint64_t)getpid() << 17;
#endif
    rng_state = t ^ p;
    if (!rng_state) rng_state = 88172645463325252ULL;
}

static uint64_t rng_next(void) {
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ULL;
}

/* 抽一个没用过的数字；失败返回 0 */
static int draw_number(U64Set *used, int digits, uint64_t *out) {
    uint64_t low = 1, high = 9;
    if (digits > 1) { low = 1; for (int i = 1; i < digits; i++) low *= 10; }
    high = 1; for (int i = 0; i < digits; i++) high *= 10;
    high -= 1;
    uint64_t span = high - low + 1;
    if (used->n >= span) return 0;

    for (int i = 0; i < 20000; i++) {
        uint64_t n = low + rng_next() % span;
        if (!u64_has(used, n)) { *out = n; return 1; }
    }
    /* 密集时兜底顺序扫描（span 小才做） */
    if (span <= 1000000) {
        for (uint64_t n = low; n <= high; n++)
            if (!u64_has(used, n)) { *out = n; return 1; }
    }
    return 0;
}

/* 判断 stem 是不是本程序生成的名字；返回 1 表示是，base 为去掉数字后的原名 */
static int split_own_prefix(const char *stem, const U64Set *used, const char *sep,
                            char **base_out, uint64_t *num_out) {
    size_t n = strlen(stem), d = 0;
    while (d < n && stem[d] >= '0' && stem[d] <= '9') d++;
    if (!d) return 0;

    char *nums = xstrndup(stem, d);
    uint64_t num = (uint64_t)strtoull(nums, NULL, 10);
    free(nums);
    if (num == 0 && d > 0) { /* 前导零也算数字，但 0 不是我们发的号 */ }
    if (!u64_has(used, num)) return 0;

    const char *rest = stem + d;
    size_t seplen = strlen(sep);
    if (*rest == '\0') {
        *base_out = xstrdup("");
        *num_out = num;
        return 1;
    }
    if (seplen && strncmp(rest, sep, seplen) == 0) {
        *base_out = xstrdup(rest + seplen);
        *num_out = num;
        return 1;
    }
    if (!seplen) {
        *base_out = xstrdup(rest);
        *num_out = num;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ JSON */

static void json_escape(Buf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_putc(b, (char)*p);
        }
    }
}

typedef struct { const char *p; int err; } JP;

static void jp_ws(JP *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

static int jp_expect(JP *j, char c) {
    jp_ws(j);
    if (*j->p != c) { j->err = 1; return 0; }
    j->p++;
    return 1;
}

static void utf8_put(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_putc(b, (char)cp);
    else if (cp < 0x800) {
        buf_putc(b, (char)(0xC0 | (cp >> 6)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_putc(b, (char)(0xE0 | (cp >> 12)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | (cp >> 18)));
        buf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static unsigned jp_hex4(JP *j) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *j->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else { j->err = 1; return 0; }
    }
    return v;
}

static char *jp_string(JP *j) {
    jp_ws(j);
    if (*j->p != '"') { j->err = 1; return NULL; }
    j->p++;
    Buf b; buf_init(&b);
    buf_puts(&b, "");
    while (*j->p && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            char e = *j->p++;
            switch (e) {
            case '"':  buf_putc(&b, '"'); break;
            case '\\': buf_putc(&b, '\\'); break;
            case '/':  buf_putc(&b, '/'); break;
            case 'b':  buf_putc(&b, '\b'); break;
            case 'f':  buf_putc(&b, '\f'); break;
            case 'n':  buf_putc(&b, '\n'); break;
            case 'r':  buf_putc(&b, '\r'); break;
            case 't':  buf_putc(&b, '\t'); break;
            case 'u': {
                unsigned cp = jp_hex4(j);
                if (j->err) { buf_free(&b); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                    j->p += 2;
                    unsigned lo = jp_hex4(j);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8_put(&b, cp);
                break;
            }
            default: j->err = 1; buf_free(&b); return NULL;
            }
        } else {
            buf_putc(&b, (char)c);
        }
    }
    if (*j->p != '"') { j->err = 1; buf_free(&b); return NULL; }
    j->p++;
    return b.data;
}

static int jp_skip_value(JP *j);

static int jp_skip_array(JP *j) {
    if (!jp_expect(j, '[')) return 0;
    jp_ws(j);
    if (*j->p == ']') { j->p++; return 1; }
    for (;;) {
        if (!jp_skip_value(j)) return 0;
        jp_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; return 1; }
        j->err = 1; return 0;
    }
}

static int jp_skip_object(JP *j) {
    if (!jp_expect(j, '{')) return 0;
    jp_ws(j);
    if (*j->p == '}') { j->p++; return 1; }
    for (;;) {
        char *k = jp_string(j);
        if (!k) return 0;
        free(k);
        if (!jp_expect(j, ':')) return 0;
        if (!jp_skip_value(j)) return 0;
        jp_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return 1; }
        j->err = 1; return 0;
    }
}

static int jp_skip_value(JP *j) {
    jp_ws(j);
    if (*j->p == '"') { char *s = jp_string(j); free(s); return s != NULL; }
    if (*j->p == '[') return jp_skip_array(j);
    if (*j->p == '{') return jp_skip_object(j);
    while (*j->p && strchr("-+.0123456789eEtruefalsn", *j->p)) j->p++;
    return 1;
}

/* ------------------------------------------------------------------ 状态 */

typedef struct {
    char *path;          /* NULL = 不用状态文件 */
    U64Set used;
    StrSet names;
    StrMap orig;
    StrMap fps;
    int dirty;
} State;

static void state_init(State *st, const char *path) {
    st->path = path ? xstrdup(path) : NULL;
    u64_init(&st->used, 64);
    ss_init(&st->names, 64);
    sm_init(&st->orig, 64);
    sm_init(&st->fps, 64);
    st->dirty = 0;
}

static void state_free(State *st) {
    free(st->path);
    u64_free(&st->used);
    ss_free(&st->names);
    sm_free(&st->orig);
    sm_free(&st->fps);
}

static char *read_whole_file(const char *path) {
    FileHandle fh;
    uint64_t size = 0;
    if (fs_open_read(path, &fh, &size) != 0) return NULL;
    if (size > (64u << 20)) { fs_close(&fh); return NULL; }
    char *data = xmalloc((size_t)size + 1);
    int got = size ? fs_read_at(&fh, 0, data, (size_t)size) : 0;
    fs_close(&fh);
    if (got < 0) { free(data); return NULL; }
    data[got] = '\0';
    return data;
}

static int state_load(State *st) {
    if (!st->path) return 0;
    char *text = read_whole_file(st->path);
    if (!text) return 0;

    /* 去掉可能的 UTF-8 BOM */
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    JP j = { p, 0 };
    if (!jp_expect(&j, '{')) { free(text); return -1; }

    jp_ws(&j);
    if (*j.p == '}') { j.p++; free(text); return 0; }

    for (;;) {
        char *key = jp_string(&j);
        if (!key) { free(text); return -1; }
        if (!jp_expect(&j, ':')) { free(key); free(text); return -1; }

        jp_ws(&j);
        if (!strcmp(key, "used") && *j.p == '[') {
            j.p++;
            jp_ws(&j);
            if (*j.p == ']') j.p++;
            else for (;;) {
                jp_ws(&j);
                char *end = NULL;
                unsigned long long v = strtoull(j.p, &end, 10);
                if (end == j.p) { j.err = 1; break; }
                j.p = end;
                u64_add(&st->used, (uint64_t)v);
                jp_ws(&j);
                if (*j.p == ',') { j.p++; continue; }
                if (*j.p == ']') { j.p++; break; }
                j.err = 1; break;
            }
        } else if (!strcmp(key, "names") && *j.p == '[') {
            j.p++;
            jp_ws(&j);
            if (*j.p == ']') j.p++;
            else for (;;) {
                char *s = jp_string(&j);
                if (!s) { j.err = 1; break; }
                ss_add(&st->names, s);
                free(s);
                jp_ws(&j);
                if (*j.p == ',') { j.p++; continue; }
                if (*j.p == ']') { j.p++; break; }
                j.err = 1; break;
            }
        } else if ((!strcmp(key, "orig") || !strcmp(key, "fps")) && *j.p == '{') {
            StrMap *target = !strcmp(key, "orig") ? &st->orig : &st->fps;
            j.p++;
            jp_ws(&j);
            if (*j.p == '}') j.p++;
            else for (;;) {
                char *k = jp_string(&j);
                if (!k) { j.err = 1; break; }
                if (!jp_expect(&j, ':')) { free(k); j.err = 1; break; }
                char *v = jp_string(&j);
                if (!v) { free(k); j.err = 1; break; }
                sm_put(target, k, v);
                free(k); free(v);
                jp_ws(&j);
                if (*j.p == ',') { j.p++; continue; }
                if (*j.p == '}') { j.p++; break; }
                j.err = 1; break;
            }
        } else {
            if (!jp_skip_value(&j)) { free(key); free(text); return -1; }
        }

        free(key);
        if (j.err) { free(text); return -1; }
        jp_ws(&j);
        if (*j.p == ',') { j.p++; continue; }
        if (*j.p == '}') { break; }
        free(text);
        return -1;
    }

    free(text);
    return 0;
}

static int state_save(State *st) {
    if (!st->path) return 0;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"used\":[");
    {
        int first = 1;
        for (size_t i = 0; i < st->used.cap; i++) {
            if (st->used.st[i] != 1) continue;
            buf_printf(&b, "%s%llu", first ? "" : ",", (unsigned long long)st->used.keys[i]);
            first = 0;
        }
    }
    buf_puts(&b, "],\"names\":[");
    {
        int first = 1;
        for (size_t i = 0; i < st->names.cap; i++) {
            if (st->names.st[i] != 1) continue;
            buf_puts(&b, first ? "\"" : ",\"");
            json_escape(&b, st->names.keys[i]);
            buf_putc(&b, '"');
            first = 0;
        }
    }
    buf_puts(&b, "],\"orig\":{");
    {
        int first = 1;
        for (size_t i = 0; i < st->orig.cap; i++) {
            if (st->orig.st[i] != 1) continue;
            buf_puts(&b, first ? "\"" : ",\"");
            json_escape(&b, st->orig.keys[i]);
            buf_puts(&b, "\":\"");
            json_escape(&b, st->orig.vals[i]);
            buf_putc(&b, '"');
            first = 0;
        }
    }
    buf_puts(&b, "},\"fps\":{");
    {
        int first = 1;
        for (size_t i = 0; i < st->fps.cap; i++) {
            if (st->fps.st[i] != 1) continue;
            buf_puts(&b, first ? "\"" : ",\"");
            json_escape(&b, st->fps.keys[i]);
            buf_puts(&b, "\":\"");
            json_escape(&b, st->fps.vals[i]);
            buf_putc(&b, '"');
            first = 0;
        }
    }
    buf_puts(&b, "}}");

    int rc = fs_write_atomic(st->path, b.data, b.len);
    buf_free(&b);
    if (rc != 0) { logwarn("状态文件写入失败（%s）", st->path); return -1; }
    st->dirty = 0;
    return 0;
}

/* ------------------------------------------------------------------ 改名逻辑 */

typedef struct {
    State *st;
    const char *sep;
    int digits;
    int replace_mode;
    int recursive;
    int dry_run;
    int reprocess;
    StrSet processed;   /* 本次运行刚改出来的名字 */
    StrSet previewed;   /* 试运行已预告过的路径 */
    StrSet warned;      /* 已警告过的路径，避免刷屏 */
    int count;
} Ctx;

static void ctx_init(Ctx *c, State *st, const char *sep, int digits, int replace_mode,
                     int recursive, int dry_run, int reprocess) {
    c->st = st; c->sep = sep; c->digits = digits; c->replace_mode = replace_mode;
    c->recursive = recursive; c->dry_run = dry_run; c->reprocess = reprocess;
    ss_init(&c->processed, 64);
    ss_init(&c->previewed, 64);
    ss_init(&c->warned, 64);
    c->count = 0;
}

static void ctx_free(Ctx *c) {
    ss_free(&c->processed);
    ss_free(&c->previewed);
    ss_free(&c->warned);
}

static void warn_once(Ctx *c, const char *path, const char *msg) {
    if (ss_has(&c->warned, path)) return;
    ss_add(&c->warned, path);
    logwarn("%s：%s", msg, path);
}

static const char *errname(int e) {
    if (e == EEXIST) return "目标已存在";
    if (e == ENOENT) return "文件不存在";
    if (e == EACCES) return "拒绝访问";
    return "系统错误";
}

/* 目标名被占用时加 (2)(3)… */
static char *free_name(const char *dir, const char *want) {
    char *full = path_join(dir, want);
    if (!fs_exists(full)) { free(full); return xstrdup(want); }
    free(full);

    size_t n = strlen(want), dot = n;
    for (size_t i = n; i > 0; i--) if (want[i-1] == '.') { dot = i - 1; break; }
    if (dot == n) dot = n;
    char *stem = xstrndup(want, dot);
    const char *ext = want + dot;

    for (int i = 2; i < 1000; i++) {
        char *cand = fmt("%s (%d)%s", stem, i, ext);
        char *f = path_join(dir, cand);
        int exists = fs_exists(f);
        free(f);
        if (!exists) { free(stem); return cand; }
        free(cand);
    }
    free(stem);
    return NULL;
}

static int rename_with_retry(const char *src, const char *dst) {
#ifdef _WIN32
    if (g_use_shell) {
        if (shell_rename(src, dst) == 0) return 0;
    }
#endif
    int last = 0;
    for (int i = 0; i < RENAME_RETRIES; i++) {
        last = fs_rename(src, dst);
        if (last == 0) return 0;
        if (last == EEXIST) return last;
#ifdef _WIN32
        if (i == 0 && last == 5) {          /* 拒绝访问：换成 PowerShell 通道 */
            if (shell_rename(src, dst) == 0) {
                g_use_shell = 1;
                return 0;
            }
        }
#endif
        if (i < RENAME_RETRIES - 1) sleep_ms(200 * (i + 1));
    }
    return last;
}

static void scan_dir(Ctx *c, const char *dir);

static void handle_file(Ctx *c, const char *dir, const char *name) {
    size_t stem_len = 0;
    if (!suffix_is_mp3(name, &stem_len)) return;

    char *full = path_join(dir, name);
    if (ss_has(&c->processed, name)) { free(full); return; }

    char *stem = xstrndup(name, stem_len);
    const char *suffix = name + stem_len;

    char *base = NULL;
    uint64_t pnum = 0;
    int ours = split_own_prefix(stem, &c->st->used, c->sep, &base, &pnum);
    if (!ours) base = xstrdup(stem);

    if (!c->reprocess && (ss_has(&c->st->names, name) || ours)) {
        free(base); free(stem); free(full);
        return;
    }
    if (c->dry_run && ss_has(&c->previewed, full)) {
        free(base); free(stem); free(full);
        return;
    }

    /* 选一个新编号 */
    uint64_t n = 0;
    char *target_name = NULL;
    int attempts = 0;
    for (;;) {
        if (++attempts > 500) {
            logwarn("连续 500 次都抽不到可用编号：%s", name);
            free(base); free(stem); free(full);
            return;
        }
        if (!draw_number(&c->st->used, c->digits, &n)) {
            logwarn("%d 位随机数字已经用完，请加大 --digits", c->digits);
            free(base); free(stem); free(full);
            return;
        }
        char *cand;
        if (c->replace_mode || !*base) cand = fmt("%llu%s", (unsigned long long)n, suffix);
        else cand = fmt("%llu%s%s%s", (unsigned long long)n, c->sep, base, suffix);

        char *cand_full = path_join(dir, cand);
        if (strcmp(cand, name) == 0 || fs_exists(cand_full)) {
            free(cand); free(cand_full);
            continue;
        }
        free(cand_full);
        target_name = cand;
        u64_add(&c->st->used, n);
        break;
    }

    if (strlen(target_name) > MAX_NAME_LEN) {
        u64_del(&c->st->used, n);
        warn_once(c, name, "新文件名超过 255 字符，跳过");
        free(target_name); free(base); free(stem); free(full);
        return;
    }

    if (c->dry_run) {
        ss_add(&c->previewed, full);
        loginfo("[试运行] %s  ->  %s", name, target_name);
        c->count++;
        free(target_name); free(base); free(stem); free(full);
        return;
    }

    /* 改名前先算内容指纹 */
    char digest[65];
    int have_digest = fingerprint(full, digest) == 0;
    const char *known = have_digest ? sm_get(&c->st->fps, digest) : NULL;

    char *target_full = path_join(dir, target_name);
    int rc = rename_with_retry(full, target_full);
    if (rc != 0) {
        u64_del(&c->st->used, n);
        char msg[128];
        snprintf(msg, sizeof msg, "改名失败（%s，错误码 %d），稍后重试", errname(rc), rc);
        warn_once(c, name, msg);
        free(target_full); free(target_name); free(base); free(stem); free(full);
        return;
    }

    /* 记录：hash 里已有的原名优先，其次前缀推断，最后才是当前名字 */
    char *original;
    if (known) original = xstrdup(known);
    else if (ours) original = fmt("%s%s", base, suffix);
    else {
        const char *prev = sm_get(&c->st->orig, name);
        original = xstrdup(prev ? prev : name);
    }

    sm_del(&c->st->orig, name);
    sm_put(&c->st->orig, target_name, original);
    if (have_digest) sm_put(&c->st->fps, digest, original);
    ss_del(&c->st->names, name);
    ss_add(&c->st->names, target_name);
    ss_add(&c->processed, target_name);
    c->st->dirty = 1;
    c->count++;

    loginfo("%s  ->  %s", name, target_name);

    free(original);
    free(target_full); free(target_name); free(base); free(stem); free(full);
}

static void scan_dir(Ctx *c, const char *dir) {
    DirEntry *entries = NULL;
    size_t n = 0;
    if (fs_list(dir, &entries, &n) != 0) {
        logwarn("无法读取目录：%s", dir);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;
        handle_file(c, dir, entries[i].name);
    }
    if (c->recursive) {
        for (size_t i = 0; i < n; i++) {
            if (!entries[i].is_dir) continue;
            char *sub = path_join(dir, entries[i].name);
            scan_dir(c, sub);
            free(sub);
        }
    }
    fs_list_free(entries, n);
}

/* ------------------------------------------------------------------ 恢复原名 */

typedef struct { char *name; char *why; } SkipItem;

static int restore_dir(Ctx *c, const char *dir, int force, SkipItem **skips, size_t *nskips) {
    DirEntry *entries = NULL;
    size_t n = 0;
    if (fs_list(dir, &entries, &n) != 0) {
        logwarn("无法读取目录：%s", dir);
        return 0;
    }

    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].is_dir) continue;

        const char *name = entries[i].name;
        size_t stem_len = 0;
        if (!suffix_is_mp3(name, &stem_len)) continue;

        char *stem = xstrndup(name, stem_len);
        const char *suffix = name + stem_len;

        char *original = NULL;
        const char *how = "记录";
        uint64_t pnum = 0;
        int have_num = 0;

        const char *prev = sm_get(&c->st->orig, name);
        if (prev) {
            original = xstrdup(prev);
            size_t d = 0;
            while (stem[d] >= '0' && stem[d] <= '9') d++;
            if (d) { char *t = xstrndup(stem, d); pnum = strtoull(t, NULL, 10); free(t); have_num = 1; }
        } else {
            char *reason = NULL;
            const char *seps[4];
            int nseps = 0;
            seps[nseps++] = c->sep;
            if (strcmp(c->sep, "_")) seps[nseps++] = "_";
            if (strcmp(c->sep, "-")) seps[nseps++] = "-";
            seps[nseps++] = "";

            for (int si = 0; si < nseps; si++) {
                char *base = NULL;
                uint64_t num = 0;
                if (!split_own_prefix(stem, &c->st->used, seps[si], &base, &num)) continue;

                if (!*base) {
                    reason = xstrdup("纯数字名，且没有原名记录");
                } else {
                    char numbuf[32];
                    snprintf(numbuf, sizeof numbuf, "%llu", (unsigned long long)num);
                    if (!force && strlen(numbuf) < MIN_LEGACY_DIGITS) {
                        reason = fmt("前缀 %s 位数偏短，可能是歌名自带的数字，未恢复", numbuf);
                    } else {
                        original = fmt("%s%s", base, suffix);
                        pnum = num; have_num = 1;
                        reason = NULL;
                    }
                }
                free(base);
                break;
            }

            if (!original) {
                char digest[65];
                char *fullpath = path_join(dir, name);
                int ok = fingerprint(fullpath, digest) == 0;
                free(fullpath);
                if (ok) {
                    const char *hit = sm_get(&c->st->fps, digest);
                    if (hit) {
                        original = xstrdup(hit);
                        how = "内容指纹";
                        reason = NULL;
                    }
                }
            }

            if (!original) {
                if (reason) {
                    *skips = xrealloc(*skips, sizeof(SkipItem) * (*nskips + 1));
                    (*skips)[*nskips].name = xstrdup(name);
                    (*skips)[*nskips].why = reason;
                    (*nskips)++;
                } else {
                    free(reason);
                }
                free(stem);
                continue;
            }
            free(reason);
        }

        if (c->dry_run) {
            loginfo("[试运行] %s  ->  %s（按%s）", name, original, how);
            changed++;
            free(original); free(stem);
            continue;
        }

        char *want = free_name(dir, original);
        if (!want) {
            logwarn("找不到可用的文件名：%s", original);
            free(original); free(stem);
            continue;
        }
        char *src = path_join(dir, name);
        char *dst = path_join(dir, want);
        int rc = strcmp(name, want) == 0 ? 0 : rename_with_retry(src, dst);
        if (rc != 0) {
            logwarn("恢复失败 %s：%s", name, errname(rc));
            free(src); free(dst); free(want); free(original); free(stem);
            continue;
        }
        if (strcmp(name, want) == 0) {
            free(src); free(dst); free(want); free(original); free(stem);
            continue;
        }

        sm_del(&c->st->orig, name);
        ss_del(&c->st->names, name);
        if (have_num) u64_del(&c->st->used, pnum);
        c->st->dirty = 1;
        changed++;
        loginfo("%s  ->  %s%s", name, want,
                strcmp(how, "内容指纹") == 0 ? "（按内容指纹匹配）" : "");

        free(src); free(dst); free(want); free(original); free(stem);
    }

    if (c->recursive) {
        for (size_t i = 0; i < n; i++) {
            if (!entries[i].is_dir) continue;
            char *sub = path_join(dir, entries[i].name);
            changed += restore_dir(c, sub, force, skips, nskips);
            free(sub);
        }
    }
    fs_list_free(entries, n);
    return changed;
}

/* ------------------------------------------------------------------ 命令行 */

static void usage(void) {
    fputs(
"mp3rn —— 把 mp3 文件名改成不重复的随机数字（原名保留 / 只留数字）\n"
"\n"
"用法:\n"
"  mp3rn <目录> [选项]\n"
"\n"
"选项:\n"
"  --mode prefix|replace   加在原名前(默认) / 只保留数字\n"
"  --sep <字符串>          数字与原名之间的分隔符，默认 \"_\"，可设为空\n"
"  --digits <1-18>         随机数字位数，默认 9\n"
"  --interval <秒>         监测轮询间隔，默认 1\n"
"  --recursive, -r         递归子文件夹\n"
"  --once                  只扫描一次\n"
"  --dry-run               只显示，不改名\n"
"  --reprocess             已改过名的文件重新编号\n"
"  --restore               恢复原名\n"
"  --restore-any           配合 --restore：不校验前缀位数\n"
"  --state <文件>          指定状态文件\n"
"  --no-state              不使用状态文件\n"
"  -q, --quiet             少打日志\n"
"  -h, --help              显示帮助\n"
"\n"
"示例:\n"
"  mp3rn D:\\Music --once\n"
"  mp3rn D:\\Music --restore --dry-run\n"
"  mp3rn /storage/emulated/0/Music --start\n",
    stdout);
}

#ifdef _WIN32
static int win_argc;
static char **win_argv;

static void build_argv_utf8(void) {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) die("无法解析命令行参数");
    win_argv = xmalloc(sizeof(char *) * (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) win_argv[i] = w_to_u8(wargv[i]);
    win_argv[argc] = NULL;
    win_argc = argc;
    LocalFree(wargv);
}
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

#ifndef MP3RN_NO_MAIN
int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    build_argv_utf8();
    argc = win_argc;
    argv = win_argv;
#endif
    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    if (argc < 2) { usage(); return 1; }

    const char *root = NULL;
    const char *sep = DEFAULT_SEP;
    const char *state_path = NULL;
    int digits = DEFAULT_DIGITS;
    double interval = DEFAULT_INTERVAL;
    int replace_mode = 0, recursive = 0, once = 0, dry_run = 0;
    int reprocess = 0, restore = 0, restore_any = 0, no_state = 0;

    /* 把 --opt=value 拆成 --opt value：这样 --sep= 这种空值写法在各种 shell 里都能用 */
    {
        char **norm = xmalloc(sizeof(char *) * (size_t)(argc * 2 + 2));
        int nn = 0;
        norm[nn++] = argv[0];
        for (int i = 1; i < argc; i++) {
            char *a = argv[i];
            char *eq = (a[0] == '-' && a[1] == '-') ? strchr(a, '=') : NULL;
            if (eq) {
                norm[nn++] = xstrndup(a, (size_t)(eq - a));
                norm[nn++] = eq + 1;
            } else {
                norm[nn++] = a;
            }
        }
        norm[nn] = NULL;
        argv = norm;
        argc = nn;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) g_quiet = 1;
        else if (!strcmp(a, "-r") || !strcmp(a, "--recursive")) recursive = 1;
        else if (!strcmp(a, "--once")) once = 1;
        else if (!strcmp(a, "--dry-run")) dry_run = 1;
        else if (!strcmp(a, "--reprocess")) reprocess = 1;
        else if (!strcmp(a, "--restore")) restore = 1;
        else if (!strcmp(a, "--restore-any")) restore_any = 1;
        else if (!strcmp(a, "--no-state")) no_state = 1;
        else if (!strcmp(a, "--mode")) {
            if (++i >= argc) die("--mode 需要一个值");
            if (!strcmp(argv[i], "replace")) replace_mode = 1;
            else if (!strcmp(argv[i], "prefix")) replace_mode = 0;
            else die("--mode 只能是 prefix 或 replace");
        }
        else if (!strcmp(a, "--sep")) {
            if (++i >= argc) die("--sep 需要一个值");
            sep = argv[i];
        }
        else if (!strcmp(a, "--digits")) {
            if (++i >= argc) die("--digits 需要一个值");
            digits = atoi(argv[i]);
            if (digits < 1 || digits > 18) die("--digits 必须是 1~18");
        }
        else if (!strcmp(a, "--interval")) {
            if (++i >= argc) die("--interval 需要一个值");
            interval = atof(argv[i]);
            if (interval <= 0) die("--interval 必须大于 0");
        }
        else if (!strcmp(a, "--state")) {
            if (++i >= argc) die("--state 需要一个值");
            state_path = argv[i];
        }
        else if (a[0] == '-') die("未知参数：%s", a);
        else if (!root) root = a;
        else die("多余的参数：%s", a);
    }

    if (!root) { usage(); return 1; }
    if (!fs_is_dir(root)) die("这不是文件夹：%s", root);

    char *sp = NULL;
    if (!no_state) sp = state_path ? xstrdup(state_path) : path_join(root, STATE_NAME);

    State st;
    state_init(&st, sp);
    free(sp);
    if (state_load(&st) != 0) logwarn("状态文件内容异常，当作空状态继续");

    rng_seed();

    if (restore) {
        loginfo("监测目录：%s", root);
        loginfo("模式：恢复原名（原名记录 %zu 条，内容指纹 %zu 条）", st.orig.n, st.fps.n);
        if (dry_run) loginfo("试运行模式：只显示，不动文件");
        Ctx c;
        ctx_init(&c, &st, sep, digits, replace_mode, recursive, dry_run, reprocess);
        SkipItem *skips = NULL;
        size_t nskips = 0;
        int changed = restore_dir(&c, root, restore_any, &skips, &nskips);
        if (nskips) {
            logwarn("有 %zu 个文件没能恢复：", nskips);
            for (size_t i = 0; i < nskips && i < 5; i++)
                logwarn("    %s —— %s", skips[i].name, skips[i].why);
            if (nskips > 5) logwarn("    ……还有 %zu 个同类情况", nskips - 5);
            for (size_t i = 0; i < nskips; i++) { free(skips[i].name); free(skips[i].why); }
            free(skips);
        }
        if (!dry_run && st.dirty) state_save(&st);
        loginfo("完成，共恢复 %d 个文件。", changed);
        ctx_free(&c);
        state_free(&st);
        return 0;
    }

    loginfo("监测目录：%s", root);
    loginfo("匹配规则：*.mp3（大小写不敏感）%s", recursive ? "，含子文件夹" : "，不含子文件夹");
    loginfo("改名方式：%s；随机数字 %d 位",
            replace_mode ? "只保留数字（例如 123.mp3）" : "数字加在原名前",
            digits);
    loginfo("运行方式：%s", once ? "只扫描一次" : "持续监测");
    if (dry_run) loginfo("试运行模式：不会真的改名");
    if (reprocess) loginfo("重新处理：已改过名的文件会摘掉旧数字、换新编号");
    if (st.path) loginfo("状态文件：%s（已用编号 %zu 个）", st.path, st.used.n);

    Ctx c;
    ctx_init(&c, &st, sep, digits, replace_mode, recursive, dry_run, reprocess);

    int total = 0;
    for (;;) {
        int before = c.count;
        scan_dir(&c, root);
        total += c.count - before;
        if (!dry_run && st.dirty) state_save(&st);
        if (once) break;
        if (g_stop) break;
        sleep_ms((int)(interval * 1000));
        if (g_stop) break;
    }

    loginfo("完成，共处理 %d 个文件。", total);
    ctx_free(&c);
    state_free(&st);
    return 0;
}
#endif /* MP3RN_NO_MAIN */
