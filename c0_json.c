#include "c0_json.h"
#include "c0_strbuf.h"

/* ===== Value tree ===== */

static c0_json_value *value_new(c0_json_kind k) {
    c0_json_value *v = (c0_json_value *)calloc(1, sizeof(c0_json_value));
    if (v) v->kind = k;
    return v;
}

c0_json_value *c0_json_str(const char *s, size_t len) {
    c0_json_value *v = value_new(C0_JSON_STR);
    if (!v) return NULL;
    v->s = (char *)malloc(len ? len : 1);
    if (!v->s) { free(v); return NULL; }
    if (len) memcpy(v->s, s, len);
    v->s_len = len;
    return v;
}

c0_json_value *c0_json_array(void) { return value_new(C0_JSON_ARRAY); }
c0_json_value *c0_json_object(void) { return value_new(C0_JSON_OBJECT); }

int c0_json_array_push(c0_json_value *arr, c0_json_value *item) {
    if (!item) return 0;
    if (arr->n_items == arr->cap_items) {
        size_t nc = arr->cap_items ? arr->cap_items * 2 : 4;
        c0_json_value **nv = (c0_json_value **)realloc(arr->items, nc * sizeof(c0_json_value *));
        if (!nv) return 0;
        arr->items = nv;
        arr->cap_items = nc;
    }
    arr->items[arr->n_items++] = item;
    return 1;
}

int c0_json_object_push(c0_json_value *obj, const char *key, size_t key_len, c0_json_value *val) {
    char *kc;
    if (!val) return 0;
    if (obj->n_pairs == obj->cap_pairs) {
        size_t nc = obj->cap_pairs ? obj->cap_pairs * 2 : 4;
        c0_json_pair *nv = (c0_json_pair *)realloc(obj->pairs, nc * sizeof(c0_json_pair));
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

/* Set a key: replace an existing entry's value (last wins) or append. */
static int object_set(c0_json_value *obj, const uint8_t *key, size_t key_len, c0_json_value *val) {
    size_t i;
    if (!val) return 0;
    for (i = 0; i < obj->n_pairs; i++) {
        if (obj->pairs[i].key_len == key_len && (key_len == 0 || memcmp(obj->pairs[i].key, key, key_len) == 0)) {
            c0_json_free(obj->pairs[i].val);
            obj->pairs[i].val = val;
            return 1;
        }
    }
    return c0_json_object_push(obj, (const char *)key, key_len, val);
}

void c0_json_free(c0_json_value *v) {
    size_t i;
    if (!v) return;
    switch (v->kind) {
        case C0_JSON_STR:
            free(v->s);
            break;
        case C0_JSON_ARRAY:
            for (i = 0; i < v->n_items; i++) c0_json_free(v->items[i]);
            free(v->items);
            break;
        case C0_JSON_OBJECT:
            for (i = 0; i < v->n_pairs; i++) {
                free(v->pairs[i].key);
                c0_json_free(v->pairs[i].val);
            }
            free(v->pairs);
            break;
    }
    free(v);
}

/* ===== Export: C0DATA -> value ===== */

typedef struct {
    c0_error *err;
} exp;

static void efail(exp *e, c0_status code, const char *msg) {
    if (e->err && e->err->code == C0_OK) {
        e->err->code = code;
        e->err->pos = 0;
        e->err->msg = msg;
    }
}

static c0_json_value *export_body(exp *e, c0_body body, int level);

/* A scalar unit as a string value (decoded). */
static c0_json_value *export_scalar(exp *e, c0_bytes raw) {
    uint8_t *scratch = (uint8_t *)malloc(raw.len ? raw.len : 1);
    c0_bytes v;
    c0_status st;
    c0_json_value *out;
    if (!scratch) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
    st = c0_value(raw, scratch, &v);
    if (st != C0_OK) {
        free(scratch);
        efail(e, st, "malformed value");
        return NULL;
    }
    out = c0_json_str((const char *)v.ptr, v.len);
    free(scratch);
    if (!out) efail(e, C0_ERR_OOM, "out of memory");
    return out;
}

static c0_json_value *export_raw(exp *e, c0_bytes raw) {
    switch (c0_unit_type(raw)) {
        case C0_UNIT_LEVEL: {
            c0_body lv;
            c0_status st = c0_unit_level(raw, &lv);
            if (st) { efail(e, st, "malformed level"); return NULL; }
            return export_body(e, lv, 1);
        }
        case C0_UNIT_REF: {
            c0_ref r;
            c0_strbuf sb = {0, 0, 0, 0};
            c0_json_value *obj, *s;
            size_t i;
            if (c0_unit_ref(raw, &r) != C0_OK) { efail(e, C0_ERR_SYNTAX, "malformed reference"); return NULL; }
            for (i = 0; i < r.count; i++) {
                if (i) c0_sb_byte(&sb, '/');
                c0_sb_raw(&sb, r.seg[i].ptr, r.seg[i].len);
            }
            obj = c0_json_object();
            s = sb.oom ? NULL : c0_json_str(sb.d ? (const char *)sb.d : "", sb.n);
            free(sb.d);
            if (!obj || !s || !c0_json_object_push(obj, "$ref", 4, s)) {
                c0_json_free(obj);
                c0_json_free(s);
                efail(e, C0_ERR_OOM, "out of memory");
                return NULL;
            }
            return obj;
        }
        default:
            return export_scalar(e, raw);
    }
}

static c0_json_value *export_record(exp *e, c0_record rec) {
    if (c0_record_keyed(rec)) {
        c0_json_value *obj = c0_json_object();
        c0_pair_iter pi = c0_record_pairs(rec);
        c0_bytes k, raw;
        if (!obj) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
        while (c0_next_pair(&pi, &k, &raw)) {
            c0_json_value *v = export_raw(e, raw);
            if (!v || !object_set(obj, k.ptr, k.len, v)) {
                c0_json_free(v);
                c0_json_free(obj);
                efail(e, C0_ERR_OOM, "out of memory");
                return NULL;
            }
        }
        if (pi.err.code) { efail(e, pi.err.code, pi.err.msg); c0_json_free(obj); return NULL; }
        return obj;
    } else {
        c0_json_value *arr = c0_json_array();
        c0_unit_iter ui = c0_record_units(rec);
        c0_bytes raw;
        if (!arr) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
        while (c0_next_unit(&ui, &raw)) {
            c0_json_value *v = export_raw(e, raw);
            if (!v || !c0_json_array_push(arr, v)) {
                c0_json_free(v);
                c0_json_free(arr);
                efail(e, C0_ERR_OOM, "out of memory");
                return NULL;
            }
        }
        if (ui.err.code) { efail(e, ui.err.code, ui.err.msg); c0_json_free(arr); return NULL; }
        return arr;
    }
}

/* A group: with a header row an array of objects; a single keyed record
 * (or a single record inside a level) is the record itself; otherwise an
 * array of its records. */
static c0_json_value *export_group(exp *e, c0_group g, int level) {
    c0_record header, rec;
    c0_record_iter ri;
    c0_json_value *out;
    if (c0_group_header(g, &header)) {
        c0_json_value *rows = c0_json_array();
        if (!rows) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
        ri = c0_group_records(g);
        while (c0_next_record(&ri, &rec)) {
            c0_json_value *row;
            if (c0_record_keyed(rec)) {
                row = export_record(e, rec);
            } else {
                c0_unit_iter hi = c0_record_units(header);
                c0_bytes hraw, hname, raw;
                size_t i = 0, n = c0_record_count(rec);
                row = c0_json_object();
                if (!row) { efail(e, C0_ERR_OOM, "out of memory"); c0_json_free(rows); return NULL; }
                while (c0_next_unit(&hi, &hraw)) {
                    c0_json_value *cell;
                    if (c0_value(hraw, NULL, &hname) != C0_OK) { efail(e, C0_ERR_SYNTAX, "malformed header"); c0_json_free(row); c0_json_free(rows); return NULL; }
                    if (i < n && c0_record_field(rec, i, &raw) == C0_OK) cell = export_raw(e, raw);
                    else cell = c0_json_str("", 0);
                    if (!cell || !object_set(row, hname.ptr, hname.len, cell)) {
                        c0_json_free(cell); c0_json_free(row); c0_json_free(rows);
                        efail(e, C0_ERR_OOM, "out of memory");
                        return NULL;
                    }
                    i++;
                }
            }
            if (!row || !c0_json_array_push(rows, row)) {
                c0_json_free(row); c0_json_free(rows);
                efail(e, C0_ERR_OOM, "out of memory");
                return NULL;
            }
        }
        if (ri.err.code) { efail(e, ri.err.code, ri.err.msg); c0_json_free(rows); return NULL; }
        return rows;
    }
    {
        size_t n = 0;
        c0_record first;
        ri = c0_group_records(g);
        while (c0_next_record(&ri, &rec)) {
            if (n == 0) first = rec;
            n++;
        }
        if (ri.err.code) { efail(e, ri.err.code, ri.err.msg); return NULL; }
        if (n == 1 && (c0_record_keyed(first) || level)) return export_record(e, first);
    }
    out = c0_json_array();
    if (!out) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &rec)) {
        c0_json_value *r = export_record(e, rec);
        if (!r || !c0_json_array_push(out, r)) {
            c0_json_free(r); c0_json_free(out);
            efail(e, C0_ERR_OOM, "out of memory");
            return NULL;
        }
    }
    return out;
}

/* The groups of a file: none → []; one unnamed depth-0 group → that group;
 * otherwise an object of name → group. */
static c0_json_value *export_groups(exp *e, c0_file f, int level) {
    c0_group_iter gi = c0_file_groups(f);
    c0_group g, first;
    size_t n = 0;
    c0_json_value *obj;
    while (c0_next_group(&gi, &g)) {
        if (n == 0) first = g;
        n++;
    }
    if (gi.err.code) { efail(e, gi.err.code, gi.err.msg); return NULL; }
    if (n == 0) {
        c0_json_value *arr = c0_json_array();
        if (!arr) efail(e, C0_ERR_OOM, "out of memory");
        return arr;
    }
    if (n == 1 && c0_group_name(first).len == 0 && first.depth == 0) return export_group(e, first, level);
    obj = c0_json_object();
    if (!obj) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
    gi = c0_file_groups(f);
    while (c0_next_group(&gi, &g)) {
        c0_bytes name = c0_group_name(g);
        c0_json_value *v = export_group(e, g, 0);
        if (!v || !object_set(obj, name.ptr, name.len, v)) {
            c0_json_free(v); c0_json_free(obj);
            efail(e, C0_ERR_OOM, "out of memory");
            return NULL;
        }
    }
    return obj;
}

/* A body: a bare one exports as its group(s); a named file as {name: groups};
 * several files as an object of them. */
static c0_json_value *export_body(exp *e, c0_body body, int level) {
    c0_file_iter fi = c0_body_files(body);
    c0_file f, first;
    size_t n = 0;
    c0_json_value *obj;
    while (c0_next_file(&fi, &f)) {
        if (n == 0) first = f;
        n++;
    }
    if (fi.err.code) { efail(e, fi.err.code, fi.err.msg); return NULL; }
    if (n == 1) {
        c0_bytes name = c0_file_name(first);
        c0_json_value *groups = export_groups(e, first, level);
        if (!groups) return NULL;
        if (name.len == 0) return groups;
        obj = c0_json_object();
        if (!obj || !c0_json_object_push(obj, (const char *)name.ptr, name.len, groups)) {
            c0_json_free(obj); c0_json_free(groups);
            efail(e, C0_ERR_OOM, "out of memory");
            return NULL;
        }
        return obj;
    }
    obj = c0_json_object();
    if (!obj) { efail(e, C0_ERR_OOM, "out of memory"); return NULL; }
    fi = c0_body_files(body);
    while (c0_next_file(&fi, &f)) {
        c0_bytes name = c0_file_name(f);
        c0_json_value *groups = export_groups(e, f, level);
        if (!groups || !object_set(obj, name.ptr, name.len, groups)) {
            c0_json_free(groups); c0_json_free(obj);
            efail(e, C0_ERR_OOM, "out of memory");
            return NULL;
        }
    }
    return obj;
}

c0_json_value *c0_to_value(const uint8_t *buf, size_t len, c0_error *err) {
    c0_document doc;
    exp e;
    c0_error local = {C0_OK, 0, ""};
    e.err = err ? err : &local;
    e.err->code = C0_OK;
    e.err->pos = 0;
    e.err->msg = "";
    if (c0_document_read(buf, len, 0, &doc, e.err) != C0_OK) return NULL;
    return export_body(&e, doc.body, 0);
}

/* ===== Import: value -> C0DATA ===== */

static int all_groupable(const c0_json_value *obj) {
    size_t i;
    if (obj->n_pairs == 0) return 0;
    for (i = 0; i < obj->n_pairs; i++) {
        if (obj->pairs[i].val->kind == C0_JSON_STR) return 0;
    }
    return 1;
}

static int all_kind(const c0_json_value *arr, c0_json_kind k) {
    size_t i;
    for (i = 0; i < arr->n_items; i++) {
        if (arr->items[i]->kind != k) return 0;
    }
    return 1;
}

/* Do all array items share the first object's key sequence? */
static int tabular(const c0_json_value *arr) {
    const c0_json_value *first;
    size_t i, j;
    if (arr->n_items == 0) return 0;
    first = arr->items[0];
    if (first->kind != C0_JSON_OBJECT) return 0;
    for (i = 0; i < arr->n_items; i++) {
        const c0_json_value *it = arr->items[i];
        if (it->kind != C0_JSON_OBJECT || it->n_pairs != first->n_pairs) return 0;
        for (j = 0; j < it->n_pairs; j++) {
            if (it->pairs[j].key_len != first->pairs[j].key_len ||
                memcmp(it->pairs[j].key, first->pairs[j].key, it->pairs[j].key_len) != 0)
                return 0;
        }
    }
    return 1;
}

static const c0_json_value *fetch(const c0_json_value *obj, const char *key, size_t klen) {
    size_t i;
    for (i = 0; i < obj->n_pairs; i++) {
        if (obj->pairs[i].key_len == klen && memcmp(obj->pairs[i].key, key, klen) == 0) return obj->pairs[i].val;
    }
    return NULL;
}

#define B(s, n) ((const uint8_t *)(s)), (n)

static void emit_level_body(c0_builder *b, const c0_json_value *v);
static void emit_map(c0_builder *b, const c0_json_value *obj);
static void emit_item_as_record(c0_builder *b, const c0_json_value *item);

/* A value in unit position: a scalar, or a level. */
static void emit_unit(c0_builder *b, const c0_json_value *v) {
    if (v->kind == C0_JSON_STR) {
        c0_build_field(b, B(v->s, v->s_len));
    } else {
        c0_build_level_open(b);
        emit_level_body(b, v);
        c0_build_level_close(b);
    }
}

/* An object → one keyed record, entries in object order. */
static void emit_map(c0_builder *b, const c0_json_value *obj) {
    size_t i;
    c0_build_record(b, NULL, 0);
    for (i = 0; i < obj->n_pairs; i++) {
        const c0_json_pair *p = &obj->pairs[i];
        if (p->val->kind == C0_JSON_STR) {
            c0_build_key(b, B(p->key, p->key_len), B(p->val->s, p->val->s_len));
        } else {
            c0_build_key_level_open(b, B(p->key, p->key_len));
            emit_level_body(b, p->val);
            c0_build_level_close(b);
        }
    }
}

static void emit_header_from(c0_builder *b, const c0_json_value *first) {
    c0_bytes *names = (c0_bytes *)malloc((first->n_pairs ? first->n_pairs : 1) * sizeof(c0_bytes));
    size_t i;
    if (!names) {
        if (b->status == C0_OK) b->status = C0_ERR_OOM;
        return;
    }
    for (i = 0; i < first->n_pairs; i++) {
        names[i].ptr = (const uint8_t *)first->pairs[i].key;
        names[i].len = first->pairs[i].key_len;
    }
    c0_build_header(b, names, first->n_pairs);
    free(names);
}

static void emit_table_rows(c0_builder *b, const c0_json_value *arr) {
    const c0_json_value *first = arr->items[0];
    size_t i, j;
    for (i = 0; i < arr->n_items; i++) {
        const c0_json_value *row = arr->items[i];
        c0_build_record(b, NULL, 0);
        for (j = 0; j < first->n_pairs; j++) {
            const c0_json_value *cell = fetch(row, first->pairs[j].key, first->pairs[j].key_len);
            if (cell) emit_unit(b, cell);
            else c0_build_field(b, B("", 0));
        }
    }
}

static void emit_array_as_group(c0_builder *b, const c0_json_value *arr, const char *name, size_t nlen) {
    size_t i;
    c0_build_group(b, B(name, nlen));
    if (tabular(arr)) {
        emit_header_from(b, arr->items[0]);
        emit_table_rows(b, arr);
    } else {
        for (i = 0; i < arr->n_items; i++) emit_item_as_record(b, arr->items[i]);
    }
}

static void emit_item_as_record(c0_builder *b, const c0_json_value *item) {
    size_t i;
    switch (item->kind) {
        case C0_JSON_STR:
            c0_build_block(b, B(item->s, item->s_len));
            break;
        case C0_JSON_ARRAY:
            c0_build_record(b, NULL, 0);
            for (i = 0; i < item->n_items; i++) emit_unit(b, item->items[i]);
            break;
        case C0_JSON_OBJECT:
            emit_map(b, item);
            break;
    }
}

/* Inside a level: an array of scalars is one record (a list); an array of
 * uniform objects a table with a header row; an array of arrays a table; a
 * mixed array one record per item; an object a map. */
static void emit_level_body(c0_builder *b, const c0_json_value *v) {
    size_t i, j;
    switch (v->kind) {
        case C0_JSON_OBJECT:
            emit_map(b, v);
            break;
        case C0_JSON_ARRAY:
            if (all_kind(v, C0_JSON_STR)) {
                c0_build_record(b, NULL, 0);
                for (i = 0; i < v->n_items; i++) c0_build_field(b, B(v->items[i]->s, v->items[i]->s_len));
            } else if (tabular(v)) {
                emit_header_from(b, v->items[0]);
                emit_table_rows(b, v);
            } else if (all_kind(v, C0_JSON_ARRAY)) {
                for (i = 0; i < v->n_items; i++) {
                    c0_build_record(b, NULL, 0);
                    for (j = 0; j < v->items[i]->n_items; j++) emit_unit(b, v->items[i]->items[j]);
                }
            } else {
                for (i = 0; i < v->n_items; i++) emit_item_as_record(b, v->items[i]);
            }
            break;
        case C0_JSON_STR:
            c0_build_block(b, B(v->s, v->s_len));
            break;
    }
}

static void emit_groups(c0_builder *b, const c0_json_value *obj) {
    size_t i;
    for (i = 0; i < obj->n_pairs; i++) {
        const c0_json_pair *p = &obj->pairs[i];
        switch (p->val->kind) {
            case C0_JSON_OBJECT:
                c0_build_group(b, B(p->key, p->key_len));
                emit_map(b, p->val);
                break;
            case C0_JSON_ARRAY:
                emit_array_as_group(b, p->val, p->key, p->key_len);
                break;
            case C0_JSON_STR:
                c0_build_group(b, B(p->key, p->key_len));
                c0_build_block(b, B(p->val->s, p->val->s_len));
                break;
        }
    }
}

/* Top level: an object of groups (one key wrapping an object of groups
 * becomes a file); an array becomes one group; an object of scalars one
 * group holding one map; a scalar one record. */
static void emit_root(c0_builder *b, const c0_json_value *v, const char *group_name) {
    size_t gnlen = strlen(group_name);
    switch (v->kind) {
        case C0_JSON_OBJECT:
            if (v->n_pairs == 1 && v->pairs[0].val->kind == C0_JSON_OBJECT && all_groupable(v->pairs[0].val)) {
                c0_build_file(b, B(v->pairs[0].key, v->pairs[0].key_len));
                emit_groups(b, v->pairs[0].val);
            } else if (all_groupable(v)) {
                emit_groups(b, v);
            } else {
                c0_build_group(b, B(group_name, gnlen));
                emit_map(b, v);
            }
            break;
        case C0_JSON_ARRAY:
            emit_array_as_group(b, v, group_name, gnlen);
            break;
        case C0_JSON_STR:
            c0_build_group(b, B(group_name, gnlen));
            c0_build_block(b, B(v->s, v->s_len));
            break;
    }
}

uint8_t *c0_from_value(const c0_json_value *v, const char *group_name, size_t *out_len) {
    c0_builder b;
    c0_bytes got;
    uint8_t *out;
    c0_builder_init(&b);
    emit_root(&b, v, group_name ? group_name : "data");
    if (c0_builder_status(&b) != C0_OK) {
        c0_builder_free(&b);
        return NULL;
    }
    got = c0_builder_bytes(&b);
    out = (uint8_t *)malloc(got.len ? got.len : 1);
    if (out && got.len) memcpy(out, got.ptr, got.len);
    if (out && out_len) *out_len = got.len;
    c0_builder_free(&b);
    return out;
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

static c0_json_value *jp_value(jparse *p);

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

/* A JSON string from its opening quote; decoded bytes go to sb. 0 on error. */
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

static c0_json_value *jp_string(jparse *p) {
    c0_strbuf sb = {0, 0, 0, 0};
    c0_json_value *v;
    if (!jp_string_raw(p, &sb)) { free(sb.d); return NULL; }
    v = c0_json_str(sb.d ? (const char *)sb.d : "", sb.n);
    free(sb.d);
    return v;
}

static c0_json_value *jp_object(jparse *p) {
    c0_json_value *obj = c0_json_object();
    if (!obj) return NULL;
    p->pos++; /* { */
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
    for (;;) {
        c0_strbuf key = {0, 0, 0, 0};
        c0_json_value *val;
        jp_ws(p);
        if (!jp_string_raw(p, &key)) { free(key.d); c0_json_free(obj); return NULL; }
        jp_ws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { free(key.d); c0_json_free(obj); return NULL; }
        p->pos++;
        val = jp_value(p);
        if (!val) { free(key.d); c0_json_free(obj); return NULL; }
        if (!c0_json_object_push(obj, key.d ? (const char *)key.d : "", key.n, val)) {
            free(key.d); c0_json_free(val); c0_json_free(obj); return NULL;
        }
        free(key.d);
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return obj; }
        c0_json_free(obj);
        return NULL;
    }
}

static c0_json_value *jp_array(jparse *p) {
    c0_json_value *arr = c0_json_array();
    if (!arr) return NULL;
    p->pos++; /* [ */
    jp_ws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
    for (;;) {
        c0_json_value *item = jp_value(p);
        if (!item) { c0_json_free(arr); return NULL; }
        if (!c0_json_array_push(arr, item)) { c0_json_free(item); c0_json_free(arr); return NULL; }
        jp_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return arr; }
        c0_json_free(arr);
        return NULL;
    }
}

static int lit(jparse *p, const char *word) {
    size_t n = strlen(word);
    if (p->pos + n <= p->len && memcmp(p->s + p->pos, word, n) == 0) { p->pos += n; return 1; }
    return 0;
}

static c0_json_value *jp_number(jparse *p) {
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') p->pos++;
        else break;
    }
    if (p->pos == start) return NULL;
    return c0_json_str(p->s + start, p->pos - start);
}

static c0_json_value *jp_value(jparse *p) {
    char c;
    jp_ws(p);
    if (p->pos >= p->len) return NULL;
    c = p->s[p->pos];
    switch (c) {
        case '{': return jp_object(p);
        case '[': return jp_array(p);
        case '"': return jp_string(p);
        case 't': return lit(p, "true") ? c0_json_str("true", 4) : NULL;
        case 'f': return lit(p, "false") ? c0_json_str("false", 5) : NULL;
        case 'n': return lit(p, "null") ? c0_json_str("", 0) : NULL;
        default:  return jp_number(p);
    }
}

c0_json_value *c0_json_parse(const char *json, size_t len) {
    jparse p;
    c0_json_value *v;
    p.s = json; p.len = len; p.pos = 0;
    v = jp_value(&p);
    if (!v) return NULL;
    jp_ws(&p);
    if (p.pos != p.len) { c0_json_free(v); return NULL; } /* trailing junk */
    return v;
}

/* ===== JSON text: serializer (2-space pretty) ===== */

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

static void json_emit(c0_strbuf *sb, const c0_json_value *v, int depth) {
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

char *c0_json_print(const c0_json_value *v, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    json_emit(&sb, v, 0);
    c0_sb_byte(&sb, 0);
    if (sb.oom) { free(sb.d); return NULL; }
    if (out_len) *out_len = sb.n - 1;
    return (char *)sb.d;
}

char *c0_to_json(const uint8_t *buf, size_t len, size_t *out_len, c0_error *err) {
    c0_json_value *v = c0_to_value(buf, len, err);
    char *out;
    if (!v) return NULL;
    out = c0_json_print(v, out_len);
    c0_json_free(v);
    return out;
}

uint8_t *c0_from_json(const char *json, size_t len, const char *group_name, size_t *out_len) {
    c0_json_value *v = c0_json_parse(json, len);
    uint8_t *out;
    if (!v) return NULL;
    out = c0_from_value(v, group_name, out_len);
    c0_json_free(v);
    return out;
}
