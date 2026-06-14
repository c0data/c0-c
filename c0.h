/*
 * c0.h — C0DATA: structured data using ASCII C0 control codes.
 *
 * Single-header library. In exactly one translation unit:
 *
 *     #define C0_IMPLEMENTATION
 *     #include "c0.h"
 *
 * Elsewhere just #include "c0.h" for the declarations.
 *
 * The read path is zero-copy and allocation-free: cursors walk a caller-owned
 * buffer and hand back (pointer, length) views into it. The hot loop is a
 * single comparison, `byte < 0x20`.
 *
 * License: MIT.
 */
#ifndef C0_H
#define C0_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Assigned C0 control codes. */
#define C0_SOH 0x01 /* Header (field name declarations)            */
#define C0_STX 0x02 /* Open nested sub-structure / reference scope */
#define C0_ETX 0x03 /* Close nested sub-structure / reference scope*/
#define C0_EOT 0x04 /* End of document / message                   */
#define C0_ENQ 0x05 /* Reference (enquiry)                         */
#define C0_DLE 0x10 /* Escape (next byte is literal)               */
#define C0_ETB 0x17 /* Commit marker (stream mode)                 */
#define C0_SUB 0x1a /* Substitution (C0-DIFF)                      */
#define C0_FS  0x1c /* File / Database separator                   */
#define C0_GS  0x1d /* Group / Table / Section separator           */
#define C0_RS  0x1e /* Record / Row separator                      */
#define C0_US  0x1f /* Unit / Field separator                      */

/* A zero-copy view into a buffer: not NUL-terminated. */
typedef struct {
    const uint8_t *ptr;
    size_t len;
} c0_bytes;

/* Token kinds. DLE is consumed during tokenization, never emitted. */
typedef enum {
    C0_TOK_DATA = 0,
    C0_TOK_SOH,
    C0_TOK_STX,
    C0_TOK_ETX,
    C0_TOK_EOT,
    C0_TOK_ENQ,
    C0_TOK_ETB,
    C0_TOK_SUB,
    C0_TOK_FS,
    C0_TOK_GS,
    C0_TOK_RS,
    C0_TOK_US
} c0_token_type;

typedef struct {
    c0_token_type type;
    size_t start; /* byte offset into the buffer */
    size_t end;   /* exclusive */
} c0_token;

/* Tokenizer errors. */
typedef enum {
    C0_OK = 0,
    C0_ERR_UNASSIGNED,     /* a control byte (< 0x20) that is not assigned */
    C0_ERR_UNEXPECTED_END  /* input ended right after a DLE escape         */
} c0_status;

/* Result of advancing a cursor. */
typedef enum {
    C0_END = 0,
    C0_TOKEN = 1,
    C0_ERROR = -1
} c0_step;

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    c0_status error;
} c0_tokenizer;

/* --- Bytewise helpers --- */

/* Whether a byte is an assigned C0 control code. */
int c0_is_assigned(uint8_t byte);

/*
 * Decode DLE escapes from `in`, writing the logical bytes to `out`. `out` must
 * have room for `len` bytes (decoding never grows). Returns the logical length.
 * A trailing DLE with nothing to escape (only on malformed input) is dropped.
 */
size_t c0_unescape(const uint8_t *in, size_t len, uint8_t *out);

/* Whether `in` contains any DLE escape (i.e. whether unescaping would copy). */
int c0_has_escape(const uint8_t *in, size_t len);

/*
 * Whether bytes are a canonical document unit for content addressing:
 * well-formed, minimally escaped (DLE only before bytes < 0x20), and free of
 * framing bytes (ETB, EOT). Stream logs validate per block, not with this.
 */
int c0_canonical(const uint8_t *in, size_t len);

/* --- Tokenizer --- */

void c0_tokenizer_init(c0_tokenizer *tz, const uint8_t *buf, size_t len);

/*
 * Advance to the next token. Returns C0_TOKEN and fills *out, C0_END at the end
 * of input, or C0_ERROR (with tz->error set) on malformed input.
 */
c0_step c0_tokenizer_next(c0_tokenizer *tz, c0_token *out);

/* --- Readers (zero-copy cursors) --- */

/* A group/table: a byte range beginning at its GS (or a bare buffer). */
typedef struct {
    const uint8_t *buf;
    size_t start;
    size_t end;
} c0_group;

/* A whole buffer as one group/table (no enclosing FS). */
c0_group c0_table(const uint8_t *buf, size_t len);

/* Group/table name (text after GS); empty for a bare buffer. */
c0_bytes c0_group_name(c0_group g);

/* Whether the group has an SOH header. */
int c0_group_has_header(c0_group g);

/* A forward cursor over a byte range. */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t end;
} c0_iter;

/* Header field cursor (identifier segments, US-separated). */
c0_iter c0_group_headers(c0_group g);
int c0_next_header(c0_iter *it, c0_bytes *out);

/* Record cursor: each *rec is a record's field-bytes (the RS is excluded). */
c0_iter c0_group_records(c0_group g);
int c0_next_record(c0_iter *it, c0_bytes *rec);

/* Field cursor within a record. Yields N+1 fields for N separators; respects
 * DLE escaping and STX/ETX nesting (fields are raw — use c0_unescape). */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t end;
    int done;
} c0_field_iter;
c0_field_iter c0_record_fields(c0_bytes rec);
int c0_next_field(c0_field_iter *it, c0_bytes *out);

/* Document: file name (text after FS), empty if none. */
c0_bytes c0_doc_name(const uint8_t *buf, size_t len);

/* Document cursor over top-level groups (single GS; GS*N sections are nested
 * within their parent group's range). */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} c0_doc_iter;
c0_doc_iter c0_doc(const uint8_t *buf, size_t len);
int c0_next_group(c0_doc_iter *it, c0_group *out);

/* --- Builder (writer) --- */

#define C0_BUILD_OK 0
#define C0_BUILD_OOM 1       /* allocation failed */
#define C0_BUILD_BAD_NAME 2  /* a control byte was passed where a name is required */

/* Owns a growable output buffer; call c0_builder_free when done. Once an error
 * is set, further writes are no-ops; check c0_builder_status before using the
 * bytes. Names (file/group/header) reject control bytes; record field values
 * are byte-transparent and DLE-escaped automatically. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int status;
} c0_builder;

void c0_builder_init(c0_builder *b);
void c0_builder_free(c0_builder *b);
int c0_builder_status(const c0_builder *b);
c0_bytes c0_builder_bytes(const c0_builder *b);

void c0_build_file(c0_builder *b, const uint8_t *name, size_t nlen);
void c0_build_group(c0_builder *b, const uint8_t *name, size_t nlen);
void c0_build_headers(c0_builder *b, const c0_bytes *names, size_t count);
void c0_build_record(c0_builder *b, const c0_bytes *fields, size_t count);
void c0_build_eot(c0_builder *b);
void c0_build_etb(c0_builder *b);

/* NUL-terminated convenience wrappers (cannot carry embedded NUL/binary). */
void c0_build_file_str(c0_builder *b, const char *name);
void c0_build_group_str(c0_builder *b, const char *name);
void c0_build_headers_str(c0_builder *b, const char *const *names, size_t count);
void c0_build_record_str(c0_builder *b, const char *const *fields, size_t count);

/* --- Stream mode (ETB commits) --- */

/* A view over an append-only log. `committed_end` is the offset past the last
 * commit marker; `torn` is set when uncommitted bytes trail it (residue of an
 * interrupted append — to be skipped on replay, and truncated before the next
 * append). A block is complete iff terminated by ETB. */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t committed_end;
    int torn;
} c0_stream;

c0_stream c0_stream_read(const uint8_t *buf, size_t len);
c0_bytes c0_stream_committed(const c0_stream *s);
c0_bytes c0_stream_tail(const c0_stream *s);

/* Committed-block cursor (block = bytes between commits, ETB + payload
 * excluded). The committed region also reads directly as a table via
 * c0_table(c0_stream_committed(s)...). */
typedef struct {
    const uint8_t *buf;
    size_t end;
    size_t pos;
    size_t block_start;
} c0_block_iter;
c0_block_iter c0_stream_blocks(const c0_stream *s);
int c0_next_block(c0_block_iter *it, c0_bytes *out);

#ifdef __cplusplus
}
#endif

/* ====================================================================== */
#ifdef C0_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

int c0_is_assigned(uint8_t byte) {
    switch (byte) {
        case C0_SOH: case C0_STX: case C0_ETX: case C0_EOT: case C0_ENQ:
        case C0_DLE: case C0_ETB: case C0_SUB: case C0_FS:  case C0_GS:
        case C0_RS:  case C0_US:
            return 1;
        default:
            return 0;
    }
}

int c0_has_escape(const uint8_t *in, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (in[i] == C0_DLE) return 1;
    }
    return 0;
}

size_t c0_unescape(const uint8_t *in, size_t len, uint8_t *out) {
    size_t i = 0, n = 0;
    while (i < len) {
        if (in[i] == C0_DLE) {
            i++;
            if (i >= len) break; /* dangling escape on malformed input */
            out[n++] = in[i++];
        } else {
            out[n++] = in[i++];
        }
    }
    return n;
}

int c0_canonical(const uint8_t *in, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t b = in[i];
        if (b == C0_DLE) {
            if (i + 1 >= len) return 0;     /* dangling escape    */
            if (in[i + 1] >= 0x20) return 0; /* gratuitous escape  */
            i += 2;
        } else if (b == C0_ETB || b == C0_EOT) {
            return 0;                        /* framing in a unit  */
        } else if (b < 0x20) {
            if (!c0_is_assigned(b)) return 0; /* unassigned code   */
            i++;
        } else {
            i++;
        }
    }
    return 1;
}

void c0_tokenizer_init(c0_tokenizer *tz, const uint8_t *buf, size_t len) {
    tz->buf = buf;
    tz->len = len;
    tz->pos = 0;
    tz->error = C0_OK;
}

static c0_token_type c0_control_token(uint8_t byte, int *ok) {
    *ok = 1;
    switch (byte) {
        case C0_SOH: return C0_TOK_SOH;
        case C0_STX: return C0_TOK_STX;
        case C0_ETX: return C0_TOK_ETX;
        case C0_EOT: return C0_TOK_EOT;
        case C0_ENQ: return C0_TOK_ENQ;
        case C0_ETB: return C0_TOK_ETB;
        case C0_SUB: return C0_TOK_SUB;
        case C0_FS:  return C0_TOK_FS;
        case C0_GS:  return C0_TOK_GS;
        case C0_RS:  return C0_TOK_RS;
        case C0_US:  return C0_TOK_US;
        default:     *ok = 0; return C0_TOK_DATA;
    }
}

c0_step c0_tokenizer_next(c0_tokenizer *tz, c0_token *out) {
    uint8_t byte;
    if (tz->error != C0_OK || tz->pos >= tz->len) {
        return tz->error != C0_OK ? C0_ERROR : C0_END;
    }
    byte = tz->buf[tz->pos];

    if (byte < 0x20) {
        if (byte == C0_DLE) {
            tz->pos++;
            if (tz->pos >= tz->len) {
                tz->error = C0_ERR_UNEXPECTED_END;
                return C0_ERROR;
            }
            out->type = C0_TOK_DATA;
            out->start = tz->pos;
            out->end = tz->pos + 1;
            tz->pos++;
            return C0_TOKEN;
        } else {
            int ok;
            c0_token_type t = c0_control_token(byte, &ok);
            if (!ok) {
                tz->error = C0_ERR_UNASSIGNED;
                return C0_ERROR;
            }
            out->type = t;
            out->start = tz->pos;
            out->end = tz->pos + 1;
            tz->pos++;
            return C0_TOKEN;
        }
    } else {
        size_t start = tz->pos;
        tz->pos++;
        while (tz->pos < tz->len && tz->buf[tz->pos] >= 0x20) {
            tz->pos++;
        }
        out->type = C0_TOK_DATA;
        out->start = start;
        out->end = tz->pos;
        return C0_TOKEN;
    }
}

/* --- Readers --- */

static size_t c0__skip_nested(const uint8_t *buf, size_t pos, size_t stop) {
    int depth = 1;
    pos++; /* skip STX */
    while (pos < stop && depth > 0) {
        uint8_t b = buf[pos];
        if (b == C0_STX) depth++;
        else if (b == C0_ETX) depth--;
        else if (b == C0_DLE) pos++;
        pos++;
    }
    return pos;
}

/* Position after GS + name and any ETB commit framing. */
static size_t c0__content_start(c0_group g) {
    size_t pos = g.start;
    if (pos < g.end && g.buf[pos] == C0_GS) {
        pos++;
        while (pos < g.end && g.buf[pos] >= 0x20) pos++;
    }
    while (pos < g.end && g.buf[pos] == C0_ETB) {
        pos++;
        while (pos < g.end && g.buf[pos] >= 0x20) pos++;
    }
    return pos;
}

/* Position just past an SOH header region (US-separated identifiers), or the
 * content start if there is no header. */
static size_t c0__records_start(c0_group g) {
    size_t pos = c0__content_start(g);
    if (pos < g.end && g.buf[pos] == C0_SOH) {
        pos++;
        while (pos < g.end && (g.buf[pos] >= 0x20 || g.buf[pos] == C0_US)) pos++;
    }
    return pos;
}

c0_group c0_table(const uint8_t *buf, size_t len) {
    c0_group g;
    g.buf = buf;
    g.start = 0;
    g.end = len;
    return g;
}

c0_bytes c0_group_name(c0_group g) {
    c0_bytes b;
    size_t pos = g.start;
    b.ptr = g.buf + g.start;
    b.len = 0;
    if (pos < g.end && g.buf[pos] == C0_GS) {
        size_t s;
        pos++;
        s = pos;
        while (pos < g.end && g.buf[pos] >= 0x20) pos++;
        b.ptr = g.buf + s;
        b.len = pos - s;
    }
    return b;
}

int c0_group_has_header(c0_group g) {
    size_t pos = c0__content_start(g);
    return pos < g.end && g.buf[pos] == C0_SOH;
}

c0_iter c0_group_headers(c0_group g) {
    c0_iter it;
    size_t pos = c0__content_start(g);
    it.buf = g.buf;
    if (pos < g.end && g.buf[pos] == C0_SOH) {
        size_t e;
        pos++;
        e = pos;
        while (e < g.end && (g.buf[e] >= 0x20 || g.buf[e] == C0_US)) e++;
        it.pos = pos;
        it.end = e;
    } else {
        it.pos = 1; /* pos > end => no headers */
        it.end = 0;
    }
    return it;
}

int c0_next_header(c0_iter *it, c0_bytes *out) {
    size_t s, p;
    if (it->pos > it->end) return 0;
    s = it->pos;
    p = s;
    while (p < it->end && it->buf[p] != C0_US) p++;
    out->ptr = it->buf + s;
    out->len = p - s;
    it->pos = (p < it->end) ? p + 1 : it->end + 1;
    return 1;
}

c0_iter c0_group_records(c0_group g) {
    c0_iter it;
    it.buf = g.buf;
    it.pos = c0__records_start(g);
    it.end = g.end;
    return it;
}

int c0_next_record(c0_iter *it, c0_bytes *rec) {
    while (it->pos < it->end) {
        uint8_t b = it->buf[it->pos];
        if (b == C0_GS || b == C0_FS || b == C0_EOT || b == C0_ETX) {
            it->pos = it->end;
            return 0;
        }
        if (b == C0_RS) {
            size_t s, e;
            it->pos++;
            s = it->pos;
            while (it->pos < it->end) {
                uint8_t c = it->buf[it->pos];
                if (c == C0_RS || c == C0_GS || c == C0_FS || c == C0_EOT ||
                    c == C0_ETX || c == C0_ETB) {
                    break;
                }
                if (c == C0_DLE) it->pos += 2;
                else if (c == C0_STX) it->pos = c0__skip_nested(it->buf, it->pos, it->end);
                else it->pos++;
            }
            e = it->pos < it->end ? it->pos : it->end;
            rec->ptr = it->buf + s;
            rec->len = e - s;
            return 1;
        }
        it->pos++; /* skip ETB framing and stray bytes */
    }
    return 0;
}

c0_field_iter c0_record_fields(c0_bytes rec) {
    c0_field_iter it;
    it.buf = rec.ptr;
    it.pos = 0;
    it.end = rec.len;
    it.done = 0;
    return it;
}

int c0_next_field(c0_field_iter *it, c0_bytes *out) {
    size_t s, e;
    if (it->done) return 0;
    s = it->pos;
    while (it->pos < it->end) {
        uint8_t b = it->buf[it->pos];
        if (b == C0_US) {
            out->ptr = it->buf + s;
            out->len = it->pos - s;
            it->pos++;
            return 1;
        }
        if (b == C0_DLE) it->pos += 2;
        else if (b == C0_STX) it->pos = c0__skip_nested(it->buf, it->pos, it->end);
        else it->pos++;
    }
    e = it->pos < it->end ? it->pos : it->end;
    out->ptr = it->buf + s;
    out->len = e - s;
    it->done = 1;
    return 1;
}

c0_bytes c0_doc_name(const uint8_t *buf, size_t len) {
    c0_bytes b;
    b.ptr = buf;
    b.len = 0;
    if (len > 0 && buf[0] == C0_FS) {
        size_t p = 1, s = 1;
        while (p < len && buf[p] >= 0x20) p++;
        b.ptr = buf + s;
        b.len = p - s;
    }
    return b;
}

c0_doc_iter c0_doc(const uint8_t *buf, size_t len) {
    c0_doc_iter it;
    it.buf = buf;
    it.len = len;
    it.pos = 0;
    if (len > 0 && buf[0] == C0_FS) {
        it.pos = 1;
        while (it.pos < len && buf[it.pos] >= 0x20) it.pos++;
    }
    return it;
}

/* End of the group whose GS+name ends at `pos`: scan to the next top-level GS,
 * FS, or EOT (GS*N sections stay inside). */
static size_t c0__group_end(const uint8_t *buf, size_t len, size_t pos) {
    while (pos < len) {
        uint8_t b = buf[pos];
        if (b == C0_FS || b == C0_EOT) break;
        if (b == C0_GS) {
            size_t peek = pos;
            int count = 0;
            while (peek < len && buf[peek] == C0_GS) {
                count++;
                peek++;
            }
            if (count == 1) break; /* next top-level group */
            pos = peek;
            while (pos < len && buf[pos] >= 0x20) pos++;
        } else if (b == C0_DLE) {
            pos += 2;
        } else {
            pos++;
        }
    }
    return pos < len ? pos : len;
}

int c0_next_group(c0_doc_iter *it, c0_group *out) {
    while (it->pos < it->len) {
        uint8_t b = it->buf[it->pos];
        if (b == C0_EOT) {
            it->pos = it->len;
            return 0;
        }
        if (b == C0_GS) {
            size_t gs_pos = it->pos;
            int run = 0;
            while (it->pos < it->len && it->buf[it->pos] == C0_GS) {
                run++;
                it->pos++;
            }
            while (it->pos < it->len && it->buf[it->pos] >= 0x20) it->pos++;
            if (run == 1) {
                size_t end = c0__group_end(it->buf, it->len, it->pos);
                out->buf = it->buf;
                out->start = gs_pos;
                out->end = end;
                it->pos = end;
                return 1;
            }
            /* GS*N section: already advanced past its name; keep scanning */
        } else {
            it->pos++;
        }
    }
    return 0;
}

/* --- Builder --- */

static int c0__reserve(c0_builder *b, size_t extra) {
    if (b->status != C0_BUILD_OK) return 0;
    if (b->len + extra > b->cap) {
        size_t ncap = b->cap ? b->cap : 64;
        uint8_t *nd;
        while (ncap < b->len + extra) ncap *= 2;
        nd = (uint8_t *)realloc(b->data, ncap);
        if (!nd) {
            b->status = C0_BUILD_OOM;
            return 0;
        }
        b->data = nd;
        b->cap = ncap;
    }
    return 1;
}

static void c0__byte(c0_builder *b, uint8_t x) {
    if (!c0__reserve(b, 1)) return;
    b->data[b->len++] = x;
}

static void c0__raw(c0_builder *b, const uint8_t *p, size_t n) {
    if (n == 0) return;
    if (!c0__reserve(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void c0__name(c0_builder *b, const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20) {
            b->status = C0_BUILD_BAD_NAME;
            return;
        }
    }
    c0__raw(b, p, n);
}

static void c0__escaped(c0_builder *b, const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20) c0__byte(b, C0_DLE);
        c0__byte(b, p[i]);
    }
}

void c0_builder_init(c0_builder *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->status = C0_BUILD_OK;
}

void c0_builder_free(c0_builder *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int c0_builder_status(const c0_builder *b) {
    return b->status;
}

c0_bytes c0_builder_bytes(const c0_builder *b) {
    c0_bytes r;
    r.ptr = b->data;
    r.len = b->len;
    return r;
}

void c0_build_file(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__byte(b, C0_FS);
    c0__name(b, name, nlen);
}

void c0_build_group(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__byte(b, C0_GS);
    c0__name(b, name, nlen);
}

void c0_build_headers(c0_builder *b, const c0_bytes *names, size_t count) {
    size_t i;
    c0__byte(b, C0_SOH);
    for (i = 0; i < count; i++) {
        if (i) c0__byte(b, C0_US);
        c0__name(b, names[i].ptr, names[i].len);
    }
}

void c0_build_record(c0_builder *b, const c0_bytes *fields, size_t count) {
    size_t i;
    c0__byte(b, C0_RS);
    for (i = 0; i < count; i++) {
        if (i) c0__byte(b, C0_US);
        c0__escaped(b, fields[i].ptr, fields[i].len);
    }
}

void c0_build_eot(c0_builder *b) {
    c0__byte(b, C0_EOT);
}

void c0_build_etb(c0_builder *b) {
    c0__byte(b, C0_ETB);
}

static c0_bytes c0__cstr(const char *s) {
    c0_bytes b;
    b.ptr = (const uint8_t *)s;
    b.len = strlen(s);
    return b;
}

void c0_build_file_str(c0_builder *b, const char *name) {
    c0_bytes n = c0__cstr(name);
    c0_build_file(b, n.ptr, n.len);
}

void c0_build_group_str(c0_builder *b, const char *name) {
    c0_bytes n = c0__cstr(name);
    c0_build_group(b, n.ptr, n.len);
}

void c0_build_headers_str(c0_builder *b, const char *const *names, size_t count) {
    size_t i;
    c0__byte(b, C0_SOH);
    for (i = 0; i < count; i++) {
        c0_bytes n = c0__cstr(names[i]);
        if (i) c0__byte(b, C0_US);
        c0__name(b, n.ptr, n.len);
    }
}

void c0_build_record_str(c0_builder *b, const char *const *fields, size_t count) {
    size_t i;
    c0__byte(b, C0_RS);
    for (i = 0; i < count; i++) {
        c0_bytes f = c0__cstr(fields[i]);
        if (i) c0__byte(b, C0_US);
        c0__escaped(b, f.ptr, f.len);
    }
}

/* --- Stream --- */

c0_stream c0_stream_read(const uint8_t *buf, size_t len) {
    c0_stream s;
    size_t pos = 0, last_end = 0;
    s.buf = buf;
    s.len = len;
    while (pos < len) {
        uint8_t b = buf[pos];
        if (b == C0_DLE) {
            pos += 2;
        } else if (b == C0_STX) {
            pos = c0__skip_nested(buf, pos, len);
        } else if (b == C0_ETB) {
            pos++;
            while (pos < len && buf[pos] >= 0x20) pos++;
            last_end = pos < len ? pos : len;
        } else {
            pos++;
        }
    }
    s.committed_end = last_end;
    s.torn = last_end < len;
    return s;
}

c0_bytes c0_stream_committed(const c0_stream *s) {
    c0_bytes b;
    b.ptr = s->buf;
    b.len = s->committed_end;
    return b;
}

c0_bytes c0_stream_tail(const c0_stream *s) {
    c0_bytes b;
    b.ptr = s->buf + s->committed_end;
    b.len = s->len - s->committed_end;
    return b;
}

c0_block_iter c0_stream_blocks(const c0_stream *s) {
    c0_block_iter it;
    it.buf = s->buf;
    it.end = s->committed_end;
    it.pos = 0;
    it.block_start = 0;
    return it;
}

int c0_next_block(c0_block_iter *it, c0_bytes *out) {
    while (it->pos < it->end) {
        uint8_t b = it->buf[it->pos];
        if (b == C0_DLE) {
            it->pos += 2;
        } else if (b == C0_STX) {
            it->pos = c0__skip_nested(it->buf, it->pos, it->end);
        } else if (b == C0_ETB) {
            size_t etb = it->pos;
            out->ptr = it->buf + it->block_start;
            out->len = etb - it->block_start;
            it->pos++;
            while (it->pos < it->end && it->buf[it->pos] >= 0x20) it->pos++;
            it->block_start = it->pos;
            return 1;
        } else {
            it->pos++;
        }
    }
    return 0;
}

#endif /* C0_IMPLEMENTATION */
#endif /* C0_H */
