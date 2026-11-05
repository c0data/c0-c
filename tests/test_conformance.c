/* Runs the shared conformance vectors (generated into vectors_gen.h from
 * c0-spec/vectors by tools/gen_vectors.py). */
#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_diff.h"

#include <stdio.h>
#include <stdlib.h>
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

/* The logical value of a raw unit equals e[0..n). */
static int cf_raw_eq(c0_bytes raw, const unsigned char *e, size_t n) {
    uint8_t *scratch = (uint8_t *)malloc(raw.len ? raw.len : 1);
    c0_bytes v;
    int ok;
    if (!scratch) return 0;
    ok = c0_value(raw, scratch, &v) == C0_OK && cf_eq(v, e, n);
    free(scratch);
    return ok;
}

static int cf_val_eq(c0_record r, size_t idx, const unsigned char *e, size_t n) {
    c0_bytes raw;
    if (c0_record_field(r, idx, &raw) != C0_OK) return 0;
    return cf_raw_eq(raw, e, n);
}

static int cf_level(c0_record r, size_t idx, c0_body *out) {
    c0_bytes raw;
    if (c0_record_field(r, idx, &raw) != C0_OK) return 0;
    return c0_unit_level(raw, out) == C0_OK;
}

static int cf_ref(c0_record r, size_t idx, c0_ref *out) {
    c0_bytes raw;
    if (c0_record_field(r, idx, &raw) != C0_OK) return 0;
    return c0_unit_ref(raw, out) == C0_OK;
}

/* Name of the document's first file. */
static c0_bytes cf_doc_name(const c0_document *d) {
    c0_file_iter fi = c0_body_files(d->body);
    c0_file f;
    c0_bytes empty;
    if (c0_next_file(&fi, &f)) return c0_file_name(f);
    empty.ptr = d->buf;
    empty.len = 0;
    return empty;
}

/* All records of a stream's undamaged committed blocks, read as a document. */
typedef struct {
    uint8_t *buf;
    size_t len;
    c0_document doc;
    c0_group_iter gi;
    c0_record_iter ri;
    int have_group;
} cf_recs;

static int cf_recs_init(cf_recs *rr, const c0_stream *s) {
    rr->buf = (uint8_t *)malloc(s->committed_end + 1);
    rr->have_group = 0;
    if (!rr->buf) return 0;
    rr->len = c0_stream_undamaged(s, rr->buf);
    if (c0_document_read(rr->buf, rr->len, 0, &rr->doc, NULL) != C0_OK) return 0;
    rr->gi = c0_body_groups(rr->doc.body);
    return 1;
}

static int cf_recs_next(cf_recs *rr, c0_record *r) {
    c0_group g;
    for (;;) {
        if (rr->have_group && c0_next_record(&rr->ri, r)) return 1;
        if (!c0_next_group(&rr->gi, &g)) return 0;
        rr->ri = c0_group_records(g);
        rr->have_group = 1;
    }
}

static void cf_recs_free(cf_recs *rr) { free(rr->buf); }

/* An output file with the given path has the given content. */
static int cf_out_eq(const c0_diff_file_output *out, size_t n, const unsigned char *path, size_t plen,
                     const unsigned char *content, size_t clen) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (out[i].path_len == plen && memcmp(out[i].path, path, plen) == 0) {
            return out[i].content_len == clen && (clen == 0 || memcmp(out[i].content, content, clen) == 0);
        }
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
    cf_nested();
    cf_diff();
    if (failures) {
        printf("%d conformance failure(s)\n", failures);
        return 1;
    }
    printf("all conformance vectors passed\n");
    return 0;
}
