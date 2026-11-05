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
    size_t i, r;
    c0_builder b;
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
        for (r = 0; r < nrows; r++) {
            size_t fstart = (r == 0) ? 0 : row_ends[r - 1];
            size_t fend = row_ends[r];
            size_t count = fend - fstart, k;
            c0_bytes *cells = (c0_bytes *)malloc((count ? count : 1) * sizeof(c0_bytes));
            if (!cells) { ok = 0; break; }
            for (k = 0; k < count; k++) {
                cells[k].ptr = data.d + fields.v[fstart + k].start;
                cells[k].len = fields.v[fstart + k].len;
            }
            if (r == 0) c0_build_header(&b, cells, count);
            else c0_build_record(&b, cells, count);
            free(cells);
        }
        if (ok && c0_builder_status(&b) == C0_OK) {
            c0_bytes out = c0_builder_bytes(&b);
            result = (uint8_t *)malloc(out.len ? out.len : 1);
            if (result) {
                if (out.len) memcpy(result, out.ptr, out.len);
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

static void fail(c0_error *err, c0_status code, const char *msg) {
    if (err && err->code == C0_OK) {
        err->code = code;
        err->pos = 0;
        err->msg = msg;
    }
}

/* Write one unit: its value, or a reference's path. 0 on error. */
static int csv_unit(c0_strbuf *out, c0_bytes raw, c0_error *err) {
    c0_bytes v;
    uint8_t *scratch;
    c0_status st;
    switch (c0_unit_type(raw)) {
        case C0_UNIT_LEVEL:
            fail(err, C0_ERR_SYNTAX, "CSV cannot carry a nested level");
            return 0;
        case C0_UNIT_REF: {
            c0_ref r;
            size_t i;
            if (c0_unit_ref(raw, &r) != C0_OK) {
                fail(err, C0_ERR_SYNTAX, "malformed reference");
                return 0;
            }
            scratch = (uint8_t *)malloc(raw.len ? raw.len : 1);
            if (!scratch) { fail(err, C0_ERR_OOM, "out of memory"); return 0; }
            {
                size_t n = 0;
                for (i = 0; i < r.count; i++) {
                    if (i) scratch[n++] = '/';
                    memcpy(scratch + n, r.seg[i].ptr, r.seg[i].len);
                    n += r.seg[i].len;
                }
                csv_field(out, scratch, n);
            }
            free(scratch);
            return 1;
        }
        default:
            scratch = (uint8_t *)malloc(raw.len ? raw.len : 1);
            if (!scratch) { fail(err, C0_ERR_OOM, "out of memory"); return 0; }
            st = c0_value(raw, scratch, &v);
            if (st != C0_OK) {
                free(scratch);
                fail(err, st, "malformed value");
                return 0;
            }
            csv_field(out, v.ptr, v.len);
            free(scratch);
            return 1;
    }
}

static int csv_record(c0_strbuf *out, c0_record rec, c0_error *err) {
    c0_unit_iter ui = c0_record_units(rec);
    c0_bytes raw;
    int i = 0;
    while (c0_next_unit(&ui, &raw)) {
        if (i++) c0_sb_byte(out, ',');
        if (!csv_unit(out, raw, err)) return 0;
    }
    if (ui.err.code) { fail(err, ui.err.code, ui.err.msg); return 0; }
    c0_sb_byte(out, '\n');
    return 1;
}

char *c0_to_csv(const uint8_t *buf, size_t len, const char *group_name, size_t *out_len, c0_error *err) {
    c0_document doc;
    c0_group g;
    c0_record h, rec;
    c0_record_iter ri;
    c0_strbuf out = {0, 0, 0, 0};
    c0_error local = {C0_OK, 0, ""};
    if (!err) err = &local;
    err->code = C0_OK;
    err->pos = 0;
    err->msg = "";

    if (c0_document_read(buf, len, 0, &doc, err) != C0_OK) return NULL;
    if (group_name) {
        if (c0_body_find_group_str(doc.body, group_name, &g) != C0_OK) {
            fail(err, C0_ERR_NOT_FOUND, "no such group");
            return NULL;
        }
    } else {
        c0_group_iter gi = c0_body_groups(doc.body);
        if (!c0_next_group(&gi, &g)) {
            fail(err, gi.err.code ? gi.err.code : C0_ERR_NOT_FOUND, gi.err.code ? gi.err.msg : "no group in buffer");
            return NULL;
        }
    }
    if (c0_group_header(g, &h) && !csv_record(&out, h, err)) goto bad;
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        if (!csv_record(&out, rec, err)) goto bad;
    }
    if (ri.err.code) { fail(err, ri.err.code, ri.err.msg); goto bad; }
    c0_sb_byte(&out, 0);
    if (out.oom) { fail(err, C0_ERR_OOM, "out of memory"); goto bad; }
    if (out_len) *out_len = out.n - 1;
    return (char *)out.d;
bad:
    free(out.d);
    return NULL;
}
