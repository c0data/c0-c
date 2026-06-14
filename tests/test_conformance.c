/* Runs the shared conformance vectors (generated into vectors_gen.h). */
#define C0_IMPLEMENTATION
#include "../c0.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                              \
        }                                                            \
    } while (0)

static int cf_eq(c0_bytes g, const unsigned char *e, size_t n) {
    return g.len == n && (n == 0 || memcmp(g.ptr, e, n) == 0);
}

static int cf_arity(c0_bytes rec) {
    c0_field_iter fi = c0_record_fields(rec);
    c0_bytes f;
    int n = 0;
    while (c0_next_field(&fi, &f)) n++;
    return n;
}

static int cf_val_eq(c0_bytes rec, size_t idx, const unsigned char *e, size_t n) {
    c0_field_iter fi = c0_record_fields(rec);
    c0_bytes f;
    size_t i = 0;
    while (c0_next_field(&fi, &f)) {
        if (i == idx) {
            unsigned char tmp[1024];
            size_t m;
            if (f.len > sizeof(tmp)) return 0;
            m = c0_unescape(f.ptr, f.len, tmp);
            return m == n && (n == 0 || memcmp(tmp, e, n) == 0);
        }
        i++;
    }
    return 0;
}

static int cf_wellformed(const unsigned char *b, size_t n) {
    c0_tokenizer tz;
    c0_token t;
    c0_step s;
    c0_tokenizer_init(&tz, b, n);
    while ((s = c0_tokenizer_next(&tz, &t)) == C0_TOKEN) {
    }
    return s != C0_ERROR;
}

static int cf_block_count(const c0_stream *s) {
    c0_block_iter bi = c0_stream_blocks(s);
    c0_bytes blk;
    int n = 0;
    while (c0_next_block(&bi, &blk)) n++;
    return n;
}

static int cf_block_eq(const c0_stream *s, int idx, const unsigned char *e, size_t n) {
    c0_block_iter bi = c0_stream_blocks(s);
    c0_bytes blk;
    int i = 0;
    while (c0_next_block(&bi, &blk)) {
        if (i == idx) return blk.len == n && (n == 0 || memcmp(blk.ptr, e, n) == 0);
        i++;
    }
    return 0;
}

#include "vectors_gen.h"

int main(void) {
    cf_decode();
    cf_encode();
    cf_canonical();
    cf_invalid();
    cf_stream();
    if (failures) {
        printf("%d conformance failure(s)\n", failures);
        return 1;
    }
    printf("all conformance vectors passed\n");
    return 0;
}
