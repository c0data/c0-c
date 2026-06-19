/* c0_strbuf.h — a tiny growable byte buffer shared by the converter sources.
 * static inline so it can be included into multiple translation units. */
#ifndef C0_STRBUF_H
#define C0_STRBUF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *d;
    size_t n;
    size_t cap;
    int oom;
} c0_strbuf;

static inline int c0_sb_reserve(c0_strbuf *s, size_t extra) {
    if (s->oom) return 0;
    if (s->n + extra > s->cap) {
        size_t ncap = s->cap ? s->cap : 64;
        uint8_t *nd;
        while (ncap < s->n + extra) ncap *= 2;
        nd = (uint8_t *)realloc(s->d, ncap);
        if (!nd) {
            s->oom = 1;
            return 0;
        }
        s->d = nd;
        s->cap = ncap;
    }
    return 1;
}

static inline void c0_sb_byte(c0_strbuf *s, uint8_t b) {
    if (c0_sb_reserve(s, 1)) s->d[s->n++] = b;
}

static inline void c0_sb_raw(c0_strbuf *s, const uint8_t *p, size_t n) {
    if (n && c0_sb_reserve(s, n)) {
        memcpy(s->d + s->n, p, n);
        s->n += n;
    }
}

static inline void c0_sb_cstr(c0_strbuf *s, const char *p) {
    c0_sb_raw(s, (const uint8_t *)p, strlen(p));
}

#endif /* C0_STRBUF_H */
