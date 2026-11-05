#include "c0_diff.h"

/* ===== Parse ===== */

static void *xdup(const uint8_t *p, size_t n) {
    void *out = malloc(n ? n : 1);
    if (out && n) memcpy(out, p, n);
    return out;
}

static int push(void **arr, size_t *n, size_t elem, const void *item) {
    void *nv = realloc(*arr, (*n + 1) * elem);
    if (!nv) return 0;
    *arr = nv;
    memcpy((char *)nv + *n * elem, item, elem);
    (*n)++;
    return 1;
}

static void free_section(c0_diff_section *s) {
    size_t i;
    for (i = 0; i < s->nunits; i++) {
        free(s->units[i].old);
        free(s->units[i].new_);
    }
    free(s->units);
    for (i = 0; i < s->nreplaces; i++) {
        free(s->replaces[i].old);
        free(s->replaces[i].new_);
    }
    free(s->replaces);
    memset(s, 0, sizeof(*s));
}

static void free_file(c0_diff_file *f) {
    size_t i;
    free(f->path);
    for (i = 0; i < f->nleading; i++) {
        free(f->leading[i].old);
        free(f->leading[i].new_);
    }
    free(f->leading);
    for (i = 0; i < f->nsections; i++) free_section(&f->sections[i]);
    free(f->sections);
    memset(f, 0, sizeof(*f));
}

void c0_diff_free(c0_diff *d) {
    size_t i;
    if (!d) return;
    for (i = 0; i < d->nfiles; i++) free_file(&d->files[i]);
    free(d->files);
    d->files = NULL;
    d->nfiles = 0;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    c0_error err;
    int oom;
} dparse;

static void dfail(dparse *p, size_t pos, const char *msg) {
    if (p->err.code == C0_OK) {
        p->err.code = C0_ERR_SYNTAX;
        p->err.pos = pos;
        p->err.msg = msg;
    }
}

/* A value in a diff body: plain or text, decoded into a fresh buffer.
 * Returns the position after it (and its layout). */
static size_t read_dvalue(dparse *p, size_t pos, uint8_t **out, size_t *out_len) {
    const uint8_t *buf = p->buf;
    size_t len = p->len;
    *out = NULL;
    *out_len = 0;
    pos = c0_scan_layout(buf, pos, len);
    if (pos >= len) goto empty;
    if (buf[pos] == C0_STX) {
        size_t stop = c0_scan_text(buf, pos, len, &p->err);
        if (p->err.code) return len;
        *out = (uint8_t *)malloc(stop - pos);
        if (!*out) { p->oom = 1; return len; }
        *out_len = c0_decode_text(buf + pos + 1, stop - pos - 2, *out);
        return c0_scan_layout(buf, stop, len);
    }
    if (buf[pos] >= 0x20) {
        size_t stop = c0_scan_plain(buf, pos, len);
        c0_bytes v = c0_trim(c0_bytes_of(buf + pos, stop - pos));
        *out = (uint8_t *)xdup(v.ptr, v.len);
        if (!*out) { p->oom = 1; return len; }
        *out_len = v.len;
        return stop;
    }
empty:
    *out = (uint8_t *)malloc(1);
    if (!*out) p->oom = 1;
    return pos;
}

static size_t read_name(dparse *p, size_t pos, uint8_t **out, size_t *out_len) {
    const uint8_t *buf = p->buf;
    size_t stop;
    c0_bytes v;
    *out = NULL;
    *out_len = 0;
    pos = c0_scan_layout(buf, pos, p->len);
    if (pos < p->len && buf[pos] == C0_STX) {
        dfail(p, pos, "a path cannot be text");
        return p->len;
    }
    stop = c0_scan_plain(buf, pos, p->len);
    v = c0_trim(c0_bytes_of(buf + pos, stop - pos));
    *out = (uint8_t *)xdup(v.ptr, v.len);
    *out_len = v.len;
    if (!*out) p->oom = 1;
    return stop;
}

/* After GS: the pattern's units. */
static size_t parse_pattern(dparse *p, size_t pos, c0_diff_section *s) {
    const uint8_t *buf = p->buf;
    size_t len = p->len;
    memset(s, 0, sizeof(*s));
    for (;;) {
        c0_diff_unit u;
        memset(&u, 0, sizeof(u));
        pos = read_dvalue(p, pos, &u.old, &u.old_len);
        if (p->err.code || p->oom) { free(u.old); return len; }
        if (pos < len && buf[pos] == C0_SUB) {
            pos = read_dvalue(p, pos + 1, &u.new_, &u.new_len);
            u.is_sub = 1;
            if (p->err.code || p->oom) { free(u.old); free(u.new_); return len; }
        }
        if (!push((void **)&s->units, &s->nunits, sizeof(u), &u)) {
            free(u.old); free(u.new_);
            p->oom = 1;
            return len;
        }
        /* notes after a unit */
        while (pos < len && buf[pos] == C0_BEL) {
            size_t p2 = c0_scan_layout(buf, pos + 1, len);
            if (!(p2 < len && (buf[p2] >= 0x20 || buf[p2] == C0_STX))) break;
            pos = c0_scan_layout(buf, c0_scan_comment(buf, pos, len, &p->err), len);
            if (p->err.code) return len;
        }
        if (!(pos < len && buf[pos] == C0_US)) break;
        pos++;
    }
    return pos;
}

/* After RS: old SUB new. */
static size_t parse_replace(dparse *p, size_t pos, c0_diff_replacement *r) {
    const uint8_t *buf = p->buf;
    size_t len = p->len, start = pos;
    memset(r, 0, sizeof(*r));
    pos = read_dvalue(p, pos, &r->old, &r->old_len);
    if (p->err.code || p->oom) return len;
    if (r->old_len == 0) {
        dfail(p, start, "replace-all with an empty old part");
        return len;
    }
    if (!(pos < len && buf[pos] == C0_SUB)) {
        dfail(p, pos, "replace-all without SUB");
        return len;
    }
    pos = read_dvalue(p, pos + 1, &r->new_, &r->new_len);
    return (p->err.code || p->oom) ? len : pos;
}

c0_diff c0_diff_parse(const uint8_t *buf, size_t len) {
    c0_diff d;
    dparse p;
    c0_diff_file cur;
    int have_file = 0;
    size_t pos;
    memset(&d, 0, sizeof(d));
    memset(&p, 0, sizeof(p));
    memset(&cur, 0, sizeof(cur));
    p.buf = buf;
    p.len = len;
    p.err.msg = "";

    pos = c0_scan_layout(buf, 0, len);
    if (pos < len && buf[pos] == C0_SYN) {
        c0_document doc;
        c0_bytes shape;
        if (c0_document_read(buf, len, pos, &doc, &p.err) != C0_OK) goto fail;
        shape = c0_heading_shape(&doc);
        if (shape.len && !(shape.len == 4 && memcmp(shape.ptr, "diff", 4) == 0)) {
            dfail(&p, doc.heading_start, "heading declares a shape that is not diff");
            goto fail;
        }
        pos = doc.heading_end;
    }
    while (pos < len) {
        uint8_t b;
        int dead = 0;
        pos = c0_scan_layout(buf, pos, len);
        if (pos >= len) break;
        b = buf[pos];
        if (b == C0_BEL) {
            size_t p2 = c0_scan_layout(buf, pos + 1, len);
            if (p2 < len && (buf[p2] == C0_FS || buf[p2] == C0_GS || buf[p2] == C0_RS)) {
                dead = 1;
                pos = p2;
                b = buf[pos];
            } else {
                pos = c0_scan_comment(buf, pos, len, &p.err);
                if (p.err.code) goto fail;
                continue;
            }
        }
        if (b == C0_FS) {
            uint8_t *name;
            size_t nlen;
            pos = read_name(&p, pos + 1, &name, &nlen);
            if (p.err.code || p.oom) { free(name); goto fail; }
            if (dead) {
                free(name);
                continue;
            }
            if (have_file) {
                if (!push((void **)&d.files, &d.nfiles, sizeof(cur), &cur)) { free(name); p.oom = 1; goto fail; }
                memset(&cur, 0, sizeof(cur));
            }
            {
                size_t i;
                for (i = 0; i < d.nfiles; i++) {
                    if (d.files[i].path_len == nlen && memcmp(d.files[i].path, name, nlen) == 0) {
                        free(name);
                        dfail(&p, pos, "the same path twice");
                        goto fail;
                    }
                }
            }
            cur.path = name;
            cur.path_len = nlen;
            have_file = 1;
        } else if (b == C0_GS) {
            c0_diff_section s;
            if (!have_file) { dfail(&p, pos, "section before any file"); goto fail; }
            pos = parse_pattern(&p, pos + 1, &s);
            if (p.err.code || p.oom) { free_section(&s); goto fail; }
            if (dead) {
                free_section(&s);
            } else if (!push((void **)&cur.sections, &cur.nsections, sizeof(s), &s)) {
                free_section(&s);
                p.oom = 1;
                goto fail;
            }
        } else if (b == C0_RS) {
            c0_diff_replacement r;
            if (!have_file) { dfail(&p, pos, "replace-all before any file"); goto fail; }
            pos = parse_replace(&p, pos + 1, &r);
            if (p.err.code || p.oom) { free(r.old); free(r.new_); goto fail; }
            if (dead) {
                free(r.old);
                free(r.new_);
            } else {
                int ok;
                if (cur.nsections == 0) {
                    ok = push((void **)&cur.leading, &cur.nleading, sizeof(r), &r);
                } else {
                    c0_diff_section *last = &cur.sections[cur.nsections - 1];
                    ok = push((void **)&last->replaces, &last->nreplaces, sizeof(r), &r);
                }
                if (!ok) { free(r.old); free(r.new_); p.oom = 1; goto fail; }
            }
        } else if (b == C0_ETB) {
            pos = c0_scan_plain(buf, c0_scan_layout(buf, pos + 1, len), len);
        } else if (b == C0_EOT) {
            pos = c0_scan_layout(buf, pos + 1, len);
            if (pos < len && buf[pos] != C0_SYN && buf[pos] != C0_EM) {
                dfail(&p, pos, "unexpected byte after EOT");
                goto fail;
            }
            break;
        } else {
            dfail(&p, pos, "unexpected byte in a diff body");
            goto fail;
        }
    }
    if (have_file) {
        if (!push((void **)&d.files, &d.nfiles, sizeof(cur), &cur)) { p.oom = 1; goto fail; }
        memset(&cur, 0, sizeof(cur));
    }
    d.status = C0_OK;
    d.err = p.err;
    return d;
fail:
    free_file(&cur);
    c0_diff_free(&d);
    d.status = p.oom ? C0_ERR_OOM : (p.err.code ? p.err.code : C0_ERR_SYNTAX);
    d.err = p.err;
    if (p.oom) { d.err.code = C0_ERR_OOM; d.err.msg = "out of memory"; }
    return d;
}

static uint8_t *section_concat(const c0_diff_section *s, int replacement, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    size_t i;
    for (i = 0; i < s->nunits; i++) {
        const c0_diff_unit *u = &s->units[i];
        if (replacement && u->is_sub) c0_sb_raw(&sb, u->new_, u->new_len);
        else c0_sb_raw(&sb, u->old, u->old_len);
    }
    if (sb.oom) { free(sb.d); return NULL; }
    if (!sb.d) sb.d = (uint8_t *)malloc(1);
    if (!sb.d) return NULL;
    if (out_len) *out_len = sb.n;
    return sb.d;
}

uint8_t *c0_diff_section_search(const c0_diff_section *s, size_t *out_len) { return section_concat(s, 0, out_len); }
uint8_t *c0_diff_section_replacement(const c0_diff_section *s, size_t *out_len) { return section_concat(s, 1, out_len); }

/* ===== Apply ===== */

static long find(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen, size_t from) {
    size_t i;
    if (nlen == 0 || nlen > hlen) return -1;
    for (i = from; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0) return (long)i;
    }
    return -1;
}

static void set_err(c0_diff_error *err, c0_diff_status st, const uint8_t *path, size_t plen, size_t section) {
    if (!err) return;
    err->status = st;
    if (path) {
        size_t n = plen < sizeof(err->path) - 1 ? plen : sizeof(err->path) - 1;
        memcpy(err->path, path, n);
        err->path[n] = 0;
    }
    err->section = section;
}

/* Replace every old with new in segment; appends to out. */
static c0_diff_status replace_all(c0_strbuf *out, const uint8_t *seg, size_t slen,
                                  const c0_diff_replacement *reps, size_t nreps) {
    c0_strbuf cur = {0, 0, 0, 0};
    size_t i;
    c0_strbuf a = {0, 0, 0, 0};
    c0_sb_raw(&a, seg, slen);
    for (i = 0; i < nreps; i++) {
        size_t pos = 0, count = 0;
        long at;
        cur.n = 0;
        while ((at = find(a.d, a.n, reps[i].old, reps[i].old_len, pos)) >= 0) {
            c0_sb_raw(&cur, a.d + pos, (size_t)at - pos);
            c0_sb_raw(&cur, reps[i].new_, reps[i].new_len);
            pos = (size_t)at + reps[i].old_len;
            count++;
        }
        if (count == 0) {
            free(a.d);
            free(cur.d);
            return C0_DIFF_REPLACE_NOT_FOUND;
        }
        c0_sb_raw(&cur, a.d + pos, a.n - pos);
        {
            c0_strbuf t = a;
            a = cur;
            cur = t;
        }
    }
    if (a.oom || cur.oom) { free(a.d); free(cur.d); return C0_DIFF_OOM; }
    c0_sb_raw(out, a.d, a.n);
    free(a.d);
    free(cur.d);
    return out->oom ? C0_DIFF_OOM : C0_DIFF_OK;
}

typedef struct { size_t s, e; uint8_t *repl; size_t rlen; } match;

/* Apply one file's edits (DIFF.md "Sections", "Replace-all"). */
static c0_diff_status apply_file(const uint8_t *data, size_t dlen, const c0_diff_file *f,
                                 uint8_t **out, size_t *out_len, size_t *bad_section) {
    match *m = NULL;
    size_t cursor = 0, i, first_start;
    c0_strbuf o = {0, 0, 0, 0};
    c0_diff_status st = C0_DIFF_OK;
    m = (match *)calloc(f->nsections ? f->nsections : 1, sizeof(match));
    if (!m) return C0_DIFF_OOM;
    for (i = 0; i < f->nsections; i++) {
        size_t slen;
        uint8_t *search = c0_diff_section_search(&f->sections[i], &slen);
        long first;
        *bad_section = i;
        if (!search) { st = C0_DIFF_OOM; goto done; }
        if (slen == 0) { free(search); st = C0_DIFF_EMPTY_SEARCH; goto done; }
        first = find(data, dlen, search, slen, cursor);
        if (first < 0) { free(search); st = C0_DIFF_PATTERN_NOT_FOUND; goto done; }
        if (find(data, dlen, search, slen, (size_t)first + 1) >= 0) { free(search); st = C0_DIFF_PATTERN_AMBIGUOUS; goto done; }
        free(search);
        m[i].s = (size_t)first;
        m[i].e = (size_t)first + slen;
        m[i].repl = c0_diff_section_replacement(&f->sections[i], &m[i].rlen);
        if (!m[i].repl) { st = C0_DIFF_OOM; goto done; }
        cursor = m[i].e;
    }
    first_start = f->nsections ? m[0].s : dlen;
    *bad_section = 0;
    st = replace_all(&o, data, first_start, f->leading, f->nleading);
    if (st) goto done;
    for (i = 0; i < f->nsections; i++) {
        size_t nxt = i + 1 < f->nsections ? m[i + 1].s : dlen;
        *bad_section = i;
        c0_sb_raw(&o, m[i].repl, m[i].rlen);
        st = replace_all(&o, data + m[i].e, nxt - m[i].e, f->sections[i].replaces, f->sections[i].nreplaces);
        if (st) goto done;
    }
    if (o.oom) st = C0_DIFF_OOM;
done:
    for (i = 0; i < f->nsections; i++) free(m[i].repl);
    free(m);
    if (st) {
        free(o.d);
        return st;
    }
    if (!o.d) o.d = (uint8_t *)malloc(1);
    if (!o.d) return C0_DIFF_OOM;
    *out = o.d;
    *out_len = o.n;
    return C0_DIFF_OK;
}

static long find_input(const c0_diff_file_input *files, size_t nfiles, const uint8_t *path, size_t plen) {
    size_t i;
    for (i = 0; i < nfiles; i++) {
        if (files[i].path.len == plen && (plen == 0 || memcmp(files[i].path.ptr, path, plen) == 0)) return (long)i;
    }
    return -1;
}

void c0_diff_free_outputs(c0_diff_file_output *out, size_t count) {
    size_t i;
    if (!out) return;
    for (i = 0; i < count; i++) {
        free(out[i].path);
        free(out[i].content);
    }
    free(out);
}

int c0_diff_apply(const uint8_t *diff_buf, size_t diff_len,
                  const c0_diff_file_input *files, size_t nfiles,
                  c0_diff_file_output **out, size_t *out_count,
                  c0_diff_error *err) {
    c0_diff d = c0_diff_parse(diff_buf, diff_len);
    c0_diff_file_output *res = NULL;
    size_t i;
    int ok = 0;

    if (err) {
        memset(err, 0, sizeof(*err));
        err->msg = "";
    }
    if (d.status != C0_OK) {
        if (err) {
            err->status = d.status == C0_ERR_OOM ? C0_DIFF_OOM : C0_DIFF_SYNTAX;
            if (d.err.msg && strstr(d.err.msg, "shape")) err->status = C0_DIFF_WRONG_SHAPE;
            if (d.err.msg && strstr(d.err.msg, "same path")) err->status = C0_DIFF_DUPLICATE_PATH;
            err->pos = d.err.pos;
            err->msg = d.err.msg ? d.err.msg : "";
        }
        return 0;
    }

    res = (c0_diff_file_output *)calloc(nfiles ? nfiles : 1, sizeof(c0_diff_file_output));
    if (!res) { set_err(err, C0_DIFF_OOM, NULL, 0, 0); goto done; }

    /* Validate and compute every edit first; nothing is handed out on error. */
    for (i = 0; i < d.nfiles; i++) {
        const c0_diff_file *f = &d.files[i];
        long idx = find_input(files, nfiles, f->path, f->path_len);
        size_t section = 0;
        c0_diff_status st;
        if (idx < 0) { set_err(err, C0_DIFF_FILE_NOT_FOUND, f->path, f->path_len, 0); goto done; }
        st = apply_file(files[idx].content.ptr, files[idx].content.len, f, &res[idx].content, &res[idx].content_len, &section);
        if (st) { set_err(err, st, f->path, f->path_len, section); goto done; }
    }
    for (i = 0; i < nfiles; i++) {
        res[i].path = (uint8_t *)xdup(files[i].path.ptr, files[i].path.len);
        res[i].path_len = files[i].path.len;
        if (!res[i].path) { set_err(err, C0_DIFF_OOM, NULL, 0, 0); goto done; }
        if (!res[i].content) {
            res[i].content = (uint8_t *)xdup(files[i].content.ptr, files[i].content.len);
            res[i].content_len = files[i].content.len;
            if (!res[i].content) { set_err(err, C0_DIFF_OOM, NULL, 0, 0); goto done; }
        }
    }
    *out = res;
    *out_count = nfiles;
    res = NULL;
    ok = 1;
done:
    if (res) c0_diff_free_outputs(res, nfiles);
    c0_diff_free(&d);
    return ok;
}

/* ===== Builder ===== */

static void dvalue(c0_strbuf *sb, const uint8_t *p, size_t n) {
    size_t i;
    if (c0_is_plain(p, n)) {
        c0_sb_raw(sb, p, n);
        return;
    }
    c0_sb_byte(sb, C0_STX);
    for (i = 0; i < n; i++) {
        if (c0_is_text_escaped(p[i])) c0_sb_byte(sb, C0_DLE);
        c0_sb_byte(sb, p[i]);
    }
    c0_sb_byte(sb, C0_ETX);
}

void c0_diff_builder_init(c0_diff_builder *b, int heading) {
    memset(b, 0, sizeof(*b));
    b->unit_first = 1;
    if (heading) {
        c0_sb_byte(&b->buf, C0_SYN);
        c0_sb_byte(&b->buf, C0_SOH);
        c0_sb_cstr(&b->buf, "shape");
        c0_sb_byte(&b->buf, C0_US);
        c0_sb_cstr(&b->buf, "diff");
    }
}

void c0_diff_builder_free(c0_diff_builder *b) {
    free(b->buf.d);
    memset(b, 0, sizeof(*b));
}

void c0_diff_file_begin(c0_diff_builder *b, const uint8_t *path, size_t path_len) {
    c0_sb_byte(&b->buf, C0_FS);
    c0_sb_raw(&b->buf, path, path_len);
    b->unit_first = 1;
}

void c0_diff_section_begin(c0_diff_builder *b) {
    c0_sb_byte(&b->buf, C0_GS);
    b->unit_first = 1;
}

static void sep(c0_diff_builder *b) {
    if (!b->unit_first) c0_sb_byte(&b->buf, C0_US);
    b->unit_first = 0;
}

void c0_diff_anchor(c0_diff_builder *b, const uint8_t *text, size_t len) {
    sep(b);
    dvalue(&b->buf, text, len);
}

void c0_diff_sub(c0_diff_builder *b, const uint8_t *old, size_t old_len, const uint8_t *new_, size_t new_len) {
    sep(b);
    dvalue(&b->buf, old, old_len);
    c0_sb_byte(&b->buf, C0_SUB);
    dvalue(&b->buf, new_, new_len);
}

void c0_diff_replace(c0_diff_builder *b,
                     const uint8_t *before, size_t before_len,
                     const uint8_t *old, size_t old_len,
                     const uint8_t *new_, size_t new_len,
                     const uint8_t *after, size_t after_len) {
    c0_diff_section_begin(b);
    if (before_len) c0_diff_anchor(b, before, before_len);
    c0_diff_sub(b, old, old_len, new_, new_len);
    if (after_len) c0_diff_anchor(b, after, after_len);
}

void c0_diff_replace_all(c0_diff_builder *b, const uint8_t *old, size_t old_len, const uint8_t *new_, size_t new_len) {
    c0_sb_byte(&b->buf, C0_RS);
    dvalue(&b->buf, old, old_len);
    c0_sb_byte(&b->buf, C0_SUB);
    dvalue(&b->buf, new_, new_len);
    b->unit_first = 1;
}

void c0_diff_note(c0_diff_builder *b, const uint8_t *text, size_t len) {
    c0_sb_byte(&b->buf, C0_BEL);
    dvalue(&b->buf, text, len);
}

void c0_diff_eot(c0_diff_builder *b) { c0_sb_byte(&b->buf, C0_EOT); }

c0_bytes c0_diff_builder_bytes(const c0_diff_builder *b) {
    c0_bytes r;
    r.ptr = b->buf.d;
    r.len = b->buf.n;
    return r;
}

#define U(s) ((const uint8_t *)(s)), strlen(s)
void c0_diff_file_begin_str(c0_diff_builder *b, const char *path) { c0_diff_file_begin(b, U(path)); }
void c0_diff_anchor_str(c0_diff_builder *b, const char *text) { c0_diff_anchor(b, U(text)); }
void c0_diff_sub_str(c0_diff_builder *b, const char *old, const char *new_) { c0_diff_sub(b, U(old), U(new_)); }
void c0_diff_replace_str(c0_diff_builder *b, const char *before, const char *old, const char *new_, const char *after) {
    c0_diff_replace(b, U(before), U(old), U(new_), U(after));
}
void c0_diff_replace_all_str(c0_diff_builder *b, const char *old, const char *new_) { c0_diff_replace_all(b, U(old), U(new_)); }
void c0_diff_note_str(c0_diff_builder *b, const char *text) { c0_diff_note(b, U(text)); }
