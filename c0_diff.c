#include "c0_diff.h"

/* ===== Parse ===== */

/* Collect a data span [start,stop), removing DLE escapes, into a fresh
 * malloc'd buffer. Returns NULL on allocation failure. */
static uint8_t *collect_data(const uint8_t *buf, size_t start, size_t stop, size_t *out_len) {
    uint8_t *out = (uint8_t *)malloc((stop - start) ? (stop - start) : 1);
    size_t n = 0, pos = start;
    if (!out) return NULL;
    while (pos < stop) {
        if (buf[pos] == C0_DLE) {
            pos++;
            if (pos < stop) out[n++] = buf[pos++];
        } else {
            out[n++] = buf[pos++];
        }
    }
    *out_len = n;
    return out;
}

static int units_push(c0_diff_section *s, c0_diff_unit u) {
    c0_diff_unit *nv = (c0_diff_unit *)realloc(s->units, (s->nunits + 1) * sizeof(c0_diff_unit));
    if (!nv) return 0;
    s->units = nv;
    s->units[s->nunits++] = u;
    return 1;
}

static int sections_push(c0_diff_file *f, c0_diff_section sec) {
    c0_diff_section *nv = (c0_diff_section *)realloc(f->sections, (f->nsections + 1) * sizeof(c0_diff_section));
    if (!nv) return 0;
    f->sections = nv;
    f->sections[f->nsections++] = sec;
    return 1;
}

static int files_push(c0_diff *d, c0_diff_file f) {
    c0_diff_file *nv = (c0_diff_file *)realloc(d->files, (d->nfiles + 1) * sizeof(c0_diff_file));
    if (!nv) return 0;
    d->files = nv;
    d->files[d->nfiles++] = f;
    return 1;
}

/* Turn a collected span into a unit (a pure anchor) or complete a pending
 * substitution (the previous anchor becomes its `old`). Takes ownership of
 * `span`. Returns 0 on allocation failure. */
static int flush_span(c0_diff_section *s, int *in_sub, uint8_t *span, size_t span_len) {
    if (*in_sub && s->nunits > 0) {
        c0_diff_unit *u = &s->units[s->nunits - 1];
        u->is_sub = 1;
        u->replace = span;     /* the new text */
        u->replace_len = span_len;
        *in_sub = 0;
    } else {
        c0_diff_unit u;
        u.search = span;
        u.search_len = span_len;
        /* anchor: replacement equals search */
        u.replace = (uint8_t *)malloc(span_len ? span_len : 1);
        if (!u.replace) { free(span); return 0; }
        memcpy(u.replace, span, span_len);
        u.replace_len = span_len;
        u.is_sub = 0;
        if (!units_push(s, u)) { free(span); free(u.replace); return 0; }
    }
    return 1;
}

/* Parse a section body starting at pos (after GS). Returns next pos; *fail set
 * on allocation failure. */
static size_t parse_section(const uint8_t *buf, size_t len, size_t pos,
                            c0_diff_section *s, int *fail) {
    int in_sub = 0;
    size_t data_start = pos;
    s->units = NULL;
    s->nunits = 0;

    while (pos < len) {
        uint8_t byte = buf[pos];
        if (byte == C0_GS || byte == C0_FS || byte == C0_EOT) break;
        if (byte == C0_US) {
            if (pos > data_start) {
                size_t sl;
                uint8_t *span = collect_data(buf, data_start, pos, &sl);
                if (!span || !flush_span(s, &in_sub, span, sl)) { *fail = 1; return pos; }
            }
            pos++;
            data_start = pos;
        } else if (byte == C0_SUB) {
            if (pos > data_start) {
                /* the span so far is the substitution's `old` (a temp anchor) */
                size_t sl;
                c0_diff_unit u;
                uint8_t *span = collect_data(buf, data_start, pos, &sl);
                if (!span) { *fail = 1; return pos; }
                u.search = span;
                u.search_len = sl;
                u.replace = NULL;
                u.replace_len = 0;
                u.is_sub = 0;
                if (!units_push(s, u)) { free(span); *fail = 1; return pos; }
                in_sub = 1;
            }
            pos++;
            data_start = pos;
        } else if (byte == C0_DLE) {
            pos += 2;
        } else {
            pos++;
        }
    }
    if (pos > data_start) {
        size_t sl;
        size_t stop = pos < len ? pos : len;
        uint8_t *span = collect_data(buf, data_start, stop, &sl);
        if (!span || !flush_span(s, &in_sub, span, sl)) { *fail = 1; return pos; }
    }
    return pos;
}

static size_t parse_file(const uint8_t *buf, size_t len, size_t pos,
                         c0_diff_file *f, int *fail) {
    size_t path_start = pos;
    f->sections = NULL;
    f->nsections = 0;
    while (pos < len && buf[pos] >= 0x20) pos++;
    f->path_len = pos - path_start;
    f->path = (uint8_t *)malloc(f->path_len ? f->path_len : 1);
    if (!f->path) { *fail = 1; return pos; }
    memcpy(f->path, buf + path_start, f->path_len);

    while (pos < len) {
        uint8_t byte = buf[pos];
        if (byte == C0_FS || byte == C0_EOT) break;
        if (byte == C0_GS) {
            c0_diff_section sec;
            pos = parse_section(buf, len, pos + 1, &sec, fail);
            if (*fail) return pos;
            if (!sections_push(f, sec)) {
                /* free the orphaned section */
                size_t k;
                for (k = 0; k < sec.nunits; k++) { free(sec.units[k].search); free(sec.units[k].replace); }
                free(sec.units);
                *fail = 1;
                return pos;
            }
        } else {
            pos++;
        }
    }
    return pos;
}

c0_diff c0_diff_parse(const uint8_t *buf, size_t len) {
    c0_diff d;
    size_t pos = 0;
    int fail = 0;
    d.files = NULL;
    d.nfiles = 0;
    d.oom = 0;

    while (pos < len) {
        uint8_t byte = buf[pos];
        if (byte == C0_EOT) break;
        if (byte == C0_FS) {
            c0_diff_file f;
            f.path = NULL;
            pos = parse_file(buf, len, pos + 1, &f, &fail);
            if (fail) {
                /* free the orphaned file */
                size_t si, ui;
                for (si = 0; si < f.nsections; si++) {
                    for (ui = 0; ui < f.sections[si].nunits; ui++) {
                        free(f.sections[si].units[ui].search);
                        free(f.sections[si].units[ui].replace);
                    }
                    free(f.sections[si].units);
                }
                free(f.sections);
                free(f.path);
                c0_diff_free(&d);
                d.oom = 1;
                return d;
            }
            if (!files_push(&d, f)) {
                size_t si, ui;
                for (si = 0; si < f.nsections; si++) {
                    for (ui = 0; ui < f.sections[si].nunits; ui++) {
                        free(f.sections[si].units[ui].search);
                        free(f.sections[si].units[ui].replace);
                    }
                    free(f.sections[si].units);
                }
                free(f.sections);
                free(f.path);
                c0_diff_free(&d);
                d.oom = 1;
                return d;
            }
        } else {
            pos++;
        }
    }
    return d;
}

void c0_diff_free(c0_diff *d) {
    size_t fi, si, ui;
    if (!d) return;
    for (fi = 0; fi < d->nfiles; fi++) {
        c0_diff_file *f = &d->files[fi];
        for (si = 0; si < f->nsections; si++) {
            c0_diff_section *s = &f->sections[si];
            for (ui = 0; ui < s->nunits; ui++) {
                free(s->units[ui].search);
                free(s->units[ui].replace);
            }
            free(s->units);
        }
        free(f->sections);
        free(f->path);
    }
    free(d->files);
    d->files = NULL;
    d->nfiles = 0;
}

static uint8_t *section_concat(const c0_diff_section *s, int want_replace, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    size_t i;
    for (i = 0; i < s->nunits; i++) {
        const c0_diff_unit *u = &s->units[i];
        if (want_replace) c0_sb_raw(&sb, u->replace, u->replace_len);
        else              c0_sb_raw(&sb, u->search, u->search_len);
    }
    if (sb.oom) { free(sb.d); return NULL; }
    if (!sb.d) sb.d = (uint8_t *)malloc(1); /* non-NULL for an empty section */
    if (!sb.d) return NULL;
    if (out_len) *out_len = sb.n;
    return sb.d;
}

uint8_t *c0_diff_section_search(const c0_diff_section *s, size_t *out_len) {
    return section_concat(s, 0, out_len);
}
uint8_t *c0_diff_section_replace(const c0_diff_section *s, size_t *out_len) {
    return section_concat(s, 1, out_len);
}

/* ===== Apply ===== */

static size_t count_occ(const char *hay, size_t hlen, const uint8_t *needle, size_t nlen) {
    size_t count = 0, i;
    if (nlen == 0 || nlen > hlen) return 0;
    for (i = 0; i + nlen <= hlen;) {
        if (memcmp(hay + i, needle, nlen) == 0) { count++; i += nlen; }
        else i++;
    }
    return count;
}

/* Replace the first occurrence of needle in hay; returns a fresh buffer. */
static char *replace_first(const char *hay, size_t hlen,
                           const uint8_t *needle, size_t nlen,
                           const uint8_t *repl, size_t rlen, size_t *out_len) {
    c0_strbuf sb = {0, 0, 0, 0};
    size_t i;
    int done = 0;
    for (i = 0; i < hlen;) {
        if (!done && nlen > 0 && i + nlen <= hlen && memcmp(hay + i, needle, nlen) == 0) {
            c0_sb_raw(&sb, repl, rlen);
            i += nlen;
            done = 1;
        } else {
            c0_sb_byte(&sb, (uint8_t)hay[i]);
            i++;
        }
    }
    if (sb.oom) { free(sb.d); return NULL; }
    if (!sb.d) sb.d = (uint8_t *)malloc(1);
    if (!sb.d) return NULL;
    if (out_len) *out_len = sb.n;
    return (char *)sb.d;
}

static void set_err_path(c0_diff_error *err, const uint8_t *path, size_t len) {
    size_t n = len < sizeof(err->path) - 1 ? len : sizeof(err->path) - 1;
    memcpy(err->path, path, n);
    err->path[n] = 0;
}

/* Find an input file by path bytes; -1 if absent. */
static long find_input(const c0_diff_file_input *files, size_t nfiles,
                       const uint8_t *path, size_t plen) {
    size_t i;
    for (i = 0; i < nfiles; i++) {
        if (strlen(files[i].path) == plen && memcmp(files[i].path, path, plen) == 0)
            return (long)i;
    }
    return -1;
}

int c0_diff_apply(const uint8_t *diff_buf, size_t diff_len,
                  const c0_diff_file_input *files, size_t nfiles,
                  c0_diff_file_output **out, size_t *out_count,
                  c0_diff_error *err) {
    c0_diff d = c0_diff_parse(diff_buf, diff_len);
    c0_diff_file_output *res = NULL;
    size_t fi, si, i;

    if (err) { err->status = C0_DIFF_OK; err->path[0] = 0; err->section = 0; err->count = 0; }

    if (d.oom) { if (err) err->status = C0_DIFF_OOM; return 0; }

    /* Validate every edit against the original file contents. */
    for (fi = 0; fi < d.nfiles; fi++) {
        c0_diff_file *f = &d.files[fi];
        long idx = find_input(files, nfiles, f->path, f->path_len);
        if (idx < 0) {
            if (err) { err->status = C0_DIFF_FILE_NOT_FOUND; set_err_path(err, f->path, f->path_len); }
            c0_diff_free(&d);
            return 0;
        }
        for (si = 0; si < f->nsections; si++) {
            size_t plen;
            uint8_t *pat = c0_diff_section_search(&f->sections[si], &plen);
            size_t c;
            if (!pat) { if (err) err->status = C0_DIFF_OOM; c0_diff_free(&d); return 0; }
            c = count_occ(files[idx].content, files[idx].content_len, pat, plen);
            free(pat);
            if (c == 0) {
                if (err) { err->status = C0_DIFF_PATTERN_NOT_FOUND; set_err_path(err, f->path, f->path_len); err->section = si; }
                c0_diff_free(&d);
                return 0;
            } else if (c > 1) {
                if (err) { err->status = C0_DIFF_PATTERN_AMBIGUOUS; set_err_path(err, f->path, f->path_len); err->section = si; err->count = c; }
                c0_diff_free(&d);
                return 0;
            }
        }
    }

    /* Allocate outputs, one per input file, initially a copy of the input. */
    res = (c0_diff_file_output *)calloc(nfiles ? nfiles : 1, sizeof(c0_diff_file_output));
    if (!res) { if (err) err->status = C0_DIFF_OOM; c0_diff_free(&d); return 0; }
    for (i = 0; i < nfiles; i++) {
        size_t plen = strlen(files[i].path);
        res[i].path = (char *)malloc(plen + 1);
        res[i].content = (char *)malloc(files[i].content_len ? files[i].content_len : 1);
        if (!res[i].path || !res[i].content) {
            c0_diff_free_outputs(res, nfiles);
            if (err) err->status = C0_DIFF_OOM;
            c0_diff_free(&d);
            return 0;
        }
        memcpy(res[i].path, files[i].path, plen + 1);
        memcpy(res[i].content, files[i].content, files[i].content_len);
        res[i].content_len = files[i].content_len;
    }

    /* Apply: each edit starts from the original input, sections sequentially. */
    for (fi = 0; fi < d.nfiles; fi++) {
        c0_diff_file *f = &d.files[fi];
        long idx = find_input(files, nfiles, f->path, f->path_len);
        char *content = (char *)malloc(files[idx].content_len ? files[idx].content_len : 1);
        size_t clen = files[idx].content_len;
        if (!content) { c0_diff_free_outputs(res, nfiles); if (err) err->status = C0_DIFF_OOM; c0_diff_free(&d); return 0; }
        memcpy(content, files[idx].content, clen);
        for (si = 0; si < f->nsections; si++) {
            size_t plen, rlen, nlen;
            uint8_t *pat = c0_diff_section_search(&f->sections[si], &plen);
            uint8_t *rep = c0_diff_section_replace(&f->sections[si], &rlen);
            char *next;
            if (!pat || !rep) { free(pat); free(rep); free(content); c0_diff_free_outputs(res, nfiles); if (err) err->status = C0_DIFF_OOM; c0_diff_free(&d); return 0; }
            next = replace_first(content, clen, pat, plen, rep, rlen, &nlen);
            free(pat); free(rep); free(content);
            if (!next) { c0_diff_free_outputs(res, nfiles); if (err) err->status = C0_DIFF_OOM; c0_diff_free(&d); return 0; }
            content = next;
            clen = nlen;
        }
        free(res[idx].content);
        res[idx].content = content;
        res[idx].content_len = clen;
    }

    c0_diff_free(&d);
    *out = res;
    *out_count = nfiles;
    return 1;
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

/* ===== Builder ===== */

static void diff_escaped(c0_strbuf *sb, const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = (uint8_t)s[i];
        if (b < 0x20) c0_sb_byte(sb, C0_DLE);
        c0_sb_byte(sb, b);
    }
}

void c0_diff_builder_init(c0_diff_builder *b) {
    b->buf.d = NULL; b->buf.n = 0; b->buf.cap = 0; b->buf.oom = 0;
    b->unit_first = 1;
}
void c0_diff_builder_free(c0_diff_builder *b) {
    free(b->buf.d);
    b->buf.d = NULL; b->buf.n = 0; b->buf.cap = 0;
}

void c0_diff_file_begin(c0_diff_builder *b, const char *path, size_t path_len) {
    c0_sb_byte(&b->buf, C0_FS);
    c0_sb_raw(&b->buf, (const uint8_t *)path, path_len);
}
void c0_diff_section_begin(c0_diff_builder *b) {
    c0_sb_byte(&b->buf, C0_GS);
    b->unit_first = 1;
}
void c0_diff_anchor(c0_diff_builder *b, const char *text, size_t len) {
    if (!b->unit_first) c0_sb_byte(&b->buf, C0_US);
    diff_escaped(&b->buf, text, len);
    b->unit_first = 0;
}
void c0_diff_sub(c0_diff_builder *b, const char *old, size_t old_len,
                 const char *new_, size_t new_len) {
    if (!b->unit_first) c0_sb_byte(&b->buf, C0_US);
    diff_escaped(&b->buf, old, old_len);
    c0_sb_byte(&b->buf, C0_SUB);
    diff_escaped(&b->buf, new_, new_len);
    b->unit_first = 0;
}
void c0_diff_replace(c0_diff_builder *b,
                     const char *before, size_t before_len,
                     const char *old, size_t old_len,
                     const char *new_, size_t new_len,
                     const char *after, size_t after_len) {
    c0_sb_byte(&b->buf, C0_GS);
    if (before_len) { diff_escaped(&b->buf, before, before_len); c0_sb_byte(&b->buf, C0_US); }
    diff_escaped(&b->buf, old, old_len);
    c0_sb_byte(&b->buf, C0_SUB);
    diff_escaped(&b->buf, new_, new_len);
    if (after_len) { c0_sb_byte(&b->buf, C0_US); diff_escaped(&b->buf, after, after_len); }
    b->unit_first = 0;
}

c0_bytes c0_diff_builder_bytes(const c0_diff_builder *b) {
    c0_bytes r;
    r.ptr = b->buf.d;
    r.len = b->buf.n;
    return r;
}
