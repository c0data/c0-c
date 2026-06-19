#include "c0_csv.h"
#include "c0_strbuf.h"

/* --- CSV → C0DATA --- */

typedef struct {
    size_t start, len;
} csv_span;

typedef struct {
    csv_span *v;
    size_t n, cap;
} csv_spans;

static int spans_push(csv_spans *a, size_t start, size_t len) {
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        csv_span *nv = (csv_span *)realloc(a->v, nc * sizeof(csv_span));
        if (!nv) return 0;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n].start = start;
    a->v[a->n].len = len;
    a->n++;
    return 1;
}

static int sizes_push(size_t **v, size_t *n, size_t *cap, size_t x) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        size_t *nv = (size_t *)realloc(*v, nc * sizeof(size_t));
        if (!nv) return 0;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return 1;
}

uint8_t *c0_from_csv(const char *csv, size_t csv_len, const char *group_name, size_t *out_len) {
    c0_strbuf data = {0, 0, 0, 0}; /* unquoted field bytes */
    csv_spans fields = {0, 0, 0};  /* field spans into data */
    size_t *row_ends = NULL;       /* fields-index where each row ends */
    size_t nrows = 0, caprows = 0;
    size_t field_start = 0;
    int in_quotes = 0, started = 0;
    size_t i;
    c0_builder b;
    c0_bytes out;
    uint8_t *result = NULL;
    int ok = 1;

    if (out_len) *out_len = 0;

    for (i = 0; i < csv_len; i++) {
        char c = csv[i];
        started = 1;
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < csv_len && csv[i + 1] == '"') {
                    c0_sb_byte(&data, '"');
                    i++;
                } else {
                    in_quotes = 0;
                }
            } else {
                c0_sb_byte(&data, (uint8_t)c);
            }
        } else if (c == '"') {
            in_quotes = 1;
        } else if (c == ',') {
            if (!spans_push(&fields, field_start, data.n - field_start)) { ok = 0; break; }
            field_start = data.n;
        } else if (c == '\n') {
            if (!spans_push(&fields, field_start, data.n - field_start)) { ok = 0; break; }
            if (!sizes_push(&row_ends, &nrows, &caprows, fields.n)) { ok = 0; break; }
            field_start = data.n;
            started = 0;
        } else if (c != '\r') {
            c0_sb_byte(&data, (uint8_t)c);
        }
    }
    if (ok && (started || data.n > field_start || fields.n > (nrows ? row_ends[nrows - 1] : 0))) {
        if (spans_push(&fields, field_start, data.n - field_start)) {
            sizes_push(&row_ends, &nrows, &caprows, fields.n);
        }
    }

    if (ok && !data.oom && nrows > 0) {
        c0_builder_init(&b);
        c0_build_group(&b, (const uint8_t *)(group_name ? group_name : "data"),
                       group_name ? strlen(group_name) : 4);
        for (size_t r = 0; r < nrows; r++) {
            size_t fstart = (r == 0) ? 0 : row_ends[r - 1];
            size_t fend = row_ends[r];
            size_t count = fend - fstart;
            c0_bytes *cells = (c0_bytes *)malloc((count ? count : 1) * sizeof(c0_bytes));
            if (!cells) { ok = 0; break; }
            for (size_t k = 0; k < count; k++) {
                cells[k].ptr = data.d + fields.v[fstart + k].start;
                cells[k].len = fields.v[fstart + k].len;
            }
            if (r == 0) {
                c0_build_headers(&b, cells, count);
            } else {
                c0_build_record(&b, cells, count);
            }
            free(cells);
        }
        if (ok && c0_builder_status(&b) == C0_BUILD_OK) {
            out = c0_builder_bytes(&b);
            result = (uint8_t *)malloc(out.len ? out.len : 1);
            if (result) {
                memcpy(result, out.ptr, out.len);
                if (out_len) *out_len = out.len;
            }
        }
        c0_builder_free(&b);
    }

    free(data.d);
    free(fields.v);
    free(row_ends);
    return result;
}

/* --- C0DATA → CSV --- */

static void csv_field(c0_strbuf *out, const uint8_t *p, size_t n) {
    int quote = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == ',' || p[i] == '"' || p[i] == '\n' || p[i] == '\r') {
            quote = 1;
            break;
        }
    }
    if (!quote) {
        c0_sb_raw(out, p, n);
        return;
    }
    c0_sb_byte(out, '"');
    for (i = 0; i < n; i++) {
        if (p[i] == '"') c0_sb_byte(out, '"');
        c0_sb_byte(out, p[i]);
    }
    c0_sb_byte(out, '"');
}

static c0_group find_table(const uint8_t *buf, size_t len) {
    if (len > 0 && buf[0] == C0_FS) {
        c0_doc_iter di = c0_doc(buf, len);
        c0_group g;
        if (c0_next_group(&di, &g)) return g;
    }
    return c0_table(buf, len);
}

char *c0_to_csv(const uint8_t *buf, size_t len, size_t *out_len) {
    c0_group g = find_table(buf, len);
    c0_strbuf out = {0, 0, 0, 0};
    c0_iter hi, ri;
    c0_bytes h, rec;
    int i, any_header = 0;

    hi = c0_group_headers(g);
    i = 0;
    while (c0_next_header(&hi, &h)) {
        if (i++) c0_sb_byte(&out, ',');
        csv_field(&out, h.ptr, h.len); /* header names carry no escapes */
        any_header = 1;
    }
    if (any_header) c0_sb_byte(&out, '\n');

    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        c0_field_iter fi = c0_record_fields(rec);
        c0_bytes f;
        i = 0;
        while (c0_next_field(&fi, &f)) {
            uint8_t *tmp;
            size_t tn;
            if (i++) c0_sb_byte(&out, ',');
            tmp = (uint8_t *)malloc(f.len ? f.len : 1);
            if (!tmp) {
                out.oom = 1;
                break;
            }
            tn = c0_unescape(f.ptr, f.len, tmp);
            csv_field(&out, tmp, tn);
            free(tmp);
        }
        c0_sb_byte(&out, '\n');
    }

    c0_sb_byte(&out, 0); /* NUL-terminate */
    if (out.oom) {
        free(out.d);
        return NULL;
    }
    if (out_len) *out_len = out.n - 1;
    return (char *)out.d;
}
