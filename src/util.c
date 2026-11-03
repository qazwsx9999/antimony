// util.c - memory allocation, byte buffers, error reporting, string interning.
#include "sb.h"

const char *g_filename = "<input>";
const char *g_source = "";

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void buf_reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_u8(Buf *b, uint8_t v) {
    buf_reserve(b, 1);
    b->data[b->len++] = v;
}

void buf_u16(Buf *b, uint16_t v) {
    buf_reserve(b, 2);
    memcpy(b->data + b->len, &v, 2);
    b->len += 2;
}

void buf_u32(Buf *b, uint32_t v) {
    buf_reserve(b, 4);
    memcpy(b->data + b->len, &v, 4);
    b->len += 4;
}

void buf_u64(Buf *b, uint64_t v) {
    buf_reserve(b, 8);
    memcpy(b->data + b->len, &v, 8);
    b->len += 8;
}

void buf_bytes(Buf *b, const void *p, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_zero(Buf *b, size_t n) {
    if (!n) return;
    buf_reserve(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

void buf_align(Buf *b, size_t a) {
    size_t rem = b->len % a;
    if (rem) buf_zero(b, a - rem);
}

void buf_put_u32(Buf *b, size_t off, uint32_t v) {
    memcpy(b->data + off, &v, 4);
}

_Noreturn void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sbc: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

_Noreturn void fatal(Pos pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d:%d: error: ", g_filename, pos.line, pos.col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    // Print the offending source line with a caret.
    if (g_source && pos.line > 0) {
        const char *p = g_source;
        int line = 1;
        while (*p && line < pos.line) {
            if (*p++ == '\n') line++;
        }
        const char *end = p;
        while (*end && *end != '\n' && *end != '\r') end++;
        fprintf(stderr, "  %4d | %.*s\n", pos.line, (int)(end - p), p);
        fprintf(stderr, "       | ");
        for (int i = 1; i < pos.col; i++) {
            fputc((p + i - 1 < end && p[i - 1] == '\t') ? '\t' : ' ', stderr);
        }
        fprintf(stderr, "^\n");
    }
    exit(1);
}

// Open-addressed hash table for string interning.
typedef struct {
    const char *str;
    int len;
    uint32_t hash;
} InternEntry;

static InternEntry *intern_tab;
static int intern_cap;
static int intern_cnt;

static uint32_t fnv1a(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

const char *intern(const char *s, int len) {
    if (intern_cnt * 2 >= intern_cap) {
        int ncap = intern_cap ? intern_cap * 2 : 256;
        InternEntry *ntab = xcalloc((size_t)ncap, sizeof(InternEntry));
        for (int i = 0; i < intern_cap; i++) {
            if (!intern_tab[i].str) continue;
            uint32_t idx = intern_tab[i].hash & (uint32_t)(ncap - 1);
            while (ntab[idx].str) idx = (idx + 1) & (uint32_t)(ncap - 1);
            ntab[idx] = intern_tab[i];
        }
        free(intern_tab);
        intern_tab = ntab;
        intern_cap = ncap;
    }
    uint32_t h = fnv1a(s, len);
    uint32_t idx = h & (uint32_t)(intern_cap - 1);
    while (intern_tab[idx].str) {
        if (intern_tab[idx].hash == h && intern_tab[idx].len == len &&
            memcmp(intern_tab[idx].str, s, (size_t)len) == 0) {
            return intern_tab[idx].str;
        }
        idx = (idx + 1) & (uint32_t)(intern_cap - 1);
    }
    char *copy = xmalloc((size_t)len + 1);
    memcpy(copy, s, (size_t)len);
    copy[len] = '\0';
    intern_tab[idx] = (InternEntry){copy, len, h};
    intern_cnt++;
    return copy;
}

char *i128_str(i128 v, char *buf) {
    char tmp[48];
    int i = 0;
    bool neg = v < 0;
    unsigned __int128 u = neg ? -(unsigned __int128)v : (unsigned __int128)v;
    if (u == 0) {
        tmp[i++] = '0';
    } else {
        while (u > 0) {
            tmp[i++] = (char)('0' + (u % 10));
            u /= 10;
        }
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}
