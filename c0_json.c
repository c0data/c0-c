#include "c0_json.h"
#include "c0_strbuf.h"

/* ===== Value tree ===== */

static c0_value *value_new(c0_value_kind k) {
    c0_value *v = (c0_value *)calloc(1, sizeof(c0_value));
    if (v) v->kind = k;
    return v;
}

c0_value *c0_value_str(const char *s, size_t len) {
    c0_value *v = value_new(C0_JSON_STR);
    if (!v) return NULL;
    v->s = (char *)malloc(len ? len : 1);
    if (!v->s) { free(v); return NULL; }
    if (len) memcpy(v->s, s, len);
    v->s_len = len;
    return v;
}

c0_value *c0_value_array(void) { return value_new(C0_JSON_ARRAY); }
c0_value *c0_value_object(void) { return value_new(C0_JSON_OBJECT); }

int c0_value_array_push(c0_value *arr, c0_value *item) {
    if (!item) return 0;
    if (arr->n_items == arr->cap_items) {
        size_t nc = arr->cap_items ? arr->cap_items * 2 : 4;
        c0_value **nv = (c0_value **)realloc(arr->items, nc * sizeof(c0_value *));
        if (!nv) return 0;
        arr->items = nv;
        arr->cap_items = nc;
    }
    arr->items[arr->n_items++] = item;
    return 1;
}

int c0_value_object_push(c0_value *obj, const char *key, size_t key_len, c0_value *val) {
    char *kc;
    if (!val) return 0;
    if (obj->n_pairs == obj->cap_pairs) {
        size_t nc = obj->cap_pairs ? obj->cap_pairs * 2 : 4;
        c0_pair *nv = (c0_pair *)realloc(obj->pairs, nc * sizeof(c0_pair));
        if (!nv) return 0;
        obj->pairs = nv;
        obj->cap_pairs = nc;
    }
    kc = (char *)malloc(key_len ? key_len : 1);
    if (!kc) return 0;
    if (key_len) memcpy(kc, key, key_len);
    obj->pairs[obj->n_pairs].key = kc;
    obj->pairs[obj->n_pairs].key_len = key_len;
    obj->pairs[obj->n_pairs].val = val;
    obj->n_pairs++;
    return 1;
}

void c0_value_free(c0_value *v) {
    size_t i;
    if (!v) return;
    switch (v->kind) {
        case C0_JSON_STR:
            free(v->s);
            break;
        case C0_JSON_ARRAY:
            for (i = 0; i < v->n_items; i++) c0_value_free(v->items[i]);
            free(v->items);
            break;
        case C0_JSON_OBJECT:
            for (i = 0; i < v->n_pairs; i++) {
                free(v->pairs[i].key);
                c0_value_free(v->pairs[i].val);
            }
            free(v->pairs);
            break;
    }
    free(v);
}

/* A string value from raw bytes, DLE-unescaped. */
static c0_value *value_unescaped(const uint8_t *b, size_t len) {
    uint8_t *tmp;
    size_t n;
    c0_value *v;
    tmp = (uint8_t *)malloc(len ? len : 1);
    if (!tmp) return NULL;
    n = c0_unescape(b, len, tmp);
    v = c0_value_str((const char *)tmp, n);
    free(tmp);
    return v;
}

/* ===== Export: C0DATA -> Value ===== */

typedef struct { c0_bytes *v; size_t n, cap; } bytes_vec;

static int bv_push(bytes_vec *a, c0_bytes b) {
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        c0_bytes *nv = (c0_bytes *)realloc(a->v, nc * sizeof(c0_bytes));
        if (!nv) return 0;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = b;
    return 1;
}

static int collect_fields(c0_bytes rec, bytes_vec *out) {
    c0_field_iter fi = c0_record_fields(rec);
    c0_bytes f;
    out->v = NULL; out->n = 0; out->cap = 0;
    while (c0_next_field(&fi, &f)) {
        if (!bv_push(out, f)) return 0;
    }
    return 1;
}

static c0_value *field_to_value(const uint8_t *field, size_t len);

/* parse a nested STX..ETX field into an object/array Value */
static size_t skip_nested(const uint8_t *buf, size_t pos, size_t stop) {
    int depth = 1;
    pos++; /* skip STX */
    while (pos < stop && depth > 0) {
        switch (buf[pos]) {
            case C0_STX: depth++; break;
            case C0_ETX: depth--; break;
            case C0_DLE: pos++; break;
            default: break;
        }
        pos++;
    }
    return pos;
}

static c0_value *parse_nested_kv(const uint8_t *f, size_t pos, size_t stop) {
    c0_value *obj = c0_value_object();
    if (!obj) return NULL;
    while (pos < stop) {
        if (f[pos] == C0_RS) {
            size_t key_start, key_stop;
            pos++;
            key_start = pos;
            while (pos < stop && f[pos] != C0_US) {
                if (f[pos] == C0_DLE) pos += 2; else pos++;
            }
            key_stop = pos < stop ? pos : stop;
            {
                uint8_t *ktmp = (uint8_t *)malloc((key_stop - key_start) ? (key_stop - key_start) : 1);
                size_t klen;
                c0_value *val;
                if (!ktmp) { c0_value_free(obj); return NULL; }
                klen = c0_unescape(f + key_start, key_stop - key_start, ktmp);
                if (pos < stop && f[pos] == C0_US) {
                    size_t val_start, val_stop;
                    pos++;
                    val_start = pos;
                    while (pos < stop && f[pos] != C0_RS) {
                        if (f[pos] == C0_STX) pos = skip_nested(f, pos, stop);
                        else if (f[pos] == C0_DLE) pos += 2;
                        else pos++;
                    }
                    val_stop = pos < stop ? pos : stop;
                    val = field_to_value(f + val_start, val_stop - val_start);
                } else {
                    val = c0_value_str("", 0);
                }
                if (!val || !c0_value_object_push(obj, (const char *)ktmp, klen, val)) {
                    c0_value_free(val); free(ktmp); c0_value_free(obj); return NULL;
                }
                free(ktmp);
            }
        } else {
            pos++;
        }
    }
    return obj;
}

static c0_value *parse_nested_array(const uint8_t *f, size_t pos, size_t stop) {
    c0_value *arr = c0_value_array();
    if (!arr) return NULL;
    while (pos < stop) {
        if (f[pos] == C0_US) {
            size_t item_start, item_stop;
            c0_value *item;
            pos++;
            item_start = pos;
            while (pos < stop && f[pos] != C0_US) {
                if (f[pos] == C0_STX) pos = skip_nested(f, pos, stop);
                else if (f[pos] == C0_DLE) pos += 2;
                else pos++;
            }
            item_stop = pos < stop ? pos : stop;
            item = field_to_value(f + item_start, item_stop - item_start);
            if (!item || !c0_value_array_push(arr, item)) {
                c0_value_free(item); c0_value_free(arr); return NULL;
            }
        } else {
            pos++;
        }
    }
    return arr;
}

static c0_value *parse_nested_field(const uint8_t *f, size_t len) {
    size_t stop = len, start, scan;
    int has_rs = 0;
    if (stop > 0 && f[stop - 1] == C0_ETX) stop--;
    start = len > 0 ? 1 : 0; /* skip STX */
    scan = start;
    while (scan < stop) {
        if (f[scan] == C0_RS) { has_rs = 1; break; }
        if (f[scan] == C0_STX) scan = skip_nested(f, scan, stop);
        else if (f[scan] == C0_DLE) scan += 2;
        else scan++;
    }
    return has_rs ? parse_nested_kv(f, start, stop) : parse_nested_array(f, start, stop);
}

static c0_value *field_to_value(const uint8_t *field, size_t len) {
    if (len > 0 && field[0] == C0_STX) return parse_nested_field(field, len);
    return value_unescaped(field, len);
}

static c0_value *export_group_data(c0_group g);

static c0_value *export_table(c0_group g, bytes_vec *headers) {
    c0_value *rows = c0_value_array();
    c0_iter ri;
    c0_bytes rec;
    if (!rows) return NULL;
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        bytes_vec fields;
        c0_value *row = c0_value_object();
        size_t i;
        if (!row) { c0_value_free(rows); return NULL; }
        if (!collect_fields(rec, &fields)) { free(fields.v); c0_value_free(row); c0_value_free(rows); return NULL; }
        for (i = 0; i < headers->n; i++) {
            c0_value *cell;
            if (i < fields.n) cell = field_to_value(fields.v[i].ptr, fields.v[i].len);
            else cell = c0_value_str("", 0);
            if (!cell || !c0_value_object_push(row, (const char *)headers->v[i].ptr, headers->v[i].len, cell)) {
                c0_value_free(cell); free(fields.v); c0_value_free(row); c0_value_free(rows); return NULL;
            }
        }
        free(fields.v);
        if (!c0_value_array_push(rows, row)) { c0_value_free(row); c0_value_free(rows); return NULL; }
    }
    return rows;
}

static c0_value *export_kv(c0_group g) {
    c0_value *obj = c0_value_object();
    c0_iter ri;
    c0_bytes rec;
    if (!obj) return NULL;
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        bytes_vec fields;
        uint8_t *ktmp;
        size_t klen;
        c0_value *val;
        if (!collect_fields(rec, &fields)) { free(fields.v); c0_value_free(obj); return NULL; }
        ktmp = (uint8_t *)malloc(fields.n > 0 && fields.v[0].len ? fields.v[0].len : 1);
        if (!ktmp) { free(fields.v); c0_value_free(obj); return NULL; }
        klen = fields.n > 0 ? c0_unescape(fields.v[0].ptr, fields.v[0].len, ktmp) : 0;
        val = fields.n > 1 ? field_to_value(fields.v[1].ptr, fields.v[1].len) : c0_value_str("", 0);
        if (!val || !c0_value_object_push(obj, (const char *)ktmp, klen, val)) {
            c0_value_free(val); free(ktmp); free(fields.v); c0_value_free(obj); return NULL;
        }
        free(ktmp);
        free(fields.v);
    }
    return obj;
}

static c0_value *export_records(c0_group g) {
    c0_value *rows = c0_value_array();
    c0_iter ri;
    c0_bytes rec;
    if (!rows) return NULL;
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        bytes_vec fields;
        c0_value *row = c0_value_array();
        size_t i;
        if (!row) { c0_value_free(rows); return NULL; }
        if (!collect_fields(rec, &fields)) { free(fields.v); c0_value_free(row); c0_value_free(rows); return NULL; }
        for (i = 0; i < fields.n; i++) {
            c0_value *cell = field_to_value(fields.v[i].ptr, fields.v[i].len);
            if (!cell || !c0_value_array_push(row, cell)) {
                c0_value_free(cell); free(fields.v); c0_value_free(row); c0_value_free(rows); return NULL;
            }
        }
        free(fields.v);
        if (!c0_value_array_push(rows, row)) { c0_value_free(row); c0_value_free(rows); return NULL; }
    }
    return rows;
}

static c0_value *export_group_data(c0_group g) {
    bytes_vec headers;
    c0_iter hi;
    c0_bytes h;
    c0_iter ri;
    c0_bytes rec;
    int has_records = 0;
    size_t rec0_fields = 0;
    c0_value *result;

    headers.v = NULL; headers.n = 0; headers.cap = 0;
    hi = c0_group_headers(g);
    while (c0_next_header(&hi, &h)) {
        if (!bv_push(&headers, h)) { free(headers.v); return NULL; }
    }
    if (headers.n > 0) {
        result = export_table(g, &headers);
        free(headers.v);
        return result;
    }
    free(headers.v);

    /* Inspect the first record to choose key-value vs raw records. */
    ri = c0_group_records(g);
    if (c0_next_record(&ri, &rec)) {
        c0_field_iter fi = c0_record_fields(rec);
        c0_bytes f;
        has_records = 1;
        while (c0_next_field(&fi, &f)) rec0_fields++;
    }
    if (has_records && rec0_fields == 2) return export_kv(g);
    if (has_records) return export_records(g);
    return c0_value_array();
}

static c0_value *export_document(const uint8_t *buf, size_t len) {
    c0_bytes name = c0_doc_name(buf, len);
    c0_value *groups = c0_value_object();
    c0_doc_iter di;
    c0_group g;
    if (!groups) return NULL;
    di = c0_doc(buf, len);
    while (c0_next_group(&di, &g)) {
        c0_bytes gn = c0_group_name(g);
        c0_value *data = export_group_data(g);
        if (!data || !c0_value_object_push(groups, (const char *)gn.ptr, gn.len, data)) {
            c0_value_free(data); c0_value_free(groups); return NULL;
        }
    }
    if (name.len == 0) return groups;
    {
        c0_value *wrap = c0_value_object();
        if (!wrap || !c0_value_object_push(wrap, (const char *)name.ptr, name.len, groups)) {
            c0_value_free(wrap); c0_value_free(groups); return NULL;
        }
        return wrap;
    }
}

c0_value *c0_to_value(const uint8_t *buf, size_t len) {
    if (len > 0 && buf[0] == C0_FS) return export_document(buf, len);
    if (len > 0 && buf[0] == C0_GS) {
        c0_group g = c0_table(buf, len);
        c0_bytes gn = c0_group_name(g);
        c0_value *obj = c0_value_object();
        c0_value *data;
        if (!obj) return NULL;
        data = export_group_data(g);
        if (!data || !c0_value_object_push(obj, (const char *)gn.ptr, gn.len, data)) {
            c0_value_free(data); c0_value_free(obj); return NULL;
        }
        return obj;
    }
    return c0_value_object();
}

/* ===== Import: Value -> C0DATA ===== */

static void write_escaped(c0_strbuf *sb, const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x20) c0_sb_byte(sb, C0_DLE);
        c0_sb_byte(sb, b);
    }
}

static void write_group(c0_strbuf *sb, const char *name, size_t len) {
    c0_sb_byte(sb, C0_GS);
    c0_sb_raw(sb, (const uint8_t *)name, len);
}

static const char *as_str(const c0_value *v, size_t *len) {
    if (v->kind == C0_JSON_STR) { *len = v->s_len; return v->s; }
    *len = 0;
    return "";
}

static int all_scalar(const c0_value *obj) {
    size_t i;
    for (i = 0; i < obj->n_pairs; i++)
        if (obj->pairs[i].val->kind != C0_JSON_STR) return 0;
    return 1;
}

static int all_groupable(const c0_value *obj) {
    size_t i;
    for (i = 0; i < obj->n_pairs; i++)
        if (obj->pairs[i].val->kind == C0_JSON_STR) return 0;
    return 1;
}

/* Do all array items share the first object's key sequence? */
static int tabular(const c0_value *arr) {
    const c0_value *first;
    size_t i, j;
    if (arr->n_items == 0) return 0;
    first = arr->items[0];
    if (first->kind != C0_JSON_OBJECT) return 0;
    for (i = 0; i < arr->n_items; i++) {
        const c0_value *it = arr->items[i];
        if (it->kind != C0_JSON_OBJECT) return 0;
        if (it->n_pairs != first->n_pairs) return 0;
        for (j = 0; j < it->n_pairs; j++) {
            if (it->pairs[j].key_len != first->pairs[j].key_len ||
                memcmp(it->pairs[j].key, first->pairs[j].key, it->pairs[j].key_len) != 0)
                return 0;
        }
    }
    return 1;
}

static void emit_field_value(const c0_value *v, c0_strbuf *sb);
static void emit_array_as_group(const c0_value *arr, const char *name, size_t nlen, c0_strbuf *sb);
static void emit_hash_as_groups(const c0_value *obj, c0_strbuf *sb);

static void emit_field_value(const c0_value *v, c0_strbuf *sb) {
    size_t i;
    switch (v->kind) {
        case C0_JSON_STR:
            write_escaped(sb, v->s, v->s_len);
            break;
        case C0_JSON_OBJECT:
            c0_sb_byte(sb, C0_STX);
            for (i = 0; i < v->n_pairs; i++) {
                c0_sb_byte(sb, C0_RS);
                write_escaped(sb, v->pairs[i].key, v->pairs[i].key_len);
                c0_sb_byte(sb, C0_US);
                emit_field_value(v->pairs[i].val, sb);
            }
            c0_sb_byte(sb, C0_ETX);
            break;
        case C0_JSON_ARRAY:
            c0_sb_byte(sb, C0_STX);
            for (i = 0; i < v->n_items; i++) {
                c0_sb_byte(sb, C0_US);
                emit_field_value(v->items[i], sb);
            }
            c0_sb_byte(sb, C0_ETX);
            break;
    }
}

static void emit_array_as_group(const c0_value *arr, const char *name, size_t nlen, c0_strbuf *sb) {
    size_t i, j;
    if (tabular(arr)) {
        const c0_value *first = arr->items[0];
        write_group(sb, name, nlen);
        c0_sb_byte(sb, C0_SOH);
        for (i = 0; i < first->n_pairs; i++) {
            if (i > 0) c0_sb_byte(sb, C0_US);
            c0_sb_raw(sb, (const uint8_t *)first->pairs[i].key, first->pairs[i].key_len);
        }
        for (i = 0; i < arr->n_items; i++) {
            const c0_value *row = arr->items[i];
            c0_sb_byte(sb, C0_RS);
            for (j = 0; j < first->n_pairs; j++) {
                size_t k;
                const c0_value *cell = NULL;
                if (j > 0) c0_sb_byte(sb, C0_US);
                for (k = 0; k < row->n_pairs; k++) {
                    if (row->pairs[k].key_len == first->pairs[j].key_len &&
                        memcmp(row->pairs[k].key, first->pairs[j].key, first->pairs[j].key_len) == 0) {
                        cell = row->pairs[k].val;
                        break;
                    }
                }
                if (cell) emit_field_value(cell, sb);
            }
        }
    } else {
        write_group(sb, name, nlen);
        for (i = 0; i < arr->n_items; i++) {
            const c0_value *item = arr->items[i];
            if (item->kind == C0_JSON_STR) {
                c0_sb_byte(sb, C0_RS);
                write_escaped(sb, item->s, item->s_len);
            } else if (item->kind == C0_JSON_ARRAY) {
                c0_sb_byte(sb, C0_RS);
                for (j = 0; j < item->n_items; j++) {
                    if (j > 0) c0_sb_byte(sb, C0_US);
                    emit_field_value(item->items[j], sb);
                }
            } else { /* object */
                for (j = 0; j < item->n_pairs; j++) {
                    c0_sb_byte(sb, C0_RS);
                    write_escaped(sb, item->pairs[j].key, item->pairs[j].key_len);
                    c0_sb_byte(sb, C0_US);
                    emit_field_value(item->pairs[j].val, sb);
                }
            }
        }
    }
}

static void emit_hash_as_groups(const c0_value *obj, c0_strbuf *sb) {
    size_t i, k;
    for (i = 0; i < obj->n_pairs; i++) {
        const char *name = obj->pairs[i].key;
        size_t nlen = obj->pairs[i].key_len;
        const c0_value *value = obj->pairs[i].val;
        if (value->kind == C0_JSON_OBJECT) {
            int scalar = all_scalar(value);
            write_group(sb, name, nlen);
            for (k = 0; k < value->n_pairs; k++) {
                c0_sb_byte(sb, C0_RS);
                write_escaped(sb, value->pairs[k].key, value->pairs[k].key_len);
                c0_sb_byte(sb, C0_US);
                if (scalar) {
                    size_t sl; const char *s = as_str(value->pairs[k].val, &sl);
                    write_escaped(sb, s, sl);
                } else {
                    emit_field_value(value->pairs[k].val, sb);
                }
            }
        } else if (value->kind == C0_JSON_ARRAY) {
            emit_array_as_group(value, name, nlen, sb);
        } else {
            size_t sl; const char *s;
            write_group(sb, name, nlen);
            c0_sb_byte(sb, C0_RS);
            s = as_str(value, &sl);
            write_escaped(sb, s, sl);
        }
    }
}

static void emit_root(const c0_value *value, const char *group_name, size_t gnlen, c0_strbuf *sb) {
    size_t k;
    if (value->kind == C0_JSON_OBJECT) {
        if (all_scalar(value)) {
            write_group(sb, group_name, gnlen);
            for (k = 0; k < value->n_pairs; k++) {
                size_t sl; const char *s;
                c0_sb_byte(sb, C0_RS);
                write_escaped(sb, value->pairs[k].key, value->pairs[k].key_len);
                c0_sb_byte(sb, C0_US);
                s = as_str(value->pairs[k].val, &sl);
                write_escaped(sb, s, sl);
            }
        } else if (value->n_pairs == 1) {
            const c0_value *inner = value->pairs[0].val;
            if (inner->kind == C0_JSON_OBJECT && all_groupable(inner)) {
                c0_sb_byte(sb, C0_FS);
                c0_sb_raw(sb, (const uint8_t *)value->pairs[0].key, value->pairs[0].key_len);
                emit_hash_as_groups(inner, sb);
            } else {
                emit_hash_as_groups(value, sb);
            }
        } else {
            emit_hash_as_groups(value, sb);
        }
    } else if (value->kind == C0_JSON_ARRAY) {
        emit_array_as_group(value, group_name, gnlen, sb);
    } else {
        size_t sl; const char *s;
        write_group(sb, group_name, gnlen);
        c0_sb_byte(sb, C0_RS);
        s = as_str(value, &sl);
        write_escaped(sb, s, sl);
    }
}

uint8_t *c0_from_value(const c0_value *v, const char *group_name, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    const char *gn = group_name ? group_name : "data";
    size_t gnlen = strlen(gn);
    emit_root(v, gn, gnlen, &sb);
    if (sb.oom) { free(sb.d); return NULL; }
    if (!sb.d) sb.d = (uint8_t *)malloc(1);
    if (!sb.d) return NULL;
    if (out_len) *out_len = sb.n;
    return sb.d;
}

/* ===== JSON text: parser ===== */

typedef struct { const char *s; size_t len, pos; } jparse;

static void jp_ws(jparse *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static c0_value *jp_value(jparse *p);

static void utf8_encode(c0_strbuf *sb, unsigned cp) {
    if (cp < 0x80) {
        c0_sb_byte(sb, (uint8_t)cp);
    } else if (cp < 0x800) {
        c0_sb_byte(sb, (uint8_t)(0xC0 | (cp >> 6)));
        c0_sb_byte(sb, (uint8_t)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        c0_sb_byte(sb, (uint8_t)(0xE0 | (cp >> 12)));
        c0_sb_byte(sb, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        c0_sb_byte(sb, (uint8_t)(0x80 | (cp & 0x3F)));
    } else {
        c0_sb_byte(sb, (uint8_t)(0xF0 | (cp >> 18)));
        c0_sb_byte(sb, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
        c0_sb_byte(sb, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
        c0_sb_byte(sb, (uint8_t)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Parse a JSON string starting at the opening quote; returns decoded bytes in
 * sb (caller-owned), advances p past the closing quote. Returns 0 on error. */
static int jp_string_raw(jparse *p, c0_strbuf *sb) {
    if (p->pos >= p->len || p->s[p->pos] != '"') return 0;
    p->pos++;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == '"') { p->pos++; return !sb->oom; }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) return 0;
            c = p->s[p->pos];
            switch (c) {
                case '"': c0_sb_byte(sb, '"'); p->pos++; break;
                case '\\': c0_sb_byte(sb, '\\'); p->pos++; break;
                case '/': c0_sb_byte(sb, '/'); p->pos++; break;
                case 'b': c0_sb_byte(sb, '\b'); p->pos++; break;
                case 'f': c0_sb_byte(sb, '\f'); p->pos++; break;
                case 'n': c0_sb_byte(sb, '\n'); p->pos++; break;
                case 'r': c0_sb_byte(sb, '\r'); p->pos++; break;
                case 't': c0_sb_byte(sb, '\t'); p->pos++; break;
                case 'u': {
                    unsigned cp;
                    if (p->pos + 5 > p->len || !hex4(p->s + p->pos + 1, &cp)) return 0;
                    p->pos += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF) { /* high surrogate */
                        unsigned lo;
                        if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u'
                            && hex4(p->s + p->pos + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p->pos += 6;
                        }
                    }
                    utf8_encode(sb, cp);
                    break;
                }
                default: return 0;
            }
        } else {
            c0_sb_byte(sb, (uint8_t)c);
            p->pos++;
        }
    }
    return 0; /* unterminated */
}

static c0_value *jp_string(jparse *p) {
    c0_strbuf sb = {0, 0, 0, 0};
    c0_value *v;
    if (!jp_string_raw(p, &sb)) { free(sb.d); return NULL; }
    v = c0_value_str(sb.d ? (const char *)sb.d : "", sb.n);
    free(sb.d);
    return v;
}

static c0_value *jp_object(jparse *p) {
    c0_value *obj = c0_value_object();
    if (!obj) return NULL;
    p->pos++; /* { */
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
    for (;;) {
        c0_strbuf key = {0, 0, 0, 0};
        c0_value *val;
        jp_ws(p);
        if (!jp_string_raw(p, &key)) { free(key.d); c0_value_free(obj); return NULL; }
        jp_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { free(key.d); c0_value_free(obj); return NULL; }
        p->pos++;
        val = jp_value(p);
        if (!val) { free(key.d); c0_value_free(obj); return NULL; }
        if (!c0_value_object_push(obj, key.d ? (const char *)key.d : "", key.n, val)) {
            free(key.d); c0_value_free(val); c0_value_free(obj); return NULL;
        }
        free(key.d);
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
        c0_value_free(obj);
        return NULL;
    }
}

static c0_value *jp_array(jparse *p) {
    c0_value *arr = c0_value_array();
    if (!arr) return NULL;
    p->pos++; /* [ */
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
    for (;;) {
        c0_value *item = jp_value(p);
        if (!item) { c0_value_free(arr); return NULL; }
        if (!c0_value_array_push(arr, item)) { c0_value_free(item); c0_value_free(arr); return NULL; }
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
        c0_value_free(arr);
        return NULL;
    }
}

static int lit(jparse *p, const char *word) {
    size_t n = strlen(word);
    if (p->pos + n <= p->len && memcmp(p->s + p->pos, word, n) == 0) { p->pos += n; return 1; }
    return 0;
}

static c0_value *jp_number(jparse *p) {
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
            p->pos++;
        else break;
    }
    if (p->pos == start) return NULL;
    return c0_value_str(p->s + start, p->pos - start);
}

static c0_value *jp_value(jparse *p) {
    char c;
    jp_ws(p);
    if (p->pos >= p->len) return NULL;
    c = p->s[p->pos];
    switch (c) {
        case '{': return jp_object(p);
        case '[': return jp_array(p);
        case '"': return jp_string(p);
        case 't': return lit(p, "true") ? c0_value_str("true", 4) : NULL;
        case 'f': return lit(p, "false") ? c0_value_str("false", 5) : NULL;
        case 'n': return lit(p, "null") ? c0_value_str("", 0) : NULL;
        default:  return jp_number(p);
    }
}

c0_value *c0_json_parse(const char *json, size_t len) {
    jparse p;
    c0_value *v;
    p.s = json; p.len = len; p.pos = 0;
    v = jp_value(&p);
    if (!v) return NULL;
    jp_ws(&p);
    if (p.pos != p.len) { c0_value_free(v); return NULL; } /* trailing junk */
    return v;
}

/* ===== JSON text: serializer (serde-style 2-space pretty) ===== */

static void json_str(c0_strbuf *sb, const char *s, size_t len) {
    static const char *hex = "0123456789abcdef";
    size_t i;
    c0_sb_byte(sb, '"');
    for (i = 0; i < len; i++) {
        uint8_t b = (uint8_t)s[i];
        switch (b) {
            case '"':  c0_sb_cstr(sb, "\\\""); break;
            case '\\': c0_sb_cstr(sb, "\\\\"); break;
            case '\b': c0_sb_cstr(sb, "\\b"); break;
            case '\f': c0_sb_cstr(sb, "\\f"); break;
            case '\n': c0_sb_cstr(sb, "\\n"); break;
            case '\r': c0_sb_cstr(sb, "\\r"); break;
            case '\t': c0_sb_cstr(sb, "\\t"); break;
            default:
                if (b < 0x20) {
                    c0_sb_cstr(sb, "\\u00");
                    c0_sb_byte(sb, (uint8_t)hex[(b >> 4) & 0xF]);
                    c0_sb_byte(sb, (uint8_t)hex[b & 0xF]);
                } else {
                    c0_sb_byte(sb, b);
                }
        }
    }
    c0_sb_byte(sb, '"');
}

static void indent(c0_strbuf *sb, int depth) {
    int i;
    for (i = 0; i < depth * 2; i++) c0_sb_byte(sb, ' ');
}

static void json_emit(c0_strbuf *sb, const c0_value *v, int depth) {
    size_t i;
    switch (v->kind) {
        case C0_JSON_STR:
            json_str(sb, v->s, v->s_len);
            break;
        case C0_JSON_ARRAY:
            if (v->n_items == 0) { c0_sb_cstr(sb, "[]"); break; }
            c0_sb_cstr(sb, "[\n");
            for (i = 0; i < v->n_items; i++) {
                indent(sb, depth + 1);
                json_emit(sb, v->items[i], depth + 1);
                if (i + 1 < v->n_items) c0_sb_byte(sb, ',');
                c0_sb_byte(sb, '\n');
            }
            indent(sb, depth);
            c0_sb_byte(sb, ']');
            break;
        case C0_JSON_OBJECT:
            if (v->n_pairs == 0) { c0_sb_cstr(sb, "{}"); break; }
            c0_sb_cstr(sb, "{\n");
            for (i = 0; i < v->n_pairs; i++) {
                indent(sb, depth + 1);
                json_str(sb, v->pairs[i].key, v->pairs[i].key_len);
                c0_sb_cstr(sb, ": ");
                json_emit(sb, v->pairs[i].val, depth + 1);
                if (i + 1 < v->n_pairs) c0_sb_byte(sb, ',');
                c0_sb_byte(sb, '\n');
            }
            indent(sb, depth);
            c0_sb_byte(sb, '}');
            break;
    }
}

char *c0_json_print(const c0_value *v, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    json_emit(&sb, v, 0);
    c0_sb_byte(&sb, 0);
    if (sb.oom) { free(sb.d); return NULL; }
    if (out_len) *out_len = sb.n - 1;
    return (char *)sb.d;
}

char *c0_to_json(const uint8_t *buf, size_t len, size_t *out_len) {
    c0_value *v = c0_to_value(buf, len);
    char *out;
    if (!v) return NULL;
    out = c0_json_print(v, out_len);
    c0_value_free(v);
    return out;
}

uint8_t *c0_from_json(const char *json, size_t len, const char *group_name, size_t *out_len) {
    c0_value *v = c0_json_parse(json, len);
    uint8_t *out;
    if (!v) return NULL;
    out = c0_from_value(v, group_name, out_len);
    c0_value_free(v);
    return out;
}
