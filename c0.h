/*
 * c0.h — C0DATA 1.0: structured data using ASCII C0 control codes.
 *
 * Single-header library. In exactly one translation unit:
 *
 *     #define C0_IMPLEMENTATION
 *     #include "c0.h"
 *
 * Elsewhere just #include "c0.h" for the declarations.
 *
 * The read path is zero-copy and allocation-free: views and cursors walk a
 * caller-owned buffer and hand back (pointer, length) slices into it. The
 * hot loop outside text is a single comparison, `byte < 0x20`.
 *
 * Reading is a two-step contract: `c0_validate` checks a buffer against the
 * grammar once; the navigators (`c0_body`, `c0_document`, the iterators) then
 * assume well-formed input and stop early, setting their `err`, when they
 * meet something malformed. Writing is `c0_builder`, which owns a growable
 * buffer and spells every value in its one canonical form.
 *
 * Spec: https://github.com/c0data/c0-spec (DESIGN.md, GRAMMAR.md, DIFF.md).
 * License: MIT.
 */
#ifndef C0_H
#define C0_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C0_VERSION "1.0.0-dev"

/* ---- Assigned control codes (DESIGN.md "Assigned Control Codes") ------ */
#define C0_SOH 0x01 /**< Heading: a key for the value after it; SOH RS a header row */
#define C0_STX 0x02 /**< Start of text (a literal value)                            */
#define C0_ETX 0x03 /**< End of text                                                */
#define C0_EOT 0x04 /**< End of document                                            */
#define C0_ENQ 0x05 /**< Reference                                                  */
#define C0_BEL 0x07 /**< Comment: a note, or marks the next element dead            */
#define C0_DLE 0x10 /**< Recurse (opens a nested level) outside text; escape inside */
#define C0_SYN 0x16 /**< Start of document, then the heading                        */
#define C0_ETB 0x17 /**< Commit marker (stream mode)                                */
#define C0_CAN 0x18 /**< Close a nested level                                       */
#define C0_EM  0x19 /**< End of medium: a raw blob follows                          */
#define C0_SUB 0x1a /**< Substitution (C0-DIFF only)                                */
#define C0_FS  0x1c /**< File separator                                             */
#define C0_GS  0x1d /**< Group separator                                            */
#define C0_RS  0x1e /**< Record separator                                           */
#define C0_US  0x1f /**< Unit separator                                             */

/* Layout codes outside text. */
#define C0_HT 0x09
#define C0_LF 0x0a
#define C0_CR 0x0d
#define C0_SP 0x20

/** A zero-copy view into a buffer: not NUL-terminated. */
typedef struct {
    const uint8_t *ptr;
    size_t len;
} c0_bytes;

/** Status codes shared by every part of the library. */
typedef enum {
    C0_OK = 0,
    C0_ERR_SYNTAX,         /**< malformed input (see c0_error.msg / .pos)        */
    C0_ERR_UNASSIGNED,     /**< an unassigned control code outside text          */
    C0_ERR_UNEXPECTED_END, /**< input ended inside text, a level, or after DLE/BEL */
    C0_ERR_TOO_DEEP,       /**< nesting deeper than the validator's limit        */
    C0_ERR_NOT_SCALAR,     /**< c0_value on a level or reference                 */
    C0_ERR_NOT_LEVEL,      /**< c0_unit_level on a unit that is not DLE … CAN    */
    C0_ERR_NOT_LIST,       /**< c0_unit_list on a level that is not a list       */
    C0_ERR_NOT_REFERENCE,  /**< c0_unit_ref on a unit that is not ENQ …          */
    C0_ERR_NEEDS_DECODE,   /**< c0_value without scratch on text with escapes    */
    C0_ERR_NOT_FOUND,      /**< no such group / key / field / blob               */
    C0_ERR_BAD_NAME,       /**< builder: a name with a control byte or edge space */
    C0_ERR_BAD_STATE,      /**< builder: e.g. a level closed that was never opened */
    C0_ERR_OOM             /**< allocation failed                                */
} c0_status;

/** Error detail: the code, the byte offset it was found at, and a static
 * message. Every reader accepts a NULL `c0_error *` when it does not want it. */
typedef struct {
    c0_status code;
    size_t pos;
    const char *msg;
} c0_error;

/** Warnings a document reader raises (DESIGN.md "Warnings"), as bits. */
#define C0_WARN_NO_SYN       0x01u /**< loud: no SYN at the start               */
#define C0_WARN_NO_EOT       0x02u /**< loud: no EOT and the shape is not stream */
#define C0_WARN_UNKNOWN_KEY  0x04u /**< loud: a heading key the reader does not know */
#define C0_WARN_DUPLICATE_KEY 0x08u /**< a duplicate heading key (last wins)      */

/* ---- Bytewise helpers -------------------------------------------------- */

/** Whether a byte is one of the sixteen assigned codes. */
int c0_is_assigned(uint8_t byte);
/** Whether a byte is layout outside text (SP, HT, LF, CR). */
int c0_is_layout(uint8_t byte);
/** Whether a byte is one of the seven codes that take a DLE inside text
 * (STX ETX DLE ETB EOT SYN EM). */
int c0_is_text_escaped(uint8_t byte);
/** Whether a value can be written plain: no byte below 0x20, no edge whitespace. */
int c0_is_plain(const uint8_t *p, size_t len);
/** Whether bytes are a legal name (label, key, header name): same test as plain. */
int c0_is_name(const uint8_t *p, size_t len);
/** A view over `len` bytes at `ptr`. */
c0_bytes c0_bytes_of(const uint8_t *ptr, size_t len);
/** Layout trimmed from both ends of a raw slice. */
c0_bytes c0_trim(c0_bytes raw);

/** Decode the inside of a text (the bytes between STX and ETX): DLE escapes
 * removed. `out` needs room for `len` bytes (decoding never grows). Returns
 * the logical length. */
size_t c0_decode_text(const uint8_t *inside, size_t len, uint8_t *out);

/* ---- Scanning primitives (c0.cr `Scan`) --------------------------------
 * Each takes a buffer and a position and returns the position after the
 * construct; on malformed input it records the error (when err is given)
 * and returns `stop`. Converters and bindings build on these. */

/** Skip layout bytes. */
size_t c0_scan_layout(const uint8_t *buf, size_t pos, size_t stop);
/** Skip a run of bytes >= 0x20 (a label, key, payload or plain value with its layout). */
size_t c0_scan_plain(const uint8_t *buf, size_t pos, size_t stop);
/** At an STX: the position after the matching ETX. */
size_t c0_scan_text(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
/** At a DLE: the position after the matching CAN. */
size_t c0_scan_level(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
/** From just after a US (or a key's US): the end of the unit's value. */
size_t c0_scan_unit(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
/** At a BEL: past the note, or past the whole dead element. */
size_t c0_scan_comment(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
/** From just after an RS: the end of the record. */
size_t c0_scan_record_end(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);

/* ---- Validation -------------------------------------------------------- */

/** Check a whole buffer (one or more documents, an optional EM tail) against
 * GRAMMAR.md. Returns C0_OK or the first error (detail in *err when given). */
c0_status c0_validate(const uint8_t *buf, size_t len, c0_error *err);

/** Whether bytes are a canonical unit (DESIGN.md "Canonical Form"):
 * well-formed, no layout, one spelling per value, unique keys in each keyed
 * record, no framing, no comments. */
int c0_canonical(const uint8_t *buf, size_t len);

/* ---- Units and values -------------------------------------------------- */

/** What a raw unit holds, by its first non-layout byte. */
typedef enum {
    C0_UNIT_PLAIN = 0, /**< a plain value, the empty value, or a unit holding only a note */
    C0_UNIT_TEXT,      /**< STX … ETX                                                   */
    C0_UNIT_LEVEL,     /**< DLE … CAN                                                   */
    C0_UNIT_REF        /**< ENQ name, or ENQ STX path ETX                               */
} c0_unit_kind;

c0_unit_kind c0_unit_type(c0_bytes raw);

/**
 * The logical value of a raw unit (as yielded by c0_next_unit, c0_next_pair,
 * c0_record_field …): for a plain value the slice with edge layout removed;
 * for text the inside with its escapes decoded; for a unit holding only a
 * note, the empty value. *out points into the buffer when no decoding is
 * needed; when the text holds DLE escapes it is decoded into `scratch`
 * (room for raw.len bytes always suffices) and *out points there. With a
 * NULL scratch such a unit answers C0_ERR_NEEDS_DECODE. A level or reference
 * answers C0_ERR_NOT_SCALAR; malformed input C0_ERR_SYNTAX.
 */
c0_status c0_value(c0_bytes raw, uint8_t *scratch, c0_bytes *out);

/** A body: a document body or the inside of a nested level, as a range of
 * the buffer. Build one with c0_body_view, c0_document_read or c0_unit_level. */
typedef struct {
    const uint8_t *buf;
    size_t start;
    size_t stop;
} c0_body;

/** A body over a whole buffer (a slice with no SYN: records, groups, files). */
c0_body c0_body_view(const uint8_t *buf, size_t len);

/** A raw unit as a nested level: the body between its DLE and CAN. */
c0_status c0_unit_level(c0_bytes raw, c0_body *out);

/** A parsed reference: one segment (a group) or two or three (group,
 * record id, field name). Segments are slices into the buffer. */
typedef struct {
    c0_bytes seg[3];
    size_t count;
} c0_ref;

/** A raw unit as a reference. Checks the form only (DESIGN.md "References"). */
c0_status c0_unit_ref(c0_bytes raw, c0_ref *out);

/* ---- Records ------------------------------------------------------------ */

/** A record: the bytes after its RS, trailing layout excluded. Positional
 * (units separated by US) or keyed (SOH key US value …), never mixed. */
typedef struct {
    const uint8_t *buf;
    size_t start;
    size_t stop;
} c0_record;

/** Whether the record is keyed (its first live unit is an SOH). */
int c0_record_keyed(c0_record r);

/** Cursor over the positional units of a record, or the items of a list. */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t stop;
    int done;
    c0_error err;
} c0_unit_iter;

/** Positional units, in order. Each `raw` is the unit's bytes with its
 * layout; pass it to c0_value / c0_unit_level / c0_unit_ref. N separators
 * yield N+1 units; an empty record yields one empty unit. A keyed record
 * yields nothing. */
c0_unit_iter c0_record_units(c0_record r);
int c0_next_unit(c0_unit_iter *it, c0_bytes *raw);

/** Number of positional units. */
size_t c0_record_count(c0_record r);
/** Raw unit n (0-based), or C0_ERR_NOT_FOUND. */
c0_status c0_record_field(c0_record r, size_t n, c0_bytes *raw);

/** Cursor over keyed units (a keyed record, or a document heading). */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t stop;
    c0_error err;
} c0_pair_iter;

/** Keyed units in written order, duplicates included, dead units skipped.
 * `key` is the name; `raw` the unit's raw value. */
c0_pair_iter c0_record_pairs(c0_record r);
int c0_next_pair(c0_pair_iter *it, c0_bytes *key, c0_bytes *raw);

/** Raw value for a key (last wins), or C0_ERR_NOT_FOUND. */
c0_status c0_record_get(c0_record r, const uint8_t *key, size_t klen, c0_bytes *raw);
c0_status c0_record_get_str(c0_record r, const char *key, c0_bytes *raw);

/** A raw unit as a flat list: a level holding at most one positional record.
 * On C0_OK `items` iterates the items (none for the empty level). A level
 * with several records, a keyed record, groups or files is C0_ERR_NOT_LIST. */
c0_status c0_unit_list(c0_bytes raw, c0_unit_iter *items);

/* ---- Bodies: files, groups, records ------------------------------------ */

/** A file within a body: its name and the range of its contents. */
typedef struct {
    const uint8_t *buf;
    size_t name_start;
    size_t name_end;
    size_t start;
    size_t stop;
} c0_file;

/** A group: its depth (number of GS; 0 for the unnamed group at the start of
 * a file or level), name, and the range of its contents (header row and
 * records; deeper GS-repeated sections are separate groups). */
typedef struct {
    const uint8_t *buf;
    size_t depth;
    size_t name_start;
    size_t name_end;
    size_t start;
    size_t stop;
} c0_group;

c0_bytes c0_file_name(c0_file f);
c0_bytes c0_group_name(c0_group g);

/** Cursor over the files of a body. Records and groups before the first FS
 * form an unnamed file, reported only when it has content or no FS follows.
 * A dead FS line is skipped: what follows it stays in the previous file. */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t stop;
    int first;
    int done;
    c0_error err;
} c0_file_iter;

c0_file_iter c0_body_files(c0_body b);
int c0_next_file(c0_file_iter *it, c0_file *out);

/** Cursor over groups: every GS line at any depth, in order, plus the
 * unnamed leading group of a file or level when it holds a header row or
 * records. A dead GS line is skipped: its records stay in the group before. */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t stop;
    int first;
    int done;
    c0_error err;
} c0_group_iter;

/** Groups of one file. */
c0_group_iter c0_file_groups(c0_file f);
/** Groups of every file of a body, in order (FS lines are skipped). */
c0_group_iter c0_body_groups(c0_body b);
int c0_next_group(c0_group_iter *it, c0_group *out);

/** First group with the given name in the body (any file), or C0_ERR_NOT_FOUND. */
c0_status c0_body_find_group(c0_body b, const uint8_t *name, size_t nlen, c0_group *out);
c0_status c0_body_find_group_str(c0_body b, const char *name, c0_group *out);

/** Whether the body is a bare record stream: one unnamed file, at most the
 * unnamed group. */
int c0_body_bare(c0_body b);

/** The group's header row as a record (its units are the header names).
 * Returns 0 when the group has none. */
int c0_group_header(c0_group g, c0_record *out);

/** Cursor over the live records of a group (header rows, notes, commits and
 * dead elements skipped). */
typedef struct {
    const uint8_t *buf;
    size_t pos;
    size_t stop;
    int done;
    c0_error err;
} c0_record_iter;

c0_record_iter c0_group_records(c0_group g);
/** Records of a bare body (the unnamed group's). */
c0_record_iter c0_body_records(c0_body b);
int c0_next_record(c0_record_iter *it, c0_record *out);

/** Index of a header name in the group's header row, or C0_ERR_NOT_FOUND. */
c0_status c0_group_column(c0_group g, const uint8_t *name, size_t nlen, size_t *index);

/* ---- Documents and containers ------------------------------------------ */

/** One document: an optional SYN heading, a body, an optional EOT. */
typedef struct {
    const uint8_t *buf;
    size_t start;
    size_t stop;          /**< after the EOT, or where the document ends */
    int has_syn;
    int has_eot;
    size_t heading_start; /**< the keyed units after SYN … */
    size_t heading_end;   /**< … up to the first body code */
    c0_body body;
    unsigned warnings;    /**< C0_WARN_* bits */
} c0_document;

/** Read the document starting at `start` (after any layout). Returns C0_OK or
 * the first structural error met while finding the heading and the body's
 * end. Shapes inside the body are not checked here: use c0_validate. */
c0_status c0_document_read(const uint8_t *buf, size_t len, size_t start, c0_document *out, c0_error *err);

/** The heading's keyed units, in written order (dead units skipped). */
c0_pair_iter c0_document_heading(const c0_document *d);
/** Raw heading value for a key (last wins), or C0_ERR_NOT_FOUND. */
c0_status c0_heading_get(const c0_document *d, const uint8_t *key, size_t klen, c0_bytes *raw);
c0_status c0_heading_get_str(const c0_document *d, const char *key, c0_bytes *raw);
/** The `shape` value (decoded, must be plain), or an empty slice when absent. */
c0_bytes c0_heading_shape(const c0_document *d);
/** The `version` value as an integer; 1 when absent or not a number. */
int c0_heading_version(const c0_document *d);
/** The `blob` table (a level) as a body, or C0_ERR_NOT_FOUND / C0_ERR_NOT_LEVEL. */
c0_status c0_heading_blob(const c0_document *d, c0_body *out);

/** Cursor over the documents of a container (a file or message): each begins
 * with SYN except possibly the first; EM ends the scan and starts the tail. */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t pos;
    int has_em;
    size_t em_pos;
    int done;
    c0_error err;
} c0_doc_iter;

c0_doc_iter c0_documents(const uint8_t *buf, size_t len);
int c0_next_document(c0_doc_iter *it, c0_document *out);
/** After the cursor is exhausted: the raw bytes after EM (empty without EM). */
c0_bytes c0_doc_iter_tail(const c0_doc_iter *it);

/** Scan the whole container: *tail gets the raw region after EM (empty when
 * there is none, *has_em says which). Returns the first error met. */
c0_status c0_container_tail(const uint8_t *buf, size_t len, c0_bytes *tail, int *has_em, c0_error *err);

/** The bytes of a named blob from the tail, located by the first document's
 * `blob` table (columns matched by header name). C0_ERR_NOT_FOUND when there
 * is no table, no such row, or no EM. */
c0_status c0_blob(const uint8_t *buf, size_t len, const uint8_t *name, size_t nlen, c0_bytes *out, c0_error *err);
c0_status c0_blob_str(const uint8_t *buf, size_t len, const char *name, c0_bytes *out, c0_error *err);

/* ---- Builder (writer) --------------------------------------------------- */

/**
 * Writes C0DATA in canonical compact form into a growable buffer it owns;
 * call c0_builder_free when done. Once `status` is set, further writes are
 * no-ops; check it before using the bytes. Values are byte-transparent and
 * written in their one spelling: plain when they can be, text otherwise with
 * only the seven codes escaped. Names (file, group, header, key, reference
 * segment, ETB payload) must be plain, or C0_ERR_BAD_NAME is set.
 *
 * Two layers. The primitives, `c0_builder_<code>`, are one function per
 * code (fs gs rs us soh dle can bel enq syn eot etb em); each writes its code
 * and, when given, what grammatically follows it, so a sequence of calls
 * reads like the bytes:
 *
 *   c0_builder_rs_str(&b, "Alice"); c0_builder_us_str(&b, "1502.30");   ␞Alice␟1502.30
 *   c0_builder_rs(&b); c0_builder_soh_str(&b, "host"); c0_builder_us_str(&b, "localhost");
 *   c0_builder_bel(&b); c0_builder_gs_str(&b, "old");                   a dead group
 *
 * The composed functions, `c0_build_<what>` (file, group, header, record,
 * field, map, key, level, list, ref, note), are built on the primitives and
 * cannot write ill-formed output. The primitives write what you spell;
 * well-formedness is then yours to keep. Both layers share the record state,
 * so they mix.
 *
 * A record is opened by c0_build_record or c0_builder_rs (or implicitly by
 * the first c0_build_field / c0_build_key / c0_build_level_open /
 * c0_build_list / c0_build_ref) and ended by the next structural call. Keyed
 * units written right after c0_build_syn belong to the heading, not to a
 * record.
 */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    c0_status status;
    int state;        /**< internal: top / heading / record */
    size_t units;     /**< internal: units written in the open record */
    int *stack;       /**< internal: saved state per open level */
    size_t depth;     /**< internal: open levels */
    size_t stack_cap; /**< internal */
} c0_builder;

void c0_builder_init(c0_builder *b);
void c0_builder_free(c0_builder *b);
c0_status c0_builder_status(const c0_builder *b);
c0_bytes c0_builder_bytes(const c0_builder *b);

/* primitives: one function per code. The bare form writes the code alone;
 * the `_name` / `_value` / `_text` / `_payload` form writes the code and what
 * follows it; `_str` is that form on a NUL-terminated string. Values take
 * their one spelling (plain or text); names must be plain. */
void c0_builder_fs(c0_builder *b, const uint8_t *name, size_t nlen);        /**< FS name; ends any open record */
void c0_builder_gs(c0_builder *b);                                           /**< GS alone: one step of depth (gs, gs_name is depth 2) */
void c0_builder_gs_name(c0_builder *b, const uint8_t *name, size_t nlen);   /**< GS name; ends any open record */
void c0_builder_rs(c0_builder *b);                                           /**< RS: open a record */
void c0_builder_rs_value(c0_builder *b, const uint8_t *value, size_t vlen); /**< RS and the record's first value */
void c0_builder_us(c0_builder *b);                                           /**< US alone (an empty unit, or before DLE / ENQ) */
void c0_builder_us_value(c0_builder *b, const uint8_t *value, size_t vlen); /**< US and the value that follows it */
void c0_builder_soh(c0_builder *b);                                          /**< SOH alone: before RS, a header row */
void c0_builder_soh_name(c0_builder *b, const uint8_t *name, size_t nlen);  /**< SOH key */
void c0_builder_dle(c0_builder *b);                                          /**< DLE: open a level (saves the record state) */
void c0_builder_can(c0_builder *b);                                          /**< CAN: close the innermost level; C0_ERR_BAD_STATE if none is open */
void c0_builder_bel(c0_builder *b);                                          /**< BEL alone: the element that follows is dead */
void c0_builder_bel_text(c0_builder *b, const uint8_t *text, size_t tlen);  /**< BEL and a note; empty text is C0_ERR_BAD_STATE */
void c0_builder_enq(c0_builder *b, const uint8_t *name, size_t nlen);       /**< ENQ name */
void c0_builder_enq_path(c0_builder *b, const c0_bytes *segments, size_t count); /**< ENQ STX seg US seg [US seg] ETX (one segment: ENQ name) */
void c0_builder_syn(c0_builder *b);                                          /**< SYN; soh_name + us_value pairs that follow are the heading */
void c0_builder_eot(c0_builder *b);                                          /**< EOT */
void c0_builder_etb(c0_builder *b);                                          /**< ETB alone */
void c0_builder_etb_payload(c0_builder *b, const uint8_t *payload, size_t plen); /**< ETB and a plain payload */
void c0_builder_em(c0_builder *b, const uint8_t *tail, size_t tlen);        /**< EM and the raw tail; nothing may follow */

void c0_builder_fs_str(c0_builder *b, const char *name);
void c0_builder_gs_str(c0_builder *b, const char *name);
void c0_builder_rs_str(c0_builder *b, const char *value);
void c0_builder_us_str(c0_builder *b, const char *value);
void c0_builder_soh_str(c0_builder *b, const char *name);
void c0_builder_bel_str(c0_builder *b, const char *text);
void c0_builder_enq_str(c0_builder *b, const char *name);
void c0_builder_enq_path_str(c0_builder *b, const char *const *segments, size_t count);
void c0_builder_etb_str(c0_builder *b, const char *payload);
void c0_builder_em_str(c0_builder *b, const char *tail);

/* composed: cannot write ill-formed output */

/* documents */
void c0_build_syn(c0_builder *b);                                     /**< SYN; keyed units until the next element are the heading */
void c0_build_eot(c0_builder *b);                                     /**< EOT */
void c0_build_etb(c0_builder *b, const uint8_t *payload, size_t plen); /**< ETB and an optional plain payload (NULL for none) */
void c0_build_em(c0_builder *b, const uint8_t *tail, size_t tlen);     /**< EM and the raw tail; nothing may follow */

/* structure */
void c0_build_file(c0_builder *b, const uint8_t *name, size_t nlen);               /**< FS name */
void c0_build_group(c0_builder *b, const uint8_t *name, size_t nlen);              /**< GS name */
void c0_build_section(c0_builder *b, const uint8_t *name, size_t nlen, size_t depth); /**< GS×depth name */
void c0_build_header(c0_builder *b, const c0_bytes *names, size_t count);         /**< SOH RS name US name … */

/* records */
void c0_build_record(c0_builder *b, const c0_bytes *fields, size_t count); /**< RS and `count` units; count 0 opens an empty record */
void c0_build_field(c0_builder *b, const uint8_t *value, size_t vlen);    /**< next unit of the open record (opens one if none) */
void c0_build_map(c0_builder *b, const c0_bytes *keys, const c0_bytes *values, size_t count); /**< RS and keyed units in the order given */
void c0_build_key(c0_builder *b, const uint8_t *key, size_t klen, const uint8_t *value, size_t vlen); /**< SOH key US value, in the open record or the heading */
void c0_build_level_open(c0_builder *b);   /**< DLE: a nested level as the next unit; inside, the builder is at the top of a fresh body */
void c0_build_key_level_open(c0_builder *b, const uint8_t *key, size_t klen); /**< SOH key US DLE */
void c0_build_level_close(c0_builder *b);  /**< CAN */
void c0_build_list(c0_builder *b, const c0_bytes *items, size_t count); /**< a flat list as the next unit: DLE RS item US item … CAN, or DLE CAN */
void c0_build_ref(c0_builder *b, const uint8_t *name, size_t nlen);     /**< ENQ name as the next unit */
void c0_build_ref_path(c0_builder *b, const c0_bytes *segments, size_t count); /**< ENQ, then STX seg US seg [US seg] ETX (one segment: plain) */
void c0_build_note(c0_builder *b, const uint8_t *text, size_t tlen);     /**< BEL and the note (plain, or text when it must be) */

/* document mode */
void c0_build_block(c0_builder *b, const uint8_t *text, size_t tlen); /**< a paragraph: a record of one unit */
void c0_build_item(c0_builder *b, const uint8_t *text, size_t tlen);  /**< a list item: the next unit */

/* NUL-terminated convenience wrappers */
void c0_build_etb_str(c0_builder *b, const char *payload);
void c0_build_file_str(c0_builder *b, const char *name);
void c0_build_group_str(c0_builder *b, const char *name);
void c0_build_section_str(c0_builder *b, const char *name, size_t depth);
void c0_build_header_str(c0_builder *b, const char *const *names, size_t count);
void c0_build_record_str(c0_builder *b, const char *const *fields, size_t count);
void c0_build_field_str(c0_builder *b, const char *value);
void c0_build_map_str(c0_builder *b, const char *const *keys, const char *const *values, size_t count);
void c0_build_key_str(c0_builder *b, const char *key, const char *value);
void c0_build_key_level_open_str(c0_builder *b, const char *key);
void c0_build_list_str(c0_builder *b, const char *const *items, size_t count);
void c0_build_ref_str(c0_builder *b, const char *name);
void c0_build_ref_path_str(c0_builder *b, const char *const *segments, size_t count);
void c0_build_note_str(c0_builder *b, const char *text);
void c0_build_block_str(c0_builder *b, const char *text);
void c0_build_item_str(c0_builder *b, const char *text);

/* ---- Stream mode (ETB commits) ----------------------------------------- */

/** A scan of an append-only log. An unescaped ETB is always a commit, inside
 * text or a level too; a block so cut is damaged and yields nothing. The
 * tail after the last commit is torn unless it holds only layout, notes or
 * EOT. */
typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t committed_end; /**< just past the last ETB and its payload */
    int torn;
    size_t block_count;
    size_t damaged_count;
} c0_stream;

c0_stream c0_stream_read(const uint8_t *buf, size_t len);
c0_bytes c0_stream_committed(const c0_stream *s);
c0_bytes c0_stream_tail(const c0_stream *s);

/** Cursor over the committed blocks (previous commit to ETB, marker and
 * payload excluded); *damaged is set for a block cut inside text or a level. */
typedef struct {
    const uint8_t *buf;
    size_t stop;
    size_t pos;
    size_t block_start;
    int in_text;
    size_t depth;
} c0_block_iter;

c0_block_iter c0_stream_blocks(const c0_stream *s);
int c0_next_block(c0_block_iter *it, c0_bytes *out, int *damaged);

/** Copy the undamaged committed blocks, without their ETB markers and
 * payloads, into `out` (room for committed_end bytes suffices). Returns the
 * length. Read the result with c0_document_read to get the records. */
size_t c0_stream_undamaged(const c0_stream *s, uint8_t *out);

/** Position just past the last commit, found by a backward scan (a shortcut
 * for well-formed input; c0_stream_read is authoritative). */
size_t c0_stream_last_commit_end(const uint8_t *buf, size_t len);

/* ---- Pretty form (Unicode Control Pictures, U+2400 block) -------------- */

typedef enum {
    C0_PRETTY_COMPACT = 0, /**< one element per line, no padding            */
    C0_PRETTY_ALIGNED,     /**< table columns aligned                       */
    C0_PRETTY_SPACED       /**< aligned, plus a space after each prefix and around ␟ */
} c0_pretty_mode;

/**
 * Format compact bytes as pretty form: a malloc'd, NUL-terminated UTF-8
 * string the caller frees; *outlen (if not NULL) gets its length. `indent`
 * defaults to two spaces when NULL. NULL on error: C0_ERR_OOM, a syntax
 * error, or C0_ERR_SYNTAX with msg "glyph in data" when a value holds a
 * Control Picture character and cannot be shown unambiguously.
 */
char *c0_pretty_format(const uint8_t *buf, size_t len, const char *indent, c0_pretty_mode mode, size_t *outlen, c0_error *err);

/** Parse pretty form back to compact bytes: glyphs become codes; outside text
 * whitespace touching a glyph or a line edge is dropped. Returns a malloc'd
 * buffer the caller frees (NULL on allocation failure); *outlen gets the length. */
uint8_t *c0_pretty_parse(const char *str, size_t len, size_t *outlen);

#ifdef __cplusplus
}
#endif

/** ====================================================================== */
#ifdef C0_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

/* ---- tables ------------------------------------------------------------ */

static const unsigned char c0__assigned[32] = {
    0, 1, 1, 1, 1, 1, 0, 1, /* 0x00-0x07: SOH STX ETX EOT ENQ BEL */
    0, 0, 0, 0, 0, 0, 0, 0, /* 0x08-0x0F */
    1, 0, 0, 0, 0, 0, 1, 1, /* 0x10-0x17: DLE SYN ETB */
    1, 1, 1, 0, 1, 1, 1, 1  /* 0x18-0x1F: CAN EM SUB FS GS RS US */
};

static const unsigned char c0__text_escaped[32] = {
    0, 0, 1, 1, 1, 0, 0, 0, /* STX ETX EOT */
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 1, 1, /* DLE SYN ETB */
    0, 1, 0, 0, 0, 0, 0, 0  /* EM */
};

int c0_is_assigned(uint8_t byte) { return byte < 0x20 && c0__assigned[byte]; }
int c0_is_layout(uint8_t byte) { return byte == C0_SP || byte == C0_HT || byte == C0_LF || byte == C0_CR; }
int c0_is_text_escaped(uint8_t byte) { return byte < 0x20 && c0__text_escaped[byte]; }

int c0_is_plain(const uint8_t *p, size_t len) {
    size_t i;
    if (len == 0) return 1;
    if (c0_is_layout(p[0]) || c0_is_layout(p[len - 1])) return 0;
    for (i = 0; i < len; i++) {
        if (p[i] < 0x20) return 0;
    }
    return 1;
}

int c0_is_name(const uint8_t *p, size_t len) { return c0_is_plain(p, len); }

c0_bytes c0_bytes_of(const uint8_t *ptr, size_t len) {
    c0_bytes b;
    b.ptr = ptr;
    b.len = len;
    return b;
}

c0_bytes c0_trim(c0_bytes raw) {
    size_t s = 0, e = raw.len;
    while (s < e && c0_is_layout(raw.ptr[s])) s++;
    while (e > s && c0_is_layout(raw.ptr[e - 1])) e--;
    raw.ptr += s;
    raw.len = e - s;
    return raw;
}

size_t c0_decode_text(const uint8_t *in, size_t len, uint8_t *out) {
    size_t i = 0, n = 0;
    while (i < len) {
        if (in[i] == C0_DLE) {
            i++;
            if (i >= len) break;
        }
        out[n++] = in[i++];
    }
    return n;
}

/* ---- errors ------------------------------------------------------------ */

static void c0__fail(c0_error *err, c0_status code, size_t pos, const char *msg) {
    if (err && err->code == C0_OK) {
        err->code = code;
        err->pos = pos;
        err->msg = msg;
    }
}

static void c0__err_clear(c0_error *err) {
    if (err) {
        err->code = C0_OK;
        err->pos = 0;
        err->msg = "";
    }
}

static c0_bytes c0__slice(const uint8_t *buf, size_t s, size_t e) {
    c0_bytes b;
    b.ptr = buf + s;
    b.len = e > s ? e - s : 0;
    return b;
}

/* ---- scanning primitives (c0.cr `Scan`) --------------------------------
 * Each takes a buffer and a position and returns the position after the
 * construct. On malformed input it records the error and returns `stop`, so
 * callers' loops end; callers that care check err->code. */

static size_t c0__layout(const uint8_t *buf, size_t pos, size_t stop) {
    while (pos < stop && c0_is_layout(buf[pos])) pos++;
    return pos;
}

static size_t c0__plain(const uint8_t *buf, size_t pos, size_t stop) {
    while (pos < stop && buf[pos] >= 0x20) pos++;
    return pos;
}

/* At the STX of a text: the position after its ETX. */
static size_t c0__text(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    size_t start = pos;
    pos++;
    while (pos < stop) {
        uint8_t b = buf[pos];
        if (b == C0_DLE) {
            pos++;
            if (pos >= stop) {
                c0__fail(err, C0_ERR_UNEXPECTED_END, pos, "DLE at end of text");
                return stop;
            }
            if (!c0_is_text_escaped(buf[pos])) {
                c0__fail(err, C0_ERR_SYNTAX, pos, "DLE before a byte that is not one of the seven");
                return stop;
            }
            pos++;
        } else if (b == C0_ETX) {
            return pos + 1;
        } else if (b == C0_STX) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "STX inside text (text does not nest)");
            return stop;
        } else if (b == C0_ETB || b == C0_EOT || b == C0_SYN || b == C0_EM) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "framing inside open text");
            return stop;
        } else {
            pos++;
        }
    }
    c0__fail(err, C0_ERR_UNEXPECTED_END, start, "text is not closed");
    return stop;
}

/* At the DLE of a level: the position after its matching CAN. */
static size_t c0__level(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    size_t start = pos, depth = 1;
    pos++;
    while (pos < stop) {
        uint8_t b = buf[pos];
        if (b >= 0x20) {
            pos++;
        } else if (b == C0_STX) {
            pos = c0__text(buf, pos, stop, err);
            if (err->code) return stop;
        } else if (b == C0_DLE) {
            depth++;
            pos++;
        } else if (b == C0_CAN) {
            depth--;
            pos++;
            if (depth == 0) return pos;
        } else if (b == C0_ETB || b == C0_EOT || b == C0_SYN || b == C0_EM) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "framing inside open level");
            return stop;
        } else {
            pos++;
        }
    }
    c0__fail(err, C0_ERR_UNEXPECTED_END, start, "level is not closed");
    return stop;
}

static size_t c0__comment(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
static size_t c0__record_end(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);
static size_t c0__keyed_unit_end(const uint8_t *buf, size_t pos, size_t stop, c0_error *err);

/* Is the BEL at pos followed (after layout) by a code other than STX/DLE,
 * i.e. does it kill a record-level element? */
static int c0__bel_kills_element(const uint8_t *buf, size_t pos, size_t stop) {
    size_t p2 = c0__layout(buf, pos + 1, stop);
    return p2 < stop && buf[p2] < 0x20 && buf[p2] != C0_STX && buf[p2] != C0_DLE;
}

/* From just after a US (or a key's US): the end of one unit's value, i.e.
 * the next control code at this depth that does not belong to the value.
 * Layout at the edges stays inside the returned range. */
static size_t c0__unit(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    while (pos < stop) {
        uint8_t b = buf[pos];
        if (b >= 0x20 || c0_is_layout(b)) {
            pos++;
        } else if (b == C0_STX) {
            pos = c0__text(buf, pos, stop, err);
            if (err->code) return stop;
        } else if (b == C0_DLE) {
            pos = c0__level(buf, pos, stop, err);
            if (err->code) return stop;
        } else if (b == C0_ENQ) {
            pos++;
        } else if (b == C0_BEL) {
            if (c0__bel_kills_element(buf, pos, stop)) return pos;
            pos = c0__comment(buf, pos, stop, err);
            if (err->code) return stop;
        } else {
            return pos;
        }
    }
    return pos;
}

/* At a BEL: skip the note (plain or text) or the whole dead element. */
static size_t c0__comment(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    uint8_t b;
    pos = c0__layout(buf, pos + 1, stop);
    if (pos >= stop) {
        c0__fail(err, C0_ERR_UNEXPECTED_END, pos, "BEL at end of input");
        return stop;
    }
    b = buf[pos];
    if (b >= 0x20) return c0__plain(buf, pos, stop);
    if (b == C0_STX) return c0__text(buf, pos, stop, err);
    if (b == C0_DLE) return c0__level(buf, pos, stop, err);
    if (b == C0_RS) return c0__record_end(buf, pos + 1, stop, err);
    if (b == C0_SOH) {
        size_t p2 = c0__layout(buf, pos + 1, stop);
        if (p2 < stop && buf[p2] == C0_RS) return c0__record_end(buf, p2 + 1, stop, err);
        return c0__keyed_unit_end(buf, pos + 1, stop, err);
    }
    if (b == C0_GS) {
        while (pos < stop && (buf[pos] == C0_GS || c0_is_layout(buf[pos]))) pos++;
        return c0__plain(buf, pos, stop);
    }
    if (b == C0_FS) return c0__plain(buf, pos + 1, stop);
    c0__fail(err, C0_ERR_SYNTAX, pos, "BEL before a code that is not an element");
    return stop;
}

/* From just after an RS: the end of the record, i.e. the next record-level
 * code at this depth. */
static size_t c0__record_end(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    while (pos < stop) {
        uint8_t b = buf[pos];
        if (b >= 0x20 || c0_is_layout(b) || b == C0_US || b == C0_ENQ) {
            pos++;
        } else if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_RS) return pos; /* a header row */
            pos++;
        } else if (b == C0_STX) {
            pos = c0__text(buf, pos, stop, err);
            if (err->code) return stop;
        } else if (b == C0_DLE) {
            pos = c0__level(buf, pos, stop, err);
            if (err->code) return stop;
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && (buf[p2] == C0_RS || buf[p2] == C0_GS || buf[p2] == C0_FS)) return pos;
            if (p2 < stop && buf[p2] == C0_SOH) {
                size_t p3 = c0__layout(buf, p2 + 1, stop);
                if (p3 < stop && buf[p3] == C0_RS) return pos;
            }
            pos = c0__comment(buf, pos, stop, err);
            if (err->code) return stop;
        } else {
            return pos;
        }
    }
    return pos;
}

/* From just after an SOH that is not a header row: past the key, US and value. */
static size_t c0__keyed_unit_end(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) {
    pos = c0__layout(buf, pos, stop);
    pos = c0__plain(buf, pos, stop);
    if (!(pos < stop && buf[pos] == C0_US)) {
        c0__fail(err, C0_ERR_SYNTAX, pos, "key without a value (no US)");
        return stop;
    }
    return c0__unit(buf, pos + 1, stop, err);
}

static size_t c0__trim_end(const uint8_t *buf, size_t start, size_t stop) {
    while (stop > start && c0_is_layout(buf[stop - 1])) stop--;
    return stop;
}

#define C0__SCAN_WRAP(name, inner) \
    size_t name(const uint8_t *buf, size_t pos, size_t stop, c0_error *err) { \
        c0_error local = {C0_OK, 0, ""}; \
        if (!err) err = &local; \
        c0__err_clear(err); \
        return inner(buf, pos, stop, err); \
    }
size_t c0_scan_layout(const uint8_t *buf, size_t pos, size_t stop) { return c0__layout(buf, pos, stop); }
size_t c0_scan_plain(const uint8_t *buf, size_t pos, size_t stop) { return c0__plain(buf, pos, stop); }
C0__SCAN_WRAP(c0_scan_text, c0__text)
C0__SCAN_WRAP(c0_scan_level, c0__level)
C0__SCAN_WRAP(c0_scan_unit, c0__unit)
C0__SCAN_WRAP(c0_scan_comment, c0__comment)
C0__SCAN_WRAP(c0_scan_record_end, c0__record_end)

/* ---- values ------------------------------------------------------------ */

c0_unit_kind c0_unit_type(c0_bytes raw) {
    c0_bytes t = c0_trim(raw);
    if (t.len == 0) return C0_UNIT_PLAIN;
    switch (t.ptr[0]) {
        case C0_STX: return C0_UNIT_TEXT;
        case C0_DLE: return C0_UNIT_LEVEL;
        case C0_ENQ: return C0_UNIT_REF;
        default:     return C0_UNIT_PLAIN;
    }
}

/* From pos to the end of t only notes (and layout) may follow. */
static c0_status c0__only_notes_after(c0_bytes t, size_t pos) {
    c0_error err = {C0_OK, 0, ""};
    pos = c0__layout(t.ptr, pos, t.len);
    while (pos < t.len) {
        if (t.ptr[pos] != C0_BEL) return C0_ERR_SYNTAX;
        pos = c0__layout(t.ptr, c0__comment(t.ptr, pos, t.len, &err), t.len);
        if (err.code) return err.code;
    }
    return C0_OK;
}

c0_status c0_value(c0_bytes raw, uint8_t *scratch, c0_bytes *out) {
    c0_bytes t = c0_trim(raw);
    c0_error err = {C0_OK, 0, ""};
    size_t i;
    out->ptr = t.ptr;
    out->len = 0;
    if (t.len == 0) return C0_OK;
    switch (t.ptr[0]) {
        case C0_STX: {
            size_t stop = c0__text(t.ptr, 0, t.len, &err);
            c0_status st;
            if (err.code) return err.code;
            st = c0__only_notes_after(t, stop);
            if (st) return st;
            out->ptr = t.ptr + 1;
            out->len = stop - 2;
            for (i = 0; i < out->len; i++) {
                if (out->ptr[i] == C0_DLE) {
                    if (!scratch) return C0_ERR_NEEDS_DECODE;
                    out->len = c0_decode_text(out->ptr, out->len, scratch);
                    out->ptr = scratch;
                    break;
                }
            }
            return C0_OK;
        }
        case C0_BEL:
            return c0__only_notes_after(t, 0); /* a note or a dead level: the unit is empty */
        case C0_DLE:
        case C0_ENQ:
            return C0_ERR_NOT_SCALAR;
        default:
            for (i = 0; i < t.len; i++) {
                uint8_t b = t.ptr[i];
                if (b == C0_BEL) {
                    c0_status st = c0__only_notes_after(t, i);
                    if (st) return st;
                    *out = c0_trim(c0__slice(t.ptr, 0, i));
                    return C0_OK;
                }
                if (b < 0x20) return C0_ERR_SYNTAX; /* layout or control code inside a plain value */
            }
            *out = t;
            return C0_OK;
    }
}

c0_body c0_body_view(const uint8_t *buf, size_t len) {
    c0_body b;
    b.buf = buf;
    b.start = 0;
    b.stop = len;
    return b;
}

c0_status c0_unit_level(c0_bytes raw, c0_body *out) {
    c0_bytes t = c0_trim(raw);
    c0_error err = {C0_OK, 0, ""};
    size_t stop;
    if (!(t.len > 0 && t.ptr[0] == C0_DLE)) return C0_ERR_NOT_LEVEL;
    stop = c0__level(t.ptr, 0, t.len, &err);
    if (err.code) return err.code;
    out->buf = t.ptr;
    out->start = 1;
    out->stop = stop - 1;
    return C0_OK;
}

c0_status c0_unit_ref(c0_bytes raw, c0_ref *out) {
    c0_bytes t = c0_trim(raw);
    size_t p;
    out->count = 0;
    if (!(t.len > 0 && t.ptr[0] == C0_ENQ)) return C0_ERR_NOT_REFERENCE;
    p = c0__layout(t.ptr, 1, t.len);
    if (p < t.len && t.ptr[p] == C0_STX) {
        c0_error err = {C0_OK, 0, ""};
        size_t stop = c0__text(t.ptr, p, t.len, &err), s, i;
        if (err.code) return err.code;
        s = p + 1;
        for (i = s; i <= stop - 1; i++) {
            if (i == stop - 1 || t.ptr[i] == C0_US) {
                if (out->count == 3) return C0_ERR_SYNTAX; /* more than three segments */
                if (i == s) return C0_ERR_SYNTAX;          /* empty segment */
                out->seg[out->count] = c0__slice(t.ptr, s, i);
                if (!c0_is_name(out->seg[out->count].ptr, out->seg[out->count].len)) return C0_ERR_SYNTAX;
                out->count++;
                s = i + 1;
            }
        }
        return C0_OK;
    } else {
        c0_bytes name = c0_trim(c0__slice(t.ptr, p, t.len));
        if (name.len == 0) return C0_ERR_SYNTAX; /* reference without a name */
        if (!c0_is_name(name.ptr, name.len)) return C0_ERR_SYNTAX;
        out->seg[0] = name;
        out->count = 1;
        return C0_OK;
    }
}

/* ---- records ----------------------------------------------------------- */

int c0_record_keyed(c0_record r) {
    size_t p = c0__layout(r.buf, r.start, r.stop);
    c0_error err = {C0_OK, 0, ""};
    while (p < r.stop && r.buf[p] == C0_BEL) {
        size_t p2 = c0__layout(r.buf, p + 1, r.stop);
        if (p2 < r.stop && r.buf[p2] == C0_SOH) return 1;
        if (p2 < r.stop && r.buf[p2] < 0x20 && r.buf[p2] != C0_STX && r.buf[p2] != C0_DLE) break;
        p = c0__layout(r.buf, c0__comment(r.buf, p, r.stop, &err), r.stop);
        if (err.code) return 0;
    }
    return p < r.stop && r.buf[p] == C0_SOH;
}

c0_unit_iter c0_record_units(c0_record r) {
    c0_unit_iter it;
    it.buf = r.buf;
    it.pos = r.start;
    it.stop = r.stop;
    it.done = c0_record_keyed(r);
    c0__err_clear(&it.err);
    return it;
}

int c0_next_unit(c0_unit_iter *it, c0_bytes *raw) {
    size_t s, e;
    if (it->done) return 0;
    s = it->pos;
    e = c0__unit(it->buf, s, it->stop, &it->err);
    if (it->err.code) {
        it->done = 1;
        return 0;
    }
    *raw = c0__slice(it->buf, s, e);
    if (e < it->stop && it->buf[e] == C0_US) {
        it->pos = e + 1;
    } else {
        it->done = 1;
    }
    return 1;
}

size_t c0_record_count(c0_record r) {
    c0_unit_iter it = c0_record_units(r);
    c0_bytes u;
    size_t n = 0;
    while (c0_next_unit(&it, &u)) n++;
    return n;
}

c0_status c0_record_field(c0_record r, size_t n, c0_bytes *raw) {
    c0_unit_iter it = c0_record_units(r);
    size_t i = 0;
    while (c0_next_unit(&it, raw)) {
        if (i == n) return C0_OK;
        i++;
    }
    return it.err.code ? it.err.code : C0_ERR_NOT_FOUND;
}

static c0_pair_iter c0__pairs(const uint8_t *buf, size_t start, size_t stop) {
    c0_pair_iter it;
    it.buf = buf;
    it.pos = start;
    it.stop = stop;
    c0__err_clear(&it.err);
    return it;
}

c0_pair_iter c0_record_pairs(c0_record r) { return c0__pairs(r.buf, r.start, r.stop); }

int c0_next_pair(c0_pair_iter *it, c0_bytes *key, c0_bytes *raw) {
    const uint8_t *buf = it->buf;
    while (it->pos < it->stop) {
        uint8_t b = buf[it->pos];
        if (b == C0_SOH) {
            size_t ks = c0__layout(buf, it->pos + 1, it->stop);
            size_t ke = c0__plain(buf, ks, it->stop), vs, ve;
            if (!(ke < it->stop && buf[ke] == C0_US)) {
                c0__fail(&it->err, C0_ERR_SYNTAX, ke, "key without a value (no US)");
                it->pos = it->stop;
                return 0;
            }
            vs = ke + 1;
            ve = c0__unit(buf, vs, it->stop, &it->err);
            if (it->err.code) {
                it->pos = it->stop;
                return 0;
            }
            *key = c0_trim(c0__slice(buf, ks, ke));
            *raw = c0__slice(buf, vs, ve);
            it->pos = ve;
            return 1;
        } else if (b == C0_BEL) {
            it->pos = c0__comment(buf, it->pos, it->stop, &it->err);
            if (it->err.code) return 0;
        } else if (c0_is_layout(b)) {
            it->pos++;
        } else {
            it->pos = it->stop;
            return 0;
        }
    }
    return 0;
}

static c0_status c0__pairs_get(c0_pair_iter it, const uint8_t *key, size_t klen, c0_bytes *raw) {
    c0_bytes k, v;
    int found = 0;
    while (c0_next_pair(&it, &k, &v)) {
        if (k.len == klen && (klen == 0 || memcmp(k.ptr, key, klen) == 0)) {
            *raw = v;
            found = 1;
        }
    }
    if (it.err.code) return it.err.code;
    return found ? C0_OK : C0_ERR_NOT_FOUND;
}

c0_status c0_record_get(c0_record r, const uint8_t *key, size_t klen, c0_bytes *raw) {
    return c0__pairs_get(c0_record_pairs(r), key, klen, raw);
}

c0_status c0_record_get_str(c0_record r, const char *key, c0_bytes *raw) {
    return c0_record_get(r, (const uint8_t *)key, strlen(key), raw);
}

/* ---- element scanner over a body range ---------------------------------
 * Yields the next record-level element at this depth: a record, a header
 * row, a group line or a file line, live or dead. Notes and commits are
 * skipped. */

typedef enum { C0__EL_END = 0, C0__EL_RECORD, C0__EL_HEADER, C0__EL_GROUP, C0__EL_FILE } c0__el_kind;

typedef struct {
    c0__el_kind kind;
    int dead;
    size_t depth;  /* GROUP: number of GS */
    size_t a, b;   /* RECORD/HEADER: the record's range; GROUP/FILE: the name's range */
    size_t at;     /* where the element (or its BEL) begins */
    size_t next;   /* position after the element */
} c0__el;

static void c0__el_record(const uint8_t *buf, size_t rs, size_t stop, c0__el *el, c0__el_kind kind, c0_error *err) {
    size_t e = c0__record_end(buf, rs, stop, err);
    el->kind = kind;
    el->a = rs;
    el->b = c0__trim_end(buf, rs, e);
    el->next = e;
}

static void c0__next_el(const uint8_t *buf, size_t pos, size_t stop, c0__el *el, c0_error *err) {
    el->dead = 0;
    el->depth = 0;
    for (;;) {
        uint8_t b;
        pos = c0__layout(buf, pos, stop);
        el->at = pos;
        el->kind = C0__EL_END;
        el->next = pos;
        if (err->code || pos >= stop) return;
        b = buf[pos];
        if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 >= stop) {
                c0__fail(err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
                return;
            }
            if (buf[p2] >= 0x20 || buf[p2] == C0_STX) {
                pos = c0__comment(buf, pos, stop, err); /* a note */
                continue;
            }
            el->dead = 1;
            pos = p2;
            b = buf[pos];
            if (b == C0_DLE) {
                c0__fail(err, C0_ERR_SYNTAX, pos, "a nested level is not an item");
                return;
            }
        }
        if (b == C0_RS) {
            c0__el_record(buf, pos + 1, stop, el, C0__EL_RECORD, err);
            return;
        }
        if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_RS) {
                c0__el_record(buf, p2 + 1, stop, el, C0__EL_HEADER, err);
                return;
            }
            c0__fail(err, C0_ERR_SYNTAX, pos, "keyed unit outside a record");
            return;
        }
        if (b == C0_GS) {
            while (pos < stop && (buf[pos] == C0_GS || c0_is_layout(buf[pos]))) {
                if (buf[pos] == C0_GS) el->depth++;
                pos++;
            }
            el->kind = C0__EL_GROUP;
            el->a = pos;
            el->b = el->next = c0__plain(buf, pos, stop);
            return;
        }
        if (b == C0_FS) {
            el->kind = C0__EL_FILE;
            el->a = c0__layout(buf, pos + 1, stop);
            el->b = el->next = c0__plain(buf, el->a, stop);
            return;
        }
        if (el->dead) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "BEL before a code that is not an element");
            return;
        }
        if (b == C0_ETB) {
            pos = c0__plain(buf, c0__layout(buf, pos + 1, stop), stop);
            continue;
        }
        if (b == C0_EOT || b == C0_SYN || b == C0_EM) return; /* the body ends here */
        if (b == C0_CAN) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "CAN with no open level");
        } else if (b < 0x20 && !c0__assigned[b]) {
            c0__fail(err, C0_ERR_UNASSIGNED, pos, "unassigned control code");
        } else {
            c0__fail(err, C0_ERR_SYNTAX, pos, "unexpected byte at element level");
        }
        return;
    }
}

/* ---- bodies: files ------------------------------------------------------ */

c0_bytes c0_file_name(c0_file f) { return c0_trim(c0__slice(f.buf, f.name_start, f.name_end)); }
c0_bytes c0_group_name(c0_group g) { return c0_trim(c0__slice(g.buf, g.name_start, g.name_end)); }

c0_file_iter c0_body_files(c0_body b) {
    c0_file_iter it;
    it.buf = b.buf;
    it.pos = b.start;
    it.stop = b.stop;
    it.first = 1;
    it.done = 0;
    c0__err_clear(&it.err);
    return it;
}

/* Scan from pos to the next live FS (or the end); *has says whether a live
 * record, header row or group line was passed. */
static void c0__part_end(const uint8_t *buf, size_t pos, size_t stop, c0__el *el, int *has, c0_error *err) {
    *has = 0;
    for (;;) {
        c0__next_el(buf, pos, stop, el, err);
        if (el->kind == C0__EL_END) return;
        if (el->kind == C0__EL_FILE && !el->dead) return;
        if (!el->dead) *has = 1;
        pos = el->next;
    }
}

int c0_next_file(c0_file_iter *it, c0_file *out) {
    c0__el el;
    if (it->done) return 0;
    out->buf = it->buf;
    if (it->first) {
        int has;
        it->first = 0;
        c0__part_end(it->buf, it->pos, it->stop, &el, &has, &it->err);
        if (has || el.kind == C0__EL_END) {
            out->name_start = out->name_end = it->pos;
            out->start = it->pos;
            out->stop = el.at;
            it->pos = el.at;
            if (el.kind == C0__EL_END) it->done = 1;
            return 1;
        }
        it->pos = el.at;
    }
    c0__next_el(it->buf, it->pos, it->stop, &el, &it->err);
    if (el.kind != C0__EL_FILE) {
        it->done = 1;
        return 0;
    }
    out->name_start = el.a;
    out->name_end = el.b;
    out->start = el.next;
    {
        int has;
        c0__part_end(it->buf, el.next, it->stop, &el, &has, &it->err);
        out->stop = el.at;
        it->pos = el.at;
        if (el.kind == C0__EL_END) it->done = 1;
    }
    return 1;
}

/* ---- bodies: groups ---------------------------------------------------- */

static c0_group_iter c0__groups(const uint8_t *buf, size_t start, size_t stop) {
    c0_group_iter it;
    it.buf = buf;
    it.pos = start;
    it.stop = stop;
    it.first = 1;
    it.done = 0;
    c0__err_clear(&it.err);
    return it;
}

c0_group_iter c0_file_groups(c0_file f) { return c0__groups(f.buf, f.start, f.stop); }
c0_group_iter c0_body_groups(c0_body b) { return c0__groups(b.buf, b.start, b.stop); }

/* Scan group contents from pos: stop at the next live GS or FS line or the
 * end; *has says whether a live header row or record was passed. */
static void c0__group_end(const uint8_t *buf, size_t pos, size_t stop, c0__el *el, int *has, c0_error *err) {
    *has = 0;
    for (;;) {
        c0__next_el(buf, pos, stop, el, err);
        if (el->kind == C0__EL_END) return;
        if ((el->kind == C0__EL_GROUP || el->kind == C0__EL_FILE) && !el->dead) return;
        if (!el->dead && (el->kind == C0__EL_RECORD || el->kind == C0__EL_HEADER)) *has = 1;
        pos = el->next;
    }
}

int c0_next_group(c0_group_iter *it, c0_group *out) {
    c0__el el;
    int has;
    out->buf = it->buf;
    for (;;) {
        if (it->done) return 0;
        if (it->first) {
            it->first = 0;
            c0__group_end(it->buf, it->pos, it->stop, &el, &has, &it->err);
            if (has) {
                out->depth = 0;
                out->name_start = out->name_end = it->pos;
                out->start = it->pos;
                out->stop = el.at;
                it->pos = el.at;
                if (el.kind == C0__EL_END) it->done = 1;
                return 1;
            }
            it->pos = el.at;
        }
        c0__next_el(it->buf, it->pos, it->stop, &el, &it->err);
        if (el.kind == C0__EL_END) {
            it->done = 1;
            return 0;
        }
        if (el.kind == C0__EL_FILE) { /* body-wide iteration: a new part begins */
            it->first = 1;
            it->pos = el.next;
            continue;
        }
        /* a live group line (dead ones are passed by c0__group_end) */
        out->depth = el.depth;
        out->name_start = el.a;
        out->name_end = el.b;
        out->start = el.next;
        c0__group_end(it->buf, el.next, it->stop, &el, &has, &it->err);
        out->stop = el.at;
        it->pos = el.at;
        if (el.kind == C0__EL_END) it->done = 1;
        return 1;
    }
}

c0_status c0_body_find_group(c0_body b, const uint8_t *name, size_t nlen, c0_group *out) {
    c0_group_iter it = c0_body_groups(b);
    while (c0_next_group(&it, out)) {
        c0_bytes n = c0_group_name(*out);
        if (n.len == nlen && (nlen == 0 || memcmp(n.ptr, name, nlen) == 0)) return C0_OK;
    }
    return it.err.code ? it.err.code : C0_ERR_NOT_FOUND;
}

c0_status c0_body_find_group_str(c0_body b, const char *name, c0_group *out) {
    return c0_body_find_group(b, (const uint8_t *)name, strlen(name), out);
}

int c0_body_bare(c0_body b) {
    c0_file_iter fi = c0_body_files(b);
    c0_file f;
    c0_group_iter gi;
    c0_group g;
    size_t n = 0;
    if (!c0_next_file(&fi, &f)) return 0;
    if (c0_file_name(f).len != 0) return 0;
    if (c0_next_file(&fi, &f)) return 0;
    gi = c0_file_groups(f);
    while (c0_next_group(&gi, &g)) {
        n++;
        if (n > 1 || g.depth != 0) return 0;
    }
    return 1;
}

int c0_group_header(c0_group g, c0_record *out) {
    c0_error err = {C0_OK, 0, ""};
    size_t pos = g.start;
    c0__el el;
    for (;;) {
        c0__next_el(g.buf, pos, g.stop, &el, &err);
        if (el.kind == C0__EL_END || err.code) return 0;
        if (el.kind == C0__EL_HEADER && !el.dead) {
            out->buf = g.buf;
            out->start = el.a;
            out->stop = el.b;
            return 1;
        }
        pos = el.next;
    }
}

c0_record_iter c0_group_records(c0_group g) {
    c0_record_iter it;
    it.buf = g.buf;
    it.pos = g.start;
    it.stop = g.stop;
    it.done = 0;
    c0__err_clear(&it.err);
    return it;
}

c0_record_iter c0_body_records(c0_body b) {
    c0_file_iter fi = c0_body_files(b);
    c0_file f;
    c0_group_iter gi;
    c0_group g;
    c0_record_iter it;
    if (c0_next_file(&fi, &f)) {
        gi = c0_file_groups(f);
        if (c0_next_group(&gi, &g)) return c0_group_records(g);
    }
    it.buf = b.buf;
    it.pos = it.stop = b.start;
    it.done = 1;
    c0__err_clear(&it.err);
    return it;
}

int c0_next_record(c0_record_iter *it, c0_record *out) {
    c0__el el;
    if (it->done) return 0;
    for (;;) {
        c0__next_el(it->buf, it->pos, it->stop, &el, &it->err);
        if (el.kind == C0__EL_END) {
            it->done = 1;
            return 0;
        }
        it->pos = el.next;
        if (el.kind == C0__EL_RECORD && !el.dead) {
            out->buf = it->buf;
            out->start = el.a;
            out->stop = el.b;
            return 1;
        }
    }
}

c0_status c0_group_column(c0_group g, const uint8_t *name, size_t nlen, size_t *index) {
    c0_record h;
    c0_unit_iter it;
    c0_bytes raw, v;
    size_t i = 0;
    if (!c0_group_header(g, &h)) return C0_ERR_NOT_FOUND;
    it = c0_record_units(h);
    while (c0_next_unit(&it, &raw)) {
        if (c0_value(raw, NULL, &v) == C0_OK && v.len == nlen && (nlen == 0 || memcmp(v.ptr, name, nlen) == 0)) {
            *index = i;
            return C0_OK;
        }
        i++;
    }
    return C0_ERR_NOT_FOUND;
}

c0_status c0_unit_list(c0_bytes raw, c0_unit_iter *items) {
    c0_body body;
    c0_status st = c0_unit_level(raw, &body);
    c0_file_iter fi;
    c0_file f;
    c0_group_iter gi;
    c0_group g;
    c0_record_iter ri;
    c0_record r;
    if (st) return st;
    items->buf = body.buf;
    items->pos = items->stop = body.start;
    items->done = 1;
    c0__err_clear(&items->err);
    fi = c0_body_files(body);
    if (!c0_next_file(&fi, &f)) return C0_OK; /* an empty level: no items */
    if (c0_file_name(f).len != 0 || c0_next_file(&fi, &f)) return C0_ERR_NOT_LIST;
    gi = c0_file_groups(f);
    if (!c0_next_group(&gi, &g)) return C0_OK;
    if (g.depth != 0 || c0_next_group(&gi, &g)) return C0_ERR_NOT_LIST;
    if (c0_group_header(g, &r)) return C0_ERR_NOT_LIST;
    ri = c0_group_records(g);
    if (!c0_next_record(&ri, &r)) return ri.err.code ? ri.err.code : C0_OK;
    if (c0_record_keyed(r)) return C0_ERR_NOT_LIST;
    *items = c0_record_units(r);
    if (c0_next_record(&ri, &r)) return C0_ERR_NOT_LIST;
    return C0_OK;
}

/* ---- documents --------------------------------------------------------- */

/* Scan from the start of a body to its end: EOT, SYN, EM or the end of
 * input at depth zero, skipping text and levels. */
static size_t c0__body_end(const uint8_t *buf, size_t pos, size_t len, c0_error *err) {
    while (pos < len) {
        uint8_t b = buf[pos];
        if (b >= 0x20) {
            pos++;
        } else if (b == C0_STX) {
            pos = c0__text(buf, pos, len, err);
            if (err->code) return len;
        } else if (b == C0_DLE) {
            pos = c0__level(buf, pos, len, err);
            if (err->code) return len;
        } else if (b == C0_EOT || b == C0_SYN || b == C0_EM) {
            return pos;
        } else if (b == C0_CAN) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "CAN with no open level");
            return len;
        } else {
            pos++;
        }
    }
    return pos;
}

/* After SYN: skip the heading's keyed units and notes. Returns the position
 * of the first body code. */
static size_t c0__heading_end(const uint8_t *buf, size_t pos, size_t len, c0_error *err) {
    for (;;) {
        uint8_t b;
        pos = c0__layout(buf, pos, len);
        if (pos >= len) return pos;
        b = buf[pos];
        if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, len), vs;
            if (p2 < len && buf[p2] == C0_RS) return pos; /* a header row ends the heading */
            pos = c0__plain(buf, p2, len);
            if (!(pos < len && buf[pos] == C0_US)) {
                c0__fail(err, C0_ERR_SYNTAX, pos, "key without a value (no US)");
                return len;
            }
            vs = c0__layout(buf, pos + 1, len);
            if (vs < len && buf[vs] == C0_ENQ) {
                c0__fail(err, C0_ERR_SYNTAX, vs, "reference in a heading");
                return len;
            }
            pos = c0__unit(buf, pos + 1, len, err);
            if (err->code) return len;
        } else if (b == C0_BEL) {
            pos = c0__comment(buf, pos, len, err);
            if (err->code) return len;
        } else if (b >= 0x20) {
            c0__fail(err, C0_ERR_SYNTAX, pos, "plain text directly after SYN");
            return len;
        } else {
            return pos;
        }
    }
}

static int c0__eq_str(c0_bytes b, const char *s) {
    size_t n = strlen(s);
    return b.len == n && (n == 0 || memcmp(b.ptr, s, n) == 0);
}

c0_status c0_document_read(const uint8_t *buf, size_t len, size_t start, c0_document *out, c0_error *err) {
    c0_error local = {C0_OK, 0, ""};
    size_t pos;
    if (!err) err = &local;
    c0__err_clear(err);
    memset(out, 0, sizeof(*out));
    out->buf = buf;
    out->start = start;
    pos = c0__layout(buf, start, len);
    out->has_syn = pos < len && buf[pos] == C0_SYN;
    if (out->has_syn) {
        pos++;
        out->heading_start = pos;
        pos = c0__heading_end(buf, pos, len, err);
        if (err->code) return err->code;
        out->heading_end = pos;
    } else {
        out->warnings |= C0_WARN_NO_SYN;
        out->heading_start = out->heading_end = pos;
    }
    out->body.buf = buf;
    out->body.start = pos;
    out->body.stop = c0__body_end(buf, pos, len, err);
    if (err->code) return err->code;
    pos = c0__layout(buf, out->body.stop, len);
    out->has_eot = pos < len && buf[pos] == C0_EOT;
    if (out->has_eot) {
        pos++;
    } else if (!c0__eq_str(c0_heading_shape(out), "stream")) {
        out->warnings |= C0_WARN_NO_EOT;
    }
    out->stop = pos;
    {
        c0_pair_iter it = c0_document_heading(out);
        c0_bytes k, v;
        while (c0_next_pair(&it, &k, &v)) {
            c0_bytes other;
            if (!c0__eq_str(k, "shape") && !c0__eq_str(k, "version") && !c0__eq_str(k, "blob")) {
                out->warnings |= C0_WARN_UNKNOWN_KEY;
            }
            {
                c0_pair_iter rest = it;
                c0_bytes k2;
                while (c0_next_pair(&rest, &k2, &other)) {
                    if (k2.len == k.len && memcmp(k2.ptr, k.ptr, k.len) == 0) out->warnings |= C0_WARN_DUPLICATE_KEY;
                }
            }
        }
        if (it.err.code) {
            *err = it.err;
            return err->code;
        }
    }
    return C0_OK;
}

c0_pair_iter c0_document_heading(const c0_document *d) {
    return c0__pairs(d->buf, d->heading_start, d->heading_end);
}

c0_status c0_heading_get(const c0_document *d, const uint8_t *key, size_t klen, c0_bytes *raw) {
    return c0__pairs_get(c0_document_heading(d), key, klen, raw);
}

c0_status c0_heading_get_str(const c0_document *d, const char *key, c0_bytes *raw) {
    return c0_heading_get(d, (const uint8_t *)key, strlen(key), raw);
}

c0_bytes c0_heading_shape(const c0_document *d) {
    c0_bytes raw, v;
    v.ptr = d->buf;
    v.len = 0;
    if (c0_heading_get_str(d, "shape", &raw) == C0_OK && c0_value(raw, NULL, &v) != C0_OK) v.len = 0;
    return v;
}

int c0_heading_version(const c0_document *d) {
    c0_bytes raw, v;
    int n = 0;
    size_t i;
    if (c0_heading_get_str(d, "version", &raw) != C0_OK) return 1;
    if (c0_value(raw, NULL, &v) != C0_OK || v.len == 0) return 1;
    for (i = 0; i < v.len; i++) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') return 1;
        n = n * 10 + (v.ptr[i] - '0');
    }
    return n;
}

c0_status c0_heading_blob(const c0_document *d, c0_body *out) {
    c0_bytes raw;
    c0_status st = c0_heading_get_str(d, "blob", &raw);
    if (st) return st;
    return c0_unit_level(raw, out);
}

/* ---- containers -------------------------------------------------------- */

c0_doc_iter c0_documents(const uint8_t *buf, size_t len) {
    c0_doc_iter it;
    it.buf = buf;
    it.len = len;
    it.pos = 0;
    it.has_em = 0;
    it.em_pos = len;
    it.done = 0;
    c0__err_clear(&it.err);
    return it;
}

int c0_next_document(c0_doc_iter *it, c0_document *out) {
    size_t pos;
    if (it->done) return 0;
    pos = c0__layout(it->buf, it->pos, it->len);
    if (pos >= it->len) {
        it->done = 1;
        return 0;
    }
    if (it->buf[pos] == C0_EM) {
        it->has_em = 1;
        it->em_pos = pos;
        it->done = 1;
        return 0;
    }
    if (it->pos > 0 && it->buf[pos] != C0_SYN) {
        c0__fail(&it->err, C0_ERR_SYNTAX, pos, "a document after the first must begin with SYN");
        it->done = 1;
        return 0;
    }
    if (c0_document_read(it->buf, it->len, pos, out, &it->err) != C0_OK) {
        it->done = 1;
        return 0;
    }
    it->pos = c0__layout(it->buf, out->stop, it->len);
    if (it->pos < it->len && it->buf[it->pos] != C0_SYN && it->buf[it->pos] != C0_EM) {
        c0__fail(&it->err, C0_ERR_SYNTAX, it->pos, "unexpected byte after EOT");
        it->done = 1;
        return 0;
    }
    return 1;
}

c0_bytes c0_doc_iter_tail(const c0_doc_iter *it) {
    c0_bytes t;
    t.ptr = it->buf + it->len;
    t.len = 0;
    if (it->has_em) {
        t.ptr = it->buf + it->em_pos + 1;
        t.len = it->len - it->em_pos - 1;
    }
    return t;
}

c0_status c0_container_tail(const uint8_t *buf, size_t len, c0_bytes *tail, int *has_em, c0_error *err) {
    c0_doc_iter it = c0_documents(buf, len);
    c0_document d;
    while (c0_next_document(&it, &d)) {
    }
    if (err) *err = it.err;
    *tail = c0_doc_iter_tail(&it);
    if (has_em) *has_em = it.has_em;
    return it.err.code;
}

static int c0__to_size(c0_bytes v, size_t *out) {
    size_t i, n = 0;
    if (v.len == 0) return 0;
    for (i = 0; i < v.len; i++) {
        if (v.ptr[i] < '0' || v.ptr[i] > '9') return 0;
        n = n * 10 + (size_t)(v.ptr[i] - '0');
    }
    *out = n;
    return 1;
}

c0_status c0_blob(const uint8_t *buf, size_t len, const uint8_t *name, size_t nlen, c0_bytes *out, c0_error *err) {
    c0_doc_iter it = c0_documents(buf, len);
    c0_document first;
    c0_body table;
    c0_group_iter gi;
    c0_group g;
    c0_record_iter ri;
    c0_record r;
    c0_bytes tail;
    size_t ni = 0, oi = 1, li = 2;
    int found = 0;
    c0_status st;
    if (!c0_next_document(&it, &first)) {
        if (err) *err = it.err;
        return it.err.code ? it.err.code : C0_ERR_NOT_FOUND;
    }
    {
        c0_document d;
        while (c0_next_document(&it, &d)) {
        }
    }
    if (err) *err = it.err;
    if (it.err.code) return it.err.code;
    if (!it.has_em) return C0_ERR_NOT_FOUND;
    tail = c0_doc_iter_tail(&it);
    st = c0_heading_blob(&first, &table);
    if (st) return st;
    gi = c0_body_groups(table);
    if (!c0_next_group(&gi, &g)) return C0_ERR_NOT_FOUND;
    c0_group_column(g, (const uint8_t *)"name", 4, &ni);
    c0_group_column(g, (const uint8_t *)"offset", 6, &oi);
    c0_group_column(g, (const uint8_t *)"length", 6, &li);
    ri = c0_group_records(g);
    while (c0_next_record(&ri, &r)) {
        c0_bytes raw, v;
        size_t off, length;
        if (c0_record_field(r, ni, &raw) || c0_value(raw, NULL, &v)) continue;
        if (!(v.len == nlen && (nlen == 0 || memcmp(v.ptr, name, nlen) == 0))) continue;
        if (c0_record_field(r, oi, &raw) || c0_value(raw, NULL, &v) || !c0__to_size(v, &off)) continue;
        if (c0_record_field(r, li, &raw) || c0_value(raw, NULL, &v) || !c0__to_size(v, &length)) continue;
        if (off > tail.len || length > tail.len - off) continue;
        out->ptr = tail.ptr + off;
        out->len = length;
        found = 1; /* last wins */
    }
    return found ? C0_OK : C0_ERR_NOT_FOUND;
}

c0_status c0_blob_str(const uint8_t *buf, size_t len, const char *name, c0_bytes *out, c0_error *err) {
    return c0_blob(buf, len, (const uint8_t *)name, strlen(name), out, err);
}

/* ---- builder ----------------------------------------------------------- */

enum { C0__B_TOP = 0, C0__B_HEADING = 1, C0__B_RECORD = 2 };

static int c0__b_reserve(c0_builder *b, size_t extra) {
    if (b->status != C0_OK) return 0;
    if (b->len + extra > b->cap) {
        size_t ncap = b->cap ? b->cap : 64;
        uint8_t *nd;
        while (ncap < b->len + extra) ncap *= 2;
        nd = (uint8_t *)realloc(b->data, ncap);
        if (!nd) {
            b->status = C0_ERR_OOM;
            return 0;
        }
        b->data = nd;
        b->cap = ncap;
    }
    return 1;
}

static void c0__b_byte(c0_builder *b, uint8_t x) {
    if (c0__b_reserve(b, 1)) b->data[b->len++] = x;
}

static void c0__b_raw(c0_builder *b, const uint8_t *p, size_t n) {
    if (n && c0__b_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

static void c0__b_name(c0_builder *b, const uint8_t *p, size_t n) {
    if (!c0_is_name(p, n)) {
        if (b->status == C0_OK) b->status = C0_ERR_BAD_NAME;
        return;
    }
    c0__b_raw(b, p, n);
}

/* A value in its one spelling. */
static void c0__b_value(c0_builder *b, const uint8_t *p, size_t n) {
    size_t i;
    if (c0_is_plain(p, n)) {
        c0__b_raw(b, p, n);
        return;
    }
    c0__b_byte(b, C0_STX);
    for (i = 0; i < n; i++) {
        if (c0_is_text_escaped(p[i])) c0__b_byte(b, C0_DLE);
        c0__b_byte(b, p[i]);
    }
    c0__b_byte(b, C0_ETX);
}

static void c0__b_end_record(c0_builder *b) {
    b->state = C0__B_TOP;
    b->units = 0;
}

/* The first value of a record, straight after RS. */
static void c0__b_first_value(c0_builder *b, const uint8_t *p, size_t n) {
    c0__b_value(b, p, n);
    b->units = 1;
}

/* Position for the next unit of the open record (opening one if needed):
 * US between units, nothing before the first. */
static void c0__b_next_unit(c0_builder *b) {
    if (b->state != C0__B_RECORD) c0_builder_rs(b);
    else if (b->units > 0) c0__b_byte(b, C0_US);
    b->units++;
}

/* SOH key US, opening a record first unless one (or the heading) is open. */
static void c0__b_key_head(c0_builder *b, const uint8_t *key, size_t klen) {
    if (b->state == C0__B_TOP) c0_builder_rs(b);
    c0_builder_soh_name(b, key, klen);
}

static c0_bytes c0__cstr(const char *s) {
    c0_bytes b;
    b.ptr = (const uint8_t *)s;
    b.len = strlen(s);
    return b;
}

/* Build a c0_bytes array from C strings on the heap; NULL on OOM. */
static c0_bytes *c0__cstrs(c0_builder *b, const char *const *strs, size_t count) {
    c0_bytes *out = (c0_bytes *)malloc((count ? count : 1) * sizeof(c0_bytes));
    size_t i;
    if (!out) {
        if (b->status == C0_OK) b->status = C0_ERR_OOM;
        return NULL;
    }
    for (i = 0; i < count; i++) out[i] = c0__cstr(strs[i]);
    return out;
}

void c0_builder_init(c0_builder *b) {
    memset(b, 0, sizeof(*b));
    b->status = C0_OK;
}

void c0_builder_free(c0_builder *b) {
    free(b->data);
    free(b->stack);
    memset(b, 0, sizeof(*b));
}

c0_status c0_builder_status(const c0_builder *b) { return b->status; }

c0_bytes c0_builder_bytes(const c0_builder *b) {
    c0_bytes r;
    r.ptr = b->data;
    r.len = b->len;
    return r;
}

/* ---- primitives: one function per code ---- */

void c0_builder_fs(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_FS);
    c0__b_name(b, name, nlen);
}

void c0_builder_gs(c0_builder *b) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_GS);
}

void c0_builder_gs_name(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0_builder_gs(b);
    c0__b_name(b, name, nlen);
}

void c0_builder_rs(c0_builder *b) {
    c0__b_byte(b, C0_RS);
    b->state = C0__B_RECORD;
    b->units = 0;
}

void c0_builder_rs_value(c0_builder *b, const uint8_t *value, size_t vlen) {
    c0_builder_rs(b);
    c0__b_first_value(b, value, vlen);
}

void c0_builder_us(c0_builder *b) {
    c0__b_byte(b, C0_US);
    b->units++;
}

void c0_builder_us_value(c0_builder *b, const uint8_t *value, size_t vlen) {
    c0__b_byte(b, C0_US);
    c0__b_value(b, value, vlen);
    b->units++;
}

void c0_builder_soh(c0_builder *b) { c0__b_byte(b, C0_SOH); }

void c0_builder_soh_name(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__b_byte(b, C0_SOH);
    c0__b_name(b, name, nlen);
}

void c0_builder_dle(c0_builder *b) {
    if (b->depth + 2 > b->stack_cap) {
        size_t ncap = b->stack_cap ? b->stack_cap * 2 : 16;
        int *ns = (int *)realloc(b->stack, ncap * sizeof(int) * 2);
        if (!ns) {
            if (b->status == C0_OK) b->status = C0_ERR_OOM;
            return;
        }
        b->stack = ns;
        b->stack_cap = ncap;
    }
    b->stack[2 * b->depth] = b->state;
    b->stack[2 * b->depth + 1] = (int)b->units;
    b->depth++;
    b->state = C0__B_TOP;
    b->units = 0;
    c0__b_byte(b, C0_DLE);
}

void c0_builder_can(c0_builder *b) {
    if (b->depth == 0) {
        if (b->status == C0_OK) b->status = C0_ERR_BAD_STATE;
        return;
    }
    c0__b_byte(b, C0_CAN);
    b->depth--;
    b->state = b->stack[2 * b->depth];
    b->units = (size_t)b->stack[2 * b->depth + 1];
}

void c0_builder_bel(c0_builder *b) { c0__b_byte(b, C0_BEL); }

void c0_builder_bel_text(c0_builder *b, const uint8_t *text, size_t tlen) {
    if (tlen == 0) { /* a note needs content: bare BEL marks the next element dead */
        if (b->status == C0_OK) b->status = C0_ERR_BAD_STATE;
        return;
    }
    c0__b_byte(b, C0_BEL);
    c0__b_value(b, text, tlen);
}

void c0_builder_enq(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__b_byte(b, C0_ENQ);
    if (nlen == 0 && b->status == C0_OK) b->status = C0_ERR_BAD_NAME;
    c0__b_name(b, name, nlen);
}

void c0_builder_enq_path(c0_builder *b, const c0_bytes *segments, size_t count) {
    size_t i;
    if (count == 1) {
        c0_builder_enq(b, segments[0].ptr, segments[0].len);
        return;
    }
    c0__b_byte(b, C0_ENQ);
    if ((count < 2 || count > 3) && b->status == C0_OK) b->status = C0_ERR_BAD_NAME;
    c0__b_byte(b, C0_STX);
    for (i = 0; i < count && i < 3; i++) {
        if (i) c0__b_byte(b, C0_US);
        if (segments[i].len == 0 && b->status == C0_OK) b->status = C0_ERR_BAD_NAME;
        c0__b_name(b, segments[i].ptr, segments[i].len);
    }
    c0__b_byte(b, C0_ETX);
}

void c0_builder_syn(c0_builder *b) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_SYN);
    b->state = C0__B_HEADING;
}

void c0_builder_eot(c0_builder *b) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_EOT);
}

void c0_builder_etb(c0_builder *b) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_ETB);
}

void c0_builder_etb_payload(c0_builder *b, const uint8_t *payload, size_t plen) {
    c0_builder_etb(b);
    c0__b_name(b, payload, plen);
}

void c0_builder_em(c0_builder *b, const uint8_t *tail, size_t tlen) {
    c0__b_end_record(b);
    c0__b_byte(b, C0_EM);
    c0__b_raw(b, tail, tlen);
}

void c0_builder_fs_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_builder_fs(b, n.ptr, n.len); }
void c0_builder_gs_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_builder_gs_name(b, n.ptr, n.len); }
void c0_builder_rs_str(c0_builder *b, const char *value) { c0_bytes v = c0__cstr(value); c0_builder_rs_value(b, v.ptr, v.len); }
void c0_builder_us_str(c0_builder *b, const char *value) { c0_bytes v = c0__cstr(value); c0_builder_us_value(b, v.ptr, v.len); }
void c0_builder_soh_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_builder_soh_name(b, n.ptr, n.len); }
void c0_builder_bel_str(c0_builder *b, const char *text) { c0_bytes t = c0__cstr(text); c0_builder_bel_text(b, t.ptr, t.len); }
void c0_builder_enq_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_builder_enq(b, n.ptr, n.len); }
void c0_builder_enq_path_str(c0_builder *b, const char *const *segments, size_t count) {
    c0_bytes *v = c0__cstrs(b, segments, count);
    if (!v) return;
    c0_builder_enq_path(b, v, count);
    free(v);
}
void c0_builder_etb_str(c0_builder *b, const char *payload) { c0_bytes p = c0__cstr(payload); c0_builder_etb_payload(b, p.ptr, p.len); }
void c0_builder_em_str(c0_builder *b, const char *tail) { c0_bytes t = c0__cstr(tail); c0_builder_em(b, t.ptr, t.len); }

/* ---- composed: wrappers over the primitives ---- */

void c0_build_syn(c0_builder *b) { c0_builder_syn(b); }

void c0_build_eot(c0_builder *b) { c0_builder_eot(b); }

void c0_build_etb(c0_builder *b, const uint8_t *payload, size_t plen) {
    if (payload && plen) c0_builder_etb_payload(b, payload, plen);
    else c0_builder_etb(b);
}

void c0_build_em(c0_builder *b, const uint8_t *tail, size_t tlen) { c0_builder_em(b, tail, tlen); }

void c0_build_file(c0_builder *b, const uint8_t *name, size_t nlen) { c0_builder_fs(b, name, nlen); }

void c0_build_section(c0_builder *b, const uint8_t *name, size_t nlen, size_t depth) {
    size_t i;
    for (i = 1; i < depth; i++) c0_builder_gs(b);
    c0_builder_gs_name(b, name, nlen);
}

void c0_build_group(c0_builder *b, const uint8_t *name, size_t nlen) { c0_builder_gs_name(b, name, nlen); }

void c0_build_header(c0_builder *b, const c0_bytes *names, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) { /* validate before writing anything */
        if (!c0_is_name(names[i].ptr, names[i].len)) {
            if (b->status == C0_OK) b->status = C0_ERR_BAD_NAME;
            return;
        }
    }
    c0__b_end_record(b);
    c0_builder_soh(b);
    c0_builder_rs(b);
    for (i = 0; i < count; i++) {
        if (i) c0_builder_us(b);
        c0__b_raw(b, names[i].ptr, names[i].len);
    }
    c0__b_end_record(b);
}

void c0_build_record(c0_builder *b, const c0_bytes *fields, size_t count) {
    size_t i;
    c0_builder_rs(b);
    for (i = 0; i < count; i++) c0_build_field(b, fields[i].ptr, fields[i].len);
}

void c0_build_field(c0_builder *b, const uint8_t *value, size_t vlen) {
    if (b->state != C0__B_RECORD) c0_builder_rs_value(b, value, vlen);
    else if (b->units == 0) c0__b_first_value(b, value, vlen);
    else c0_builder_us_value(b, value, vlen);
}

void c0_build_map(c0_builder *b, const c0_bytes *keys, const c0_bytes *values, size_t count) {
    size_t i;
    c0_builder_rs(b);
    for (i = 0; i < count; i++) c0_build_key(b, keys[i].ptr, keys[i].len, values[i].ptr, values[i].len);
}

void c0_build_key(c0_builder *b, const uint8_t *key, size_t klen, const uint8_t *value, size_t vlen) {
    c0__b_key_head(b, key, klen);
    c0_builder_us_value(b, value, vlen);
}

void c0_build_level_open(c0_builder *b) {
    c0__b_next_unit(b);
    c0_builder_dle(b);
}

void c0_build_key_level_open(c0_builder *b, const uint8_t *key, size_t klen) {
    c0__b_key_head(b, key, klen);
    c0_builder_us(b);
    c0_builder_dle(b);
}

void c0_build_level_close(c0_builder *b) { c0_builder_can(b); }

void c0_build_list(c0_builder *b, const c0_bytes *items, size_t count) {
    size_t i;
    c0__b_next_unit(b);
    c0_builder_dle(b);
    if (count) {
        c0_builder_rs(b);
        for (i = 0; i < count; i++) c0_build_field(b, items[i].ptr, items[i].len);
    }
    c0_builder_can(b);
}

void c0_build_ref(c0_builder *b, const uint8_t *name, size_t nlen) {
    c0__b_next_unit(b);
    c0_builder_enq(b, name, nlen);
}

void c0_build_ref_path(c0_builder *b, const c0_bytes *segments, size_t count) {
    c0__b_next_unit(b);
    c0_builder_enq_path(b, segments, count);
}

void c0_build_note(c0_builder *b, const uint8_t *text, size_t tlen) { c0_builder_bel_text(b, text, tlen); }

void c0_build_block(c0_builder *b, const uint8_t *text, size_t tlen) {
    c0_bytes f;
    f.ptr = text;
    f.len = tlen;
    c0_build_record(b, &f, 1);
}

void c0_build_item(c0_builder *b, const uint8_t *text, size_t tlen) { c0_build_field(b, text, tlen); }

void c0_build_etb_str(c0_builder *b, const char *payload) {
    c0_bytes p = payload ? c0__cstr(payload) : c0__cstr("");
    c0_build_etb(b, p.ptr, p.len);
}
void c0_build_file_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_build_file(b, n.ptr, n.len); }
void c0_build_group_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_build_group(b, n.ptr, n.len); }
void c0_build_section_str(c0_builder *b, const char *name, size_t depth) { c0_bytes n = c0__cstr(name); c0_build_section(b, n.ptr, n.len, depth); }
void c0_build_header_str(c0_builder *b, const char *const *names, size_t count) {
    c0_bytes *v = c0__cstrs(b, names, count);
    if (!v) return;
    c0_build_header(b, v, count);
    free(v);
}
void c0_build_record_str(c0_builder *b, const char *const *fields, size_t count) {
    c0_bytes *v = c0__cstrs(b, fields, count);
    if (!v) return;
    c0_build_record(b, v, count);
    free(v);
}
void c0_build_field_str(c0_builder *b, const char *value) { c0_bytes v = c0__cstr(value); c0_build_field(b, v.ptr, v.len); }
void c0_build_map_str(c0_builder *b, const char *const *keys, const char *const *values, size_t count) {
    size_t i;
    c0_builder_rs(b);
    for (i = 0; i < count; i++) c0_build_key_str(b, keys[i], values[i]);
}
void c0_build_key_str(c0_builder *b, const char *key, const char *value) {
    c0_bytes k = c0__cstr(key), v = c0__cstr(value);
    c0_build_key(b, k.ptr, k.len, v.ptr, v.len);
}
void c0_build_key_level_open_str(c0_builder *b, const char *key) { c0_bytes k = c0__cstr(key); c0_build_key_level_open(b, k.ptr, k.len); }
void c0_build_list_str(c0_builder *b, const char *const *items, size_t count) {
    c0_bytes *v = c0__cstrs(b, items, count);
    if (!v) return;
    c0_build_list(b, v, count);
    free(v);
}
void c0_build_ref_str(c0_builder *b, const char *name) { c0_bytes n = c0__cstr(name); c0_build_ref(b, n.ptr, n.len); }
void c0_build_ref_path_str(c0_builder *b, const char *const *segments, size_t count) {
    c0_bytes *v = c0__cstrs(b, segments, count);
    if (!v) return;
    c0_build_ref_path(b, v, count);
    free(v);
}
void c0_build_note_str(c0_builder *b, const char *text) { c0_bytes t = c0__cstr(text); c0_build_note(b, t.ptr, t.len); }
void c0_build_block_str(c0_builder *b, const char *text) { c0_bytes t = c0__cstr(text); c0_build_block(b, t.ptr, t.len); }
void c0_build_item_str(c0_builder *b, const char *text) { c0_bytes t = c0__cstr(text); c0_build_item(b, t.ptr, t.len); }

/* ---- stream ------------------------------------------------------------ */

/* After an ETB at pos-1: skip its payload; the result is committed_end
 * (layout after the payload belongs to the next block). */
static size_t c0__payload_end(const uint8_t *buf, size_t pos, size_t len) {
    size_t p = c0__layout(buf, pos, len);
    if (p < len && buf[p] >= 0x20) {
        p = c0__plain(buf, p, len);
        while (p > pos && buf[p - 1] == C0_SP) p--;
        return p;
    }
    return pos;
}

static c0_block_iter c0__blocks(const uint8_t *buf, size_t stop) {
    c0_block_iter it;
    it.buf = buf;
    it.stop = stop;
    it.pos = 0;
    it.block_start = 0;
    it.in_text = 0;
    it.depth = 0;
    return it;
}

c0_block_iter c0_stream_blocks(const c0_stream *s) { return c0__blocks(s->buf, s->committed_end); }

int c0_next_block(c0_block_iter *it, c0_bytes *out, int *damaged) {
    const uint8_t *buf = it->buf;
    while (it->pos < it->stop) {
        uint8_t b = buf[it->pos];
        if (it->in_text) {
            if (b == C0_DLE) {
                it->pos += 2;
                continue;
            } else if (b == C0_ETX) {
                it->in_text = 0;
            } else if (b == C0_ETB) {
                *out = c0__slice(buf, it->block_start, it->pos);
                if (damaged) *damaged = 1;
                it->pos = c0__payload_end(buf, it->pos + 1, it->stop);
                it->block_start = it->pos;
                it->in_text = 0;
                it->depth = 0;
                return 1;
            } else if (b == C0_SYN || b == C0_EOT || b == C0_EM) {
                it->in_text = 0;
                it->depth = 0;
                if (b == C0_SYN) it->block_start = it->pos;
            }
            it->pos++;
        } else {
            if (b == C0_STX) {
                it->in_text = 1;
            } else if (b == C0_DLE) {
                it->depth++;
            } else if (b == C0_CAN) {
                if (it->depth > 0) it->depth--;
            } else if (b == C0_ETB) {
                *out = c0__slice(buf, it->block_start, it->pos);
                if (damaged) *damaged = it->depth > 0;
                it->pos = c0__payload_end(buf, it->pos + 1, it->stop);
                it->block_start = it->pos;
                it->depth = 0;
                return 1;
            } else if (b == C0_SYN && it->pos > 0) {
                it->block_start = it->pos;
                it->depth = 0;
            }
            it->pos++;
        }
    }
    return 0;
}

/* Is the tail after committed_end more than layout, notes, EOT or EM? */
static int c0__torn_tail(const uint8_t *buf, size_t pos, size_t len) {
    c0_error err = {C0_OK, 0, ""};
    for (;;) {
        uint8_t b;
        pos = c0__layout(buf, pos, len);
        if (pos >= len) return 0;
        b = buf[pos];
        if (b == C0_EOT) {
            pos++;
        } else if (b == C0_EM) {
            return 0; /* the raw region is not data */
        } else if (b == C0_BEL) {
            if (c0__bel_kills_element(buf, pos, len)) return 1; /* a dead element is data */
            pos = c0__comment(buf, pos, len, &err);
            if (err.code) return 1;
        } else {
            return 1;
        }
    }
}

c0_stream c0_stream_read(const uint8_t *buf, size_t len) {
    c0_stream s;
    c0_block_iter it = c0__blocks(buf, len);
    c0_bytes blk;
    int damaged;
    s.buf = buf;
    s.len = len;
    s.committed_end = 0;
    s.block_count = 0;
    s.damaged_count = 0;
    while (c0_next_block(&it, &blk, &damaged)) {
        s.block_count++;
        if (damaged) s.damaged_count++;
        s.committed_end = it.pos;
    }
    s.torn = c0__torn_tail(buf, s.committed_end, len);
    return s;
}

c0_bytes c0_stream_committed(const c0_stream *s) { return c0__slice(s->buf, 0, s->committed_end); }
c0_bytes c0_stream_tail(const c0_stream *s) { return c0__slice(s->buf, s->committed_end, s->len); }

size_t c0_stream_undamaged(const c0_stream *s, uint8_t *out) {
    c0_block_iter it = c0_stream_blocks(s);
    c0_bytes blk;
    int damaged;
    size_t n = 0;
    while (c0_next_block(&it, &blk, &damaged)) {
        if (damaged) continue;
        if (blk.len) memcpy(out + n, blk.ptr, blk.len);
        n += blk.len;
    }
    return n;
}

size_t c0_stream_last_commit_end(const uint8_t *buf, size_t len) {
    size_t i = len;
    while (i > 0) {
        i--;
        if (buf[i] == C0_ETB) {
            size_t dles = 0, j = i;
            while (j > 0 && buf[j - 1] == C0_DLE) {
                dles++;
                j--;
            }
            if (dles % 2 == 0) return c0__plain(buf, i + 1, len);
        }
    }
    return 0;
}

/* ---- validator (validator.cr) ------------------------------------------
 * Recursive descent over GRAMMAR.md. `c0_validate` reports the first error;
 * `c0_canonical` additionally applies the canonical constraints. On error
 * every function returns `stop` so loops end; callers test C0__V_ERR. */

#define C0__V_MAX_DEPTH 512

typedef struct {
    const uint8_t *buf;
    size_t len;
    int canonical;
    int ok;
    int diff;
    size_t depth;
    c0_error err;
} c0__val;

#define C0__V_ERR(v) ((v)->err.code != C0_OK)

static void c0__v_nc(c0__val *v) { v->ok = 0; }

static size_t c0__v_layout(c0__val *v, size_t pos, size_t stop) {
    size_t p2 = c0__layout(v->buf, pos, stop);
    if (p2 != pos) c0__v_nc(v);
    return p2;
}

static size_t c0__v_body(c0__val *v, size_t pos, size_t stop, int depth0);
static size_t c0__v_record(c0__val *v, size_t pos, size_t stop);
static size_t c0__v_value(c0__val *v, size_t pos, size_t stop, int heading);
static size_t c0__v_level(c0__val *v, size_t pos, size_t stop);

/* Keys in [start, stop) must be unique (any order). */
static void c0__v_check_keys(c0__val *v, size_t start, size_t stop) {
    c0_pair_iter a = c0__pairs(v->buf, start, stop);
    c0_bytes k, k2, raw;
    while (c0_next_pair(&a, &k, &raw)) {
        c0_pair_iter rest = a;
        while (c0_next_pair(&rest, &k2, &raw)) {
            if (k2.len == k.len && (k.len == 0 || memcmp(k.ptr, k2.ptr, k.len) == 0)) c0__v_nc(v);
        }
    }
}

static size_t c0__v_plain_name(c0__val *v, size_t pos, size_t stop) {
    size_t start;
    pos = c0__v_layout(v, pos, stop);
    if (pos < stop && v->buf[pos] == C0_STX) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "a name cannot be text");
        return stop;
    }
    start = pos;
    pos = c0__plain(v->buf, pos, stop);
    if (pos > start && c0_is_layout(v->buf[pos - 1])) c0__v_nc(v);
    return pos;
}

/* Notes (BEL + content) only; a BEL before a code is left for the caller. */
static size_t c0__v_notes(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    for (;;) {
        size_t p2;
        pos = c0__v_layout(v, pos, stop);
        if (!(pos < stop && buf[pos] == C0_BEL)) break;
        p2 = c0__layout(buf, pos + 1, stop);
        if (p2 >= stop) {
            c0__fail(&v->err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
            return stop;
        }
        if (buf[p2] < 0x20 && buf[p2] != C0_STX) break; /* a dead element, not a note */
        c0__v_nc(v);
        pos = buf[p2] == C0_STX ? c0__text(buf, p2, stop, &v->err) : c0__plain(buf, p2, stop);
        if (C0__V_ERR(v)) return stop;
    }
    return pos;
}

static size_t c0__v_trailing_notes(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    while (pos < stop && buf[pos] == C0_BEL) {
        size_t p2 = c0__layout(buf, pos + 1, stop);
        if (p2 >= stop) {
            c0__fail(&v->err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
            return stop;
        }
        if (!(buf[p2] >= 0x20 || buf[p2] == C0_STX)) break;
        c0__v_nc(v);
        pos = buf[p2] == C0_STX ? c0__text(buf, p2, stop, &v->err) : c0__plain(buf, p2, stop);
        if (C0__V_ERR(v)) return stop;
        pos = c0__v_layout(v, pos, stop);
    }
    return pos;
}

static size_t c0__v_header_names(c0__val *v, size_t pos, size_t stop) {
    pos = c0__v_layout(v, pos, stop);
    for (;;) {
        pos = c0__v_plain_name(v, pos, stop);
        if (C0__V_ERR(v)) return stop;
        if (!(pos < stop && v->buf[pos] == C0_US)) break;
        pos = c0__v_layout(v, pos + 1, stop);
    }
    return pos;
}

static size_t c0__v_header_row(c0__val *v, size_t pos, size_t stop) {
    size_t p2;
    pos = c0__v_layout(v, pos, stop);
    if (!(pos < stop && v->buf[pos] == C0_SOH)) return pos;
    p2 = c0__v_layout(v, pos + 1, stop);
    if (!(p2 < stop && v->buf[p2] == C0_RS)) return pos;
    return c0__v_header_names(v, p2 + 1, stop);
}

static size_t c0__v_commit(c0__val *v, size_t pos, size_t stop, int depth0) {
    if (!depth0) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "commit inside a nested level");
        return stop;
    }
    c0__v_nc(v);
    return c0__plain(v->buf, c0__v_layout(v, pos + 1, stop), stop);
}

/* Before a header row: notes, commits (depth zero), dead records, dead header rows. */
static size_t c0__v_pre(c0__val *v, size_t pos, size_t stop, int depth0) {
    const uint8_t *buf = v->buf;
    for (;;) {
        uint8_t b;
        pos = c0__v_layout(v, pos, stop);
        if (pos >= stop || C0__V_ERR(v)) break;
        b = buf[pos];
        if (b == C0_ETB) {
            pos = c0__v_commit(v, pos, stop, depth0);
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 >= stop) {
                c0__fail(&v->err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
                return stop;
            }
            if (buf[p2] >= 0x20 || buf[p2] == C0_STX) {
                pos = c0__v_notes(v, pos, stop);
            } else if (buf[p2] == C0_RS) {
                c0__v_nc(v);
                pos = c0__v_record(v, p2 + 1, stop);
            } else if (buf[p2] == C0_SOH) {
                size_t p3 = c0__layout(buf, p2 + 1, stop);
                if (p3 < stop && buf[p3] == C0_RS) {
                    c0__v_nc(v);
                    pos = c0__v_header_names(v, p3 + 1, stop);
                } else {
                    c0__fail(&v->err, C0_ERR_SYNTAX, p2, "keyed unit outside a record");
                    return stop;
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return pos;
}

static size_t c0__v_items(c0__val *v, size_t pos, size_t stop, int depth0) {
    const uint8_t *buf = v->buf;
    for (;;) {
        uint8_t b;
        pos = c0__v_layout(v, pos, stop);
        if (pos >= stop || C0__V_ERR(v)) break;
        b = buf[pos];
        if (b == C0_RS) {
            pos = c0__v_record(v, pos + 1, stop);
        } else if (b == C0_ETB) {
            pos = c0__v_commit(v, pos, stop, depth0);
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 >= stop || buf[p2] >= 0x20 || buf[p2] == C0_STX) {
                pos = c0__v_notes(v, pos, stop);
            } else if (buf[p2] == C0_RS) {
                c0__v_nc(v);
                pos = c0__v_record(v, p2 + 1, stop);
            } else if (buf[p2] == C0_SOH) {
                size_t p3 = c0__layout(buf, p2 + 1, stop);
                if (p3 < stop && buf[p3] == C0_RS) {
                    c0__v_nc(v);
                    pos = c0__v_header_names(v, p3 + 1, stop);
                } else {
                    c0__fail(&v->err, C0_ERR_SYNTAX, p2, "keyed unit outside a record");
                    return stop;
                }
            } else if (buf[p2] == C0_DLE) {
                c0__fail(&v->err, C0_ERR_SYNTAX, p2, "a nested level is not an item");
                return stop;
            } else {
                break; /* a dead GS or FS: the caller's business */
            }
        } else if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_RS) {
                c0__fail(&v->err, C0_ERR_SYNTAX, pos, "header row not at the start of its group");
            } else {
                c0__fail(&v->err, C0_ERR_SYNTAX, pos, "keyed unit outside a record");
            }
            return stop;
        } else {
            break;
        }
    }
    return pos;
}

static size_t c0__v_group(c0__val *v, size_t pos, size_t stop, int depth0) {
    while (pos < stop && v->buf[pos] == C0_GS) pos = c0__v_layout(v, pos + 1, stop);
    pos = c0__v_plain_name(v, pos, stop);
    if (C0__V_ERR(v)) return stop;
    pos = c0__v_pre(v, pos, stop, depth0);
    pos = c0__v_header_row(v, pos, stop);
    return c0__v_items(v, pos, stop, depth0);
}

static size_t c0__v_part(c0__val *v, size_t pos, size_t stop, int depth0) {
    const uint8_t *buf = v->buf;
    pos = c0__v_pre(v, pos, stop, depth0);
    pos = c0__v_header_row(v, pos, stop);
    pos = c0__v_items(v, pos, stop, depth0);
    while (pos < stop && !C0__V_ERR(v)) {
        uint8_t b = buf[pos];
        if (b == C0_GS) {
            pos = c0__v_group(v, pos, stop, depth0);
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_GS) {
                c0__v_nc(v);
                pos = c0__v_group(v, p2, stop, depth0);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return pos;
}

static size_t c0__v_diff_body(c0__val *v, size_t pos, size_t stop);

static size_t c0__v_body(c0__val *v, size_t pos, size_t stop, int depth0) {
    const uint8_t *buf = v->buf;
    if (v->diff && depth0) return c0__v_diff_body(v, pos, stop);
    pos = c0__v_layout(v, pos, stop);
    pos = c0__v_part(v, pos, stop, depth0);
    while (pos < stop && !C0__V_ERR(v)) {
        uint8_t b = buf[pos];
        if (b == C0_FS) {
            pos = c0__v_plain_name(v, pos + 1, stop);
            pos = c0__v_part(v, pos, stop, depth0);
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_FS) {
                c0__v_nc(v);
                pos = c0__v_plain_name(v, p2 + 1, stop);
                pos = c0__v_part(v, pos, stop, depth0);
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return C0__V_ERR(v) ? stop : pos;
}

/* Does the record starting here begin with a (live or dead) keyed unit? */
static int c0__v_keyed_start(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    c0_error scratch = {C0_OK, 0, ""};
    while (pos < stop) {
        size_t p2;
        uint8_t b = buf[pos];
        if (b == C0_SOH) return 1;
        if (b != C0_BEL) return 0;
        p2 = c0__layout(buf, pos + 1, stop);
        if (p2 >= stop) return 0;
        if (buf[p2] == C0_SOH) return 1;
        if (buf[p2] < 0x20 && buf[p2] != C0_STX) return 0;
        pos = c0__layout(buf, c0__comment(buf, pos, stop, &scratch), stop);
        if (scratch.code) return 0;
    }
    return 0;
}

/* After an SOH that is not a header row. */
static size_t c0__v_keyed_unit(c0__val *v, size_t pos, size_t stop, int heading) {
    const uint8_t *buf = v->buf;
    size_t ks, ke, vs;
    pos = c0__v_layout(v, pos, stop);
    ks = pos;
    ke = pos = c0__plain(buf, pos, stop);
    if (!(pos < stop && buf[pos] == C0_US)) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "key without a value (no US)");
        return stop;
    }
    vs = pos + 1;
    pos = c0__v_value(v, vs, stop, heading);
    if (heading && !C0__V_ERR(v)) {
        c0_bytes key = c0_trim(c0__slice(buf, ks, ke));
        c0_bytes val = c0_trim(c0__slice(buf, vs, pos));
        if (c0__eq_str(key, "shape") && c0__eq_str(val, "diff")) v->diff = 1;
    }
    return pos;
}

static size_t c0__v_record(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    pos = c0__v_layout(v, pos, stop);
    if (c0__v_keyed_start(v, pos, stop)) {
        size_t rec_start = pos;
        for (;;) {
            uint8_t b;
            pos = c0__v_layout(v, pos, stop);
            if (pos >= stop || C0__V_ERR(v)) break;
            b = buf[pos];
            if (b == C0_SOH) {
                size_t p2 = c0__layout(buf, pos + 1, stop);
                if (p2 < stop && buf[p2] == C0_RS) break;
                pos = c0__v_keyed_unit(v, pos + 1, stop, 0);
            } else if (b == C0_BEL) {
                size_t p2 = c0__layout(buf, pos + 1, stop);
                if (p2 >= stop) {
                    c0__fail(&v->err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
                    return stop;
                }
                if (buf[p2] == C0_SOH) {
                    size_t p3 = c0__layout(buf, p2 + 1, stop);
                    if (p3 < stop && buf[p3] == C0_RS) break;
                    c0__v_nc(v);
                    pos = c0__v_keyed_unit(v, p2 + 1, stop, 0);
                } else if (buf[p2] >= 0x20 || buf[p2] == C0_STX) {
                    pos = c0__v_notes(v, pos, stop);
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        if (C0__V_ERR(v)) return stop;
        c0__v_check_keys(v, rec_start, pos);
        if (pos < stop && buf[pos] == C0_US) {
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "US after a keyed value (mixed record)");
            return stop;
        }
        return pos;
    }
    for (;;) {
        uint8_t b;
        pos = c0__v_value(v, pos, stop, 0);
        pos = c0__v_layout(v, pos, stop);
        if (pos >= stop || C0__V_ERR(v)) break;
        b = buf[pos];
        if (b == C0_US) {
            pos++;
        } else if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_RS) break;
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "keyed unit in a positional record (mixed record)");
            return stop;
        } else {
            break;
        }
    }
    return C0__V_ERR(v) ? stop : pos;
}

/* Would the decoded inside of a text be a plain value? */
static int c0__v_inside_plain(const uint8_t *p, size_t n) {
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20) return 0; /* a control byte, or a DLE escaping one */
    }
    return !c0_is_layout(p[0]) && !c0_is_layout(p[n - 1]);
}

static int c0__v_second_value(const uint8_t *buf, size_t after, size_t stop) {
    return after < stop && (buf[after] >= 0x20 || buf[after] == C0_STX || buf[after] == C0_DLE || buf[after] == C0_ENQ);
}

static size_t c0__v_value(c0__val *v, size_t pos, size_t stop, int heading) {
    const uint8_t *buf = v->buf;
    size_t after;
    uint8_t b;
    pos = c0__v_layout(v, pos, stop);
    if (pos >= stop) return pos;
    b = buf[pos];
    if (b >= 0x20) {
        pos = c0__plain(buf, pos, stop);
        if (c0_is_layout(buf[pos - 1])) c0__v_nc(v);
    } else if (b == C0_STX) {
        size_t start = pos;
        pos = c0__text(buf, pos, stop, &v->err);
        if (C0__V_ERR(v)) return stop;
        if (v->canonical) {
            size_t n = pos - start - 2;
            if (n == 0 || c0__v_inside_plain(buf + start + 1, n)) c0__v_nc(v); /* empty or gratuitous text */
        }
    } else if (b == C0_DLE) {
        pos = c0__v_level(v, pos, stop);
        if (C0__V_ERR(v)) return stop;
    } else if (b == C0_ENQ) {
        if (heading) {
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "reference in a heading");
            return stop;
        }
        pos = c0__v_layout(v, pos + 1, stop);
        if (pos >= stop || (buf[pos] < 0x20 && buf[pos] != C0_STX)) {
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "reference without a name");
            return stop;
        }
        if (buf[pos] == C0_STX) {
            c0_ref r;
            size_t start = pos;
            pos = c0__text(buf, pos, stop, &v->err);
            if (C0__V_ERR(v)) return stop;
            if (c0_unit_ref(c0__slice(buf, start - 1, pos), &r) != C0_OK) {
                c0__fail(&v->err, C0_ERR_SYNTAX, start, "reference path with an empty, extra or non-name segment");
                return stop;
            }
            if (r.count == 1) c0__v_nc(v); /* group reference written as text */
        } else {
            pos = c0__plain(buf, pos, stop);
        }
        return c0__v_trailing_notes(v, c0__v_layout(v, pos, stop), stop);
    } else if (b == C0_BEL) {
        size_t p2 = c0__layout(buf, pos + 1, stop);
        if (p2 >= stop) {
            c0__fail(&v->err, C0_ERR_UNEXPECTED_END, p2, "BEL at end of input");
            return stop;
        }
        if (buf[p2] == C0_DLE) {
            c0__v_nc(v);
            pos = c0__v_level(v, p2, stop);
        } else if (buf[p2] < 0x20 && buf[p2] != C0_STX) {
            return pos; /* a dead record-level element: the caller's */
        } else {
            c0__v_nc(v);
            pos = buf[p2] == C0_STX ? c0__text(buf, p2, stop, &v->err) : c0__plain(buf, p2, stop);
        }
        if (C0__V_ERR(v)) return stop;
        return c0__v_layout(v, pos, stop);
    } else {
        return pos;
    }
    after = c0__v_trailing_notes(v, c0__v_layout(v, pos, stop), stop);
    if (C0__V_ERR(v)) return stop;
    if (c0__v_second_value(buf, after, stop)) {
        c0__fail(&v->err, C0_ERR_SYNTAX, after, "two values in one unit");
        return stop;
    }
    return after;
}

static size_t c0__v_level(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    uint8_t b;
    if (++v->depth > C0__V_MAX_DEPTH) {
        c0__fail(&v->err, C0_ERR_TOO_DEEP, pos, "levels nested too deep");
        return stop;
    }
    pos = c0__v_layout(v, pos + 1, stop);
    if (pos >= stop) {
        c0__fail(&v->err, C0_ERR_UNEXPECTED_END, pos, "level not closed");
        return stop;
    }
    b = buf[pos];
    if (b == C0_CAN) {
        v->depth--;
        return pos + 1;
    }
    if (!(b == C0_FS || b == C0_GS || b == C0_RS || b == C0_SOH || b == C0_BEL)) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "DLE must be followed by a structural code");
        return stop;
    }
    pos = c0__v_body(v, pos, stop, 0);
    if (C0__V_ERR(v)) return stop;
    pos = c0__v_layout(v, pos, stop);
    if (pos >= stop) {
        c0__fail(&v->err, C0_ERR_UNEXPECTED_END, pos, "level not closed");
        return stop;
    }
    if (buf[pos] != C0_CAN) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "unexpected byte inside level");
        return stop;
    }
    v->depth--;
    return pos + 1;
}

/* After SYN: keyed units and notes. Returns the position of the first body code. */
static size_t c0__v_heading(c0__val *v, size_t pos) {
    const uint8_t *buf = v->buf;
    size_t stop = v->len, hs;
    pos = c0__v_layout(v, pos, stop);
    hs = pos;
    while (pos < stop && !C0__V_ERR(v)) {
        uint8_t b = buf[pos];
        if (b == C0_SOH) {
            size_t p2 = c0__v_layout(v, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_RS) break;
            pos = c0__v_keyed_unit(v, pos + 1, stop, 1);
        } else if (b == C0_BEL) {
            c0__v_nc(v);
            pos = c0__comment(buf, pos, stop, &v->err);
        } else if (b >= 0x20) {
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "plain text directly after SYN");
            return stop;
        } else {
            break;
        }
        pos = c0__v_layout(v, pos, stop);
    }
    if (C0__V_ERR(v)) return stop;
    c0__v_check_keys(v, hs, pos);
    return pos;
}

/* ---- diff body ---- */

static size_t c0__v_dvalue(c0__val *v, size_t pos, size_t stop) {
    pos = c0__v_layout(v, pos, stop);
    if (pos >= stop) return pos;
    if (v->buf[pos] == C0_STX) {
        pos = c0__text(v->buf, pos, stop, &v->err);
        if (C0__V_ERR(v)) return stop;
    } else if (v->buf[pos] >= 0x20) {
        pos = c0__plain(v->buf, pos, stop);
    }
    return c0__v_layout(v, pos, stop);
}

static size_t c0__v_pattern(c0__val *v, size_t pos, size_t stop) {
    for (;;) {
        pos = c0__v_dvalue(v, pos, stop);
        if (pos < stop && v->buf[pos] == C0_SUB) pos = c0__v_dvalue(v, pos + 1, stop);
        pos = c0__v_trailing_notes(v, pos, stop);
        if (C0__V_ERR(v)) return stop;
        if (!(pos < stop && v->buf[pos] == C0_US)) break;
        pos++;
    }
    return pos;
}

static size_t c0__v_replace(c0__val *v, size_t pos, size_t stop) {
    size_t start = c0__layout(v->buf, pos, stop);
    pos = c0__v_dvalue(v, pos, stop);
    if (C0__V_ERR(v)) return stop;
    if (c0_trim(c0__slice(v->buf, start, pos)).len == 0) {
        c0__fail(&v->err, C0_ERR_SYNTAX, start, "replace-all with an empty old part");
        return stop;
    }
    if (!(pos < stop && v->buf[pos] == C0_SUB)) {
        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "replace-all without SUB");
        return stop;
    }
    return c0__v_dvalue(v, pos + 1, stop);
}

static size_t c0__v_diff_body(c0__val *v, size_t pos, size_t stop) {
    const uint8_t *buf = v->buf;
    pos = c0__v_layout(v, pos, stop);
    while (pos < stop && !C0__V_ERR(v)) {
        uint8_t b = buf[pos];
        if (b == C0_FS) {
            pos = c0__v_plain_name(v, pos + 1, stop);
        } else if (b == C0_GS) {
            pos = c0__v_pattern(v, pos + 1, stop);
        } else if (b == C0_RS) {
            pos = c0__v_replace(v, pos + 1, stop);
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, stop);
            if (p2 < stop && buf[p2] == C0_FS) {
                pos = c0__v_plain_name(v, p2 + 1, stop);
            } else if (p2 < stop && buf[p2] == C0_GS) {
                pos = c0__v_pattern(v, p2 + 1, stop);
            } else if (p2 < stop && buf[p2] == C0_RS) {
                pos = c0__v_replace(v, p2 + 1, stop);
            } else {
                pos = c0__v_notes(v, pos, stop);
            }
        } else if (b == C0_ETB) {
            c0__v_nc(v);
            pos = c0__plain(buf, c0__v_layout(v, pos + 1, stop), stop);
        } else if (b == C0_EOT || b == C0_SYN || b == C0_EM) {
            break;
        } else {
            c0__fail(&v->err, C0_ERR_SYNTAX, pos, "unexpected byte in a diff body");
            return stop;
        }
        pos = c0__v_layout(v, pos, stop);
    }
    return C0__V_ERR(v) ? stop : pos;
}

static void c0__v_run(c0__val *v) {
    const uint8_t *buf = v->buf;
    size_t len = v->len, pos;
    int first = 1;
    pos = c0__v_layout(v, 0, len);
    while (pos < len && !C0__V_ERR(v)) {
        uint8_t b = buf[pos];
        if (b == C0_SYN) {
            c0__v_nc(v);
            pos = c0__v_heading(v, pos + 1);
            first = 1;
        } else if (b == C0_EM) {
            c0__v_nc(v);
            return; /* the raw region follows */
        } else {
            if (!first) {
                c0__fail(&v->err, C0_ERR_SYNTAX, pos, "a document after the first must begin with SYN");
                return;
            }
            pos = c0__v_body(v, pos, len, 1);
            if (C0__V_ERR(v)) return;
            pos = c0__v_layout(v, pos, len);
            first = 0;
            if (pos < len) {
                b = buf[pos];
                if (b == C0_EOT) {
                    c0__v_nc(v);
                    pos = c0__v_layout(v, pos + 1, len);
                    if (pos < len && buf[pos] != C0_SYN && buf[pos] != C0_EM) {
                        c0__fail(&v->err, C0_ERR_SYNTAX, pos, "unexpected byte after EOT");
                        return;
                    }
                } else if (b == C0_SYN || b == C0_EM) {
                    continue;
                } else if (b < 0x20 && !c0__assigned[b]) {
                    c0__fail(&v->err, C0_ERR_UNASSIGNED, pos, "unassigned control code");
                    return;
                } else {
                    c0__fail(&v->err, C0_ERR_SYNTAX, pos, "unexpected byte after document");
                    return;
                }
            }
        }
    }
}

static void c0__v_init(c0__val *v, const uint8_t *buf, size_t len, int canonical) {
    v->buf = buf;
    v->len = len;
    v->canonical = canonical;
    v->ok = 1;
    v->diff = 0;
    v->depth = 0;
    c0__err_clear(&v->err);
}

c0_status c0_validate(const uint8_t *buf, size_t len, c0_error *err) {
    c0__val v;
    c0__v_init(&v, buf, len, 0);
    c0__v_run(&v);
    if (err) *err = v.err;
    return v.err.code;
}

int c0_canonical(const uint8_t *buf, size_t len) {
    c0__val v;
    c0__v_init(&v, buf, len, 1);
    c0__v_run(&v);
    return v.err.code == C0_OK && v.ok;
}

/* ---- pretty form (pretty.cr) ------------------------------------------- */

typedef struct {
    uint8_t *d;
    size_t n;
    size_t cap;
    int oom;
} c0__sb;

static int c0__sb_reserve(c0__sb *s, size_t extra) {
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

static void c0__sb_byte(c0__sb *s, uint8_t b) {
    if (c0__sb_reserve(s, 1)) s->d[s->n++] = b;
}

static void c0__sb_raw(c0__sb *s, const uint8_t *p, size_t n) {
    if (n && c0__sb_reserve(s, n)) {
        memcpy(s->d + s->n, p, n);
        s->n += n;
    }
}

static void c0__sb_cstr(c0__sb *s, const char *p) { c0__sb_raw(s, (const uint8_t *)p, strlen(p)); }

/* U+2400 + byte as 3-byte UTF-8. */
static void c0__sb_glyph(c0__sb *s, uint8_t byte) {
    c0__sb_byte(s, 0xE2);
    c0__sb_byte(s, 0x90);
    c0__sb_byte(s, (uint8_t)(0x80 | byte));
}

static void c0__sb_spaces(c0__sb *s, size_t n) {
    while (n--) c0__sb_byte(s, ' ');
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    c0__sb *o;
    const char *indent;
    size_t ilen;
    size_t depth;  /* nesting depth (levels) */
    size_t gdepth; /* group depth within the current level */
    int ls;        /* at the start of a line */
    size_t *stack;
    size_t sn, scap;
    c0_error *err;
} c0__pf;

static void c0__pf_push(c0__pf *f, size_t v) {
    if (f->sn == f->scap) {
        size_t ncap = f->scap ? f->scap * 2 : 16;
        size_t *ns = (size_t *)realloc(f->stack, ncap * sizeof(size_t));
        if (!ns) {
            c0__fail(f->err, C0_ERR_OOM, 0, "out of memory");
            return;
        }
        f->stack = ns;
        f->scap = ncap;
    }
    f->stack[f->sn++] = v;
}

static size_t c0__pf_pop(c0__pf *f) { return f->sn ? f->stack[--f->sn] : 0; }

/* Pretty form cannot show a value holding a Control Picture (U+2400-U+241F:
 * UTF-8 E2 90 80-9F). */
static void c0__check_glyphs(c0__pf *f, const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i + 2 < n; i++) {
        if (p[i] == 0xE2 && p[i + 1] == 0x90 && p[i + 2] >= 0x80 && p[i + 2] <= 0x9F) {
            c0__fail(f->err, C0_ERR_SYNTAX, 0, "glyph in data");
            return;
        }
    }
}

static void c0__pf_data(c0__pf *f, c0_bytes d) {
    c0__check_glyphs(f, d.ptr, d.len);
    c0__sb_raw(f->o, d.ptr, d.len);
}

/* A whole text (STX … ETX): glyphs for the brackets and for any control
 * byte inside other than HT, LF and CR; everything else as is. */
static void c0__pf_text(c0__pf *f, c0_bytes t) {
    size_t i;
    c0__check_glyphs(f, t.ptr, t.len);
    c0__sb_glyph(f->o, C0_STX);
    for (i = 1; i + 1 < t.len; i++) {
        uint8_t b = t.ptr[i];
        if (b < 0x20 && b != C0_LF && b != C0_CR && b != C0_HT) c0__sb_glyph(f->o, b);
        else c0__sb_byte(f->o, b);
    }
    c0__sb_glyph(f->o, C0_ETX);
}

static size_t c0__pf_plain(c0__pf *f, size_t pos) {
    size_t start;
    pos = c0__layout(f->buf, pos, f->len);
    start = pos;
    pos = c0__plain(f->buf, pos, f->len);
    c0__pf_data(f, c0_trim(c0__slice(f->buf, start, pos)));
    return pos;
}

static void c0__pf_indent(c0__pf *f, size_t depth) {
    while (depth--) c0__sb_raw(f->o, (const uint8_t *)f->indent, f->ilen);
}

static void c0__pf_nl(c0__pf *f) {
    if (!f->ls) c0__sb_byte(f->o, '\n');
}

/* A level written on one line (heading values). */
static void c0__pf_inline_level(c0__pf *f, c0_bytes raw) {
    size_t i = 0;
    while (i < raw.len && !f->err->code) {
        uint8_t b = raw.ptr[i];
        if (b == C0_STX) {
            size_t stop = c0__text(raw.ptr, i, raw.len, f->err);
            c0__pf_text(f, c0__slice(raw.ptr, i, stop));
            i = stop;
        } else if (b >= 0x20) {
            size_t start = i;
            i = c0__plain(raw.ptr, i, raw.len);
            c0__pf_data(f, c0_trim(c0__slice(raw.ptr, start, i)));
        } else if (c0_is_layout(b)) {
            i++;
        } else {
            c0__sb_glyph(f->o, b);
            i++;
        }
    }
}

/* Units of a record, header or heading on the current line. Stops at a
 * record-level code. A level opener writes ␐, breaks the line and raises the
 * depth; the caller resumes at the matching CAN. */
static size_t c0__pf_units(c0__pf *f, size_t pos) {
    const uint8_t *buf = f->buf;
    size_t len = f->len;
    while (pos < len && !f->err->code) {
        uint8_t b = buf[pos];
        if (b >= 0x20) {
            size_t start = pos;
            pos = c0__plain(buf, pos, len);
            c0__pf_data(f, c0_trim(c0__slice(buf, start, pos)));
        } else if (c0_is_layout(b)) {
            pos++;
        } else if (b == C0_US || b == C0_ENQ || b == C0_SUB) {
            c0__sb_glyph(f->o, b);
            pos++;
        } else if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, len);
            if (p2 < len && buf[p2] == C0_RS) break;
            c0__sb_glyph(f->o, C0_SOH);
            pos++;
        } else if (b == C0_STX) {
            size_t stop = c0__text(buf, pos, len, f->err);
            c0__pf_text(f, c0__slice(buf, pos, stop));
            pos = stop;
        } else if (b == C0_DLE) {
            c0__sb_glyph(f->o, C0_DLE);
            c0__sb_byte(f->o, '\n');
            c0__pf_push(f, f->gdepth);
            f->depth++;
            f->ls = 1;
            return pos + 1;
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, len);
            if (p2 < len && (buf[p2] >= 0x20 || buf[p2] == C0_STX || buf[p2] == C0_DLE)) {
                c0__sb_glyph(f->o, C0_BEL);
                pos = p2;
            } else {
                break;
            }
        } else if (b == C0_ETB) {
            c0__sb_glyph(f->o, C0_ETB);
            pos = c0__pf_plain(f, pos + 1);
        } else {
            break;
        }
    }
    f->ls = 0;
    return pos;
}

/* SOH / RS / BEL / ETB at element level: the element and its units on one
 * line (a level opener breaks the line). */
static size_t c0__pf_element(c0__pf *f, size_t pos) {
    const uint8_t *buf = f->buf;
    size_t len = f->len;
    uint8_t b = buf[pos];
    f->ls = 0;
    if (b == C0_BEL) {
        c0__sb_glyph(f->o, C0_BEL);
        pos = c0__layout(buf, pos + 1, len);
        if (pos < len && (buf[pos] >= 0x20 || buf[pos] == C0_STX)) {
            pos = c0__pf_units(f, pos);
            c0__sb_byte(f->o, '\n');
            f->ls = 1;
            return pos;
        }
        if (pos >= len) return pos;
        return c0__pf_element(f, pos); /* a dead element, rendered on this line */
    } else if (b == C0_SOH) {
        c0__sb_glyph(f->o, C0_SOH);
        pos = c0__layout(buf, pos + 1, len);
        if (pos < len && buf[pos] == C0_RS) {
            c0__sb_glyph(f->o, C0_RS);
            pos++;
        }
        pos = c0__pf_units(f, pos);
    } else if (b == C0_RS) {
        c0__sb_glyph(f->o, C0_RS);
        pos = c0__pf_units(f, pos + 1);
    } else if (b == C0_ETB) {
        c0__sb_glyph(f->o, C0_ETB);
        pos = c0__pf_plain(f, pos + 1);
    }
    if (!f->ls) {
        c0__sb_byte(f->o, '\n');
        f->ls = 1;
    }
    return pos;
}

static size_t c0__pf_heading(c0__pf *f, size_t pos) {
    const uint8_t *buf = f->buf;
    size_t len = f->len;
    while (pos < len && !f->err->code) {
        uint8_t b = buf[pos];
        if (b == C0_SOH) {
            size_t p2 = c0__layout(buf, pos + 1, len);
            if (p2 < len && buf[p2] == C0_RS) break;
            c0__sb_glyph(f->o, C0_SOH);
            pos = c0__pf_plain(f, p2);
            if (pos < len && buf[pos] == C0_US) {
                c0__sb_glyph(f->o, C0_US);
                pos = c0__layout(buf, pos + 1, len);
                if (pos < len && buf[pos] == C0_STX) {
                    size_t stop = c0__text(buf, pos, len, f->err);
                    c0__pf_text(f, c0__slice(buf, pos, stop));
                    pos = stop;
                } else if (pos < len && buf[pos] == C0_DLE) {
                    size_t stop = c0__level(buf, pos, len, f->err);
                    c0__pf_inline_level(f, c0__slice(buf, pos, stop));
                    pos = stop;
                } else {
                    pos = c0__pf_plain(f, pos);
                }
            }
        } else if (b == C0_BEL) {
            size_t p2 = c0__layout(buf, pos + 1, len);
            if (p2 < len && (buf[p2] >= 0x20 || buf[p2] == C0_STX)) {
                c0__sb_glyph(f->o, C0_BEL);
                pos = p2;
                if (buf[pos] == C0_STX) {
                    size_t stop = c0__text(buf, pos, len, f->err);
                    c0__pf_text(f, c0__slice(buf, pos, stop));
                    pos = stop;
                } else {
                    pos = c0__pf_plain(f, pos);
                }
            } else if (p2 < len && buf[p2] == C0_SOH) {
                c0__sb_glyph(f->o, C0_BEL);
                pos = p2;
            } else {
                break;
            }
        } else if (c0_is_layout(b)) {
            pos++;
        } else {
            break;
        }
    }
    return pos;
}

static void c0__pf_compact(c0__pf *f) {
    const uint8_t *buf = f->buf;
    size_t len = f->len, pos = 0;
    f->depth = 0;
    f->gdepth = 0;
    f->ls = 1;
    while (pos < len && !f->err->code) {
        uint8_t b = buf[pos];
        if (b >= 0x20 || c0_is_layout(b)) {
            if (c0_is_layout(b)) {
                pos++;
            } else {
                size_t start = pos;
                pos = c0__plain(buf, pos, len);
                c0__pf_data(f, c0__slice(buf, start, pos));
                f->ls = 0;
            }
            continue;
        }
        switch (b) {
            case C0_SYN:
                c0__pf_nl(f);
                c0__sb_glyph(f->o, C0_SYN);
                pos = c0__pf_heading(f, pos + 1);
                c0__sb_byte(f->o, '\n');
                f->ls = 1;
                f->gdepth = 0;
                break;
            case C0_FS:
                c0__pf_nl(f);
                c0__pf_indent(f, f->depth);
                c0__sb_glyph(f->o, C0_FS);
                pos = c0__pf_plain(f, pos + 1);
                c0__sb_byte(f->o, '\n');
                f->ls = 1;
                f->gdepth = 1;
                break;
            case C0_GS: {
                size_t run = 0;
                while (pos < len && buf[pos] == C0_GS) {
                    run++;
                    pos++;
                }
                c0__pf_nl(f);
                c0__pf_indent(f, f->depth);
                while (run--) c0__sb_glyph(f->o, C0_GS);
                pos = c0__pf_plain(f, pos);
                c0__sb_byte(f->o, '\n');
                f->ls = 1;
                f->gdepth = 1;
                break;
            }
            case C0_SOH:
            case C0_RS:
            case C0_BEL:
            case C0_ETB:
                c0__pf_nl(f);
                c0__pf_indent(f, f->depth + f->gdepth);
                pos = c0__pf_element(f, pos);
                break;
            case C0_CAN:
                if (f->depth > 0) f->depth--;
                f->gdepth = c0__pf_pop(f);
                c0__pf_nl(f);
                c0__pf_indent(f, f->depth + f->gdepth);
                c0__sb_glyph(f->o, C0_CAN);
                pos++;
                f->ls = 0;
                pos = c0__pf_units(f, pos); /* the rest of the enclosing record */
                if (!f->ls) {
                    c0__sb_byte(f->o, '\n');
                    f->ls = 1;
                }
                break;
            case C0_EOT:
                c0__pf_nl(f);
                c0__sb_glyph(f->o, C0_EOT);
                c0__sb_byte(f->o, '\n');
                pos++;
                f->ls = 1;
                break;
            case C0_EM: {
                size_t tail = len - pos - 1;
                c0__pf_nl(f);
                c0__sb_glyph(f->o, C0_EM);
                if (tail > 0) {
                    char num[32];
                    size_t k = sizeof(num);
                    num[--k] = 0;
                    do {
                        num[--k] = (char)('0' + tail % 10);
                        tail /= 10;
                    } while (tail);
                    c0__sb_byte(f->o, ' ');
                    c0__sb_cstr(f->o, num + k);
                    c0__sb_cstr(f->o, " bytes");
                }
                c0__sb_byte(f->o, '\n');
                return;
            }
            case C0_STX:
            case C0_DLE:
            case C0_ENQ:
            case C0_US:
            case C0_SUB:
                /* a unit-level code at element level (e.g. a diff pattern): inline */
                pos = c0__pf_units(f, pos);
                c0__sb_byte(f->o, '\n');
                f->ls = 1;
                break;
            default:
                c0__fail(f->err, C0_ERR_UNASSIGNED, pos, "unassigned control code");
                return;
        }
    }
    if (!f->ls) c0__sb_byte(f->o, '\n');
}

/* ---- alignment ---- */

static int c0__glyph_at(c0_bytes s, size_t i, uint8_t *code) {
    if (i + 2 < s.len && s.ptr[i] == 0xE2 && s.ptr[i + 1] == 0x90 && (s.ptr[i + 2] & 0xE0) == 0x80) {
        *code = (uint8_t)(s.ptr[i + 2] & 0x1F);
        return 1;
    }
    return 0;
}

static int c0__starts_glyph(c0_bytes s, uint8_t code) {
    uint8_t c;
    return c0__glyph_at(s, 0, &c) && c == code;
}

static size_t c0__cp_count(c0_bytes s) {
    size_t i, n = 0;
    for (i = 0; i < s.len; i++) {
        if ((s.ptr[i] & 0xC0) != 0x80) n++;
    }
    return n;
}

static size_t c0__utf8_adv(c0_bytes s, size_t i) {
    uint8_t c = s.ptr[i];
    size_t adv = 1;
    if ((c >> 5) == 0x6) adv = 2;
    else if ((c >> 4) == 0xE) adv = 3;
    else if ((c >> 3) == 0x1E) adv = 4;
    if (i + adv > s.len) adv = s.len - i;
    return adv;
}

typedef struct {
    size_t line;
    c0_bytes prefix;
    c0_bytes *fields;
    int *sep_us; /* sep_us[i]: a ␟ stood between fields i and i+1 (else an SOH mark begins i+1) */
    size_t n, cap;
} c0__tline;

static int c0__tl_push(c0__tline *t, c0_bytes field, int sep) {
    if (t->n == t->cap) {
        size_t ncap = t->cap ? t->cap * 2 : 8;
        c0_bytes *nf = (c0_bytes *)realloc(t->fields, ncap * sizeof(c0_bytes));
        int *ns;
        if (!nf) return 0;
        t->fields = nf;
        ns = (int *)realloc(t->sep_us, ncap * sizeof(int));
        if (!ns) return 0;
        t->sep_us = ns;
        t->cap = ncap;
    }
    t->fields[t->n] = c0_trim(field);
    t->sep_us[t->n] = sep;
    t->n++;
    return 1;
}

/* Split a ␞ or ␁␞ line into its units on ␟ (text is not split; a keyed
 * record splits on its ␁ marks too). Returns 1 and fills *t, 0 when the
 * line is not a table line, -1 on OOM. */
static int c0__parse_table_line(c0_bytes text, size_t line, c0__tline *t) {
    size_t i = 0, field_start;
    uint8_t code;
    int in_text = 0, keyed;
    memset(t, 0, sizeof(*t));
    t->line = line;
    while (i < text.len && (text.ptr[i] == ' ' || text.ptr[i] == '\t')) i++;
    if (i >= text.len) return 0;
    if (c0__glyph_at(text, i, &code) && code == C0_SOH) {
        i += 3;
        if (!(c0__glyph_at(text, i, &code) && code == C0_RS)) return 0;
        i += 3;
    } else if (c0__glyph_at(text, i, &code) && code == C0_RS) {
        i += 3;
    } else {
        return 0;
    }
    t->prefix = c0__slice(text.ptr, 0, i);
    keyed = c0__glyph_at(text, i, &code) && code == C0_SOH;
    field_start = i;
    while (i < text.len) {
        size_t adv = c0__utf8_adv(text, i);
        int is_glyph = c0__glyph_at(text, i, &code);
        if (in_text) {
            if (is_glyph && code == C0_ETX) in_text = 0;
            i += adv;
            if (is_glyph && code == C0_DLE && i < text.len) i += c0__utf8_adv(text, i);
        } else if (is_glyph && code == C0_STX) {
            in_text = 1;
            i += adv;
        } else if (is_glyph && (code == C0_DLE || code == C0_CAN)) {
            goto not_table; /* nested levels are not aligned */
        } else if (is_glyph && code == C0_US) {
            if (!c0__tl_push(t, c0__slice(text.ptr, field_start, i), 1)) goto oom;
            i += adv;
            field_start = i;
        } else if (keyed && is_glyph && code == C0_SOH && i > t->prefix.len) {
            if (!c0__tl_push(t, c0__slice(text.ptr, field_start, i), 0)) goto oom;
            field_start = i;
            i += adv;
        } else {
            i += adv;
        }
    }
    if (in_text) goto not_table; /* text continues on the next line */
    if (!c0__tl_push(t, c0__slice(text.ptr, field_start, text.len), 1)) goto oom;
    if (t->n < 2) goto not_table;
    return 1;
not_table:
    free(t->fields);
    free(t->sep_us);
    memset(t, 0, sizeof(*t));
    return 0;
oom:
    free(t->fields);
    free(t->sep_us);
    memset(t, 0, sizeof(*t));
    return -1;
}

static int c0__is_prefix_glyph(uint8_t c) {
    return c == C0_FS || c == C0_GS || c == C0_RS || c == C0_SOH || c == C0_US || c == C0_SYN || c == C0_BEL;
}

/* Reformat a compact pretty string with column alignment. Keyed records
 * align on their US like two-column tables. Returns 0 on OOM. */
static int c0__align(const uint8_t *pretty, size_t plen, int spaced, c0__sb *out) {
    c0_bytes *lines = NULL;
    size_t nlines = 0, i, s = 0;
    c0__tline *tl = NULL;   /* table lines, grouped contiguously */
    size_t ntl = 0, tlcap = 0;
    size_t *gstart = NULL;  /* start index into tl of each group */
    size_t ng = 0, gcap = 0;
    int in_group = 0;
    c0__sb *repl = NULL;    /* replacement text per line (NULL = unchanged) */
    int ok = 0;
    const char *sp = spaced ? " " : "";

    /* split lines */
    for (i = 0; i <= plen; i++) {
        if (i == plen || pretty[i] == '\n') {
            if (i == plen && i == s) break; /* drop the empty last line */
            {
                c0_bytes *nl = (c0_bytes *)realloc(lines, (nlines + 1) * sizeof(c0_bytes));
                if (!nl) goto done;
                lines = nl;
            }
            lines[nlines++] = c0__slice(pretty, s, i);
            s = i + 1;
        }
    }
    repl = (c0__sb *)calloc(nlines ? nlines : 1, sizeof(c0__sb));
    if (!repl) goto done;

    /* find table groups */
    for (i = 0; i < nlines; i++) {
        c0_bytes trimmed = c0_trim(lines[i]);
        int close = 0;
        if (trimmed.len == 0 || c0__starts_glyph(trimmed, C0_GS) || c0__starts_glyph(trimmed, C0_FS) ||
            c0__starts_glyph(trimmed, C0_EOT) || c0__starts_glyph(trimmed, C0_SYN) || c0__starts_glyph(trimmed, C0_CAN)) {
            close = 1;
        } else if (c0__starts_glyph(trimmed, C0_SOH) || c0__starts_glyph(trimmed, C0_RS)) {
            c0__tline t;
            int r = c0__parse_table_line(lines[i], i, &t);
            if (r < 0) goto done;
            if (r == 0) {
                close = 1;
            } else {
                if (ntl == tlcap) {
                    size_t ncap = tlcap ? tlcap * 2 : 16;
                    c0__tline *nt = (c0__tline *)realloc(tl, ncap * sizeof(c0__tline));
                    if (!nt) {
                        free(t.fields);
                        free(t.sep_us);
                        goto done;
                    }
                    tl = nt;
                    tlcap = ncap;
                }
                if (!in_group) {
                    if (ng == gcap) {
                        size_t ncap = gcap ? gcap * 2 : 16;
                        size_t *ngs = (size_t *)realloc(gstart, ncap * sizeof(size_t));
                        if (!ngs) {
                            free(t.fields);
                            free(t.sep_us);
                            goto done;
                        }
                        gstart = ngs;
                        gcap = ncap;
                    }
                    gstart[ng++] = ntl;
                    in_group = 1;
                }
                tl[ntl++] = t;
            }
        } else {
            close = 1;
        }
        if (close) in_group = 0;
    }

    /* align each group */
    for (i = 0; i < ng; i++) {
        size_t from = gstart[i], to = (i + 1 < ng) ? gstart[i + 1] : ntl, j, c, cols = 0;
        size_t *widths;
        for (j = from; j < to; j++) {
            if (tl[j].n > cols) cols = tl[j].n;
        }
        widths = (size_t *)calloc(cols ? cols : 1, sizeof(size_t));
        if (!widths) goto done;
        for (j = from; j < to; j++) {
            for (c = 0; c < tl[j].n; c++) {
                size_t w = c0__cp_count(tl[j].fields[c]);
                if (w > widths[c]) widths[c] = w;
            }
        }
        for (j = from; j < to; j++) {
            c0__sb *o = &repl[tl[j].line];
            c0__sb_raw(o, tl[j].prefix.ptr, tl[j].prefix.len);
            c0__sb_cstr(o, sp);
            for (c = 0; c < tl[j].n; c++) {
                c0__sb_raw(o, tl[j].fields[c].ptr, tl[j].fields[c].len);
                if (c + 1 < tl[j].n) {
                    c0__sb_spaces(o, widths[c] - c0__cp_count(tl[j].fields[c]));
                    if (tl[j].sep_us[c]) {
                        c0__sb_cstr(o, sp);
                        c0__sb_glyph(o, C0_US);
                        c0__sb_cstr(o, sp);
                    } else {
                        c0__sb_byte(o, ' ');
                    }
                }
            }
            if (o->oom) {
                free(widths);
                goto done;
            }
        }
        free(widths);
    }

    /* other lines: one space after the prefix glyphs in spaced mode */
    for (i = 0; i < nlines; i++) {
        c0_bytes text = lines[i];
        size_t ws_end = 0, glyph_end;
        uint8_t code;
        c0_bytes rest;
        c0__sb *o = &repl[i];
        if (o->d) continue;
        if (c0_trim(text).len == 0) continue;
        while (ws_end < text.len && (text.ptr[ws_end] == ' ' || text.ptr[ws_end] == '\t')) ws_end++;
        glyph_end = ws_end;
        while (c0__glyph_at(text, glyph_end, &code) && c0__is_prefix_glyph(code)) glyph_end += 3;
        if (glyph_end == ws_end || glyph_end >= text.len) continue;
        rest = c0__slice(text.ptr, glyph_end, text.len);
        while (rest.len && c0_is_layout(rest.ptr[0])) {
            rest.ptr++;
            rest.len--;
        }
        c0__sb_raw(o, text.ptr, glyph_end);
        c0__sb_cstr(o, sp);
        c0__sb_raw(o, rest.ptr, rest.len);
        if (o->oom) goto done;
    }

    for (i = 0; i < nlines; i++) {
        if (repl[i].d) c0__sb_raw(out, repl[i].d, repl[i].n);
        else c0__sb_raw(out, lines[i].ptr, lines[i].len);
        c0__sb_byte(out, '\n');
    }
    ok = !out->oom;
done:
    for (i = 0; i < ntl; i++) {
        free(tl[i].fields);
        free(tl[i].sep_us);
    }
    free(tl);
    free(gstart);
    if (repl) {
        for (i = 0; i < nlines; i++) free(repl[i].d);
        free(repl);
    }
    free(lines);
    return ok;
}

char *c0_pretty_format(const uint8_t *buf, size_t len, const char *indent, c0_pretty_mode mode, size_t *outlen, c0_error *err) {
    c0__sb o = {NULL, 0, 0, 0}, aligned = {NULL, 0, 0, 0};
    c0__pf f;
    c0_error local = {C0_OK, 0, ""};
    if (!err) err = &local;
    c0__err_clear(err);
    memset(&f, 0, sizeof(f));
    f.buf = buf;
    f.len = len;
    f.o = &o;
    f.indent = indent ? indent : "  ";
    f.ilen = strlen(f.indent);
    f.err = err;
    c0__pf_compact(&f);
    free(f.stack);
    if (o.oom) c0__fail(err, C0_ERR_OOM, 0, "out of memory");
    if (err->code) {
        free(o.d);
        return NULL;
    }
    if (mode != C0_PRETTY_COMPACT) {
        if (!c0__align(o.d, o.n, mode == C0_PRETTY_SPACED, &aligned)) {
            free(o.d);
            free(aligned.d);
            c0__fail(err, C0_ERR_OOM, 0, "out of memory");
            return NULL;
        }
        free(o.d);
        o = aligned;
    }
    c0__sb_byte(&o, 0);
    if (o.oom) {
        free(o.d);
        c0__fail(err, C0_ERR_OOM, 0, "out of memory");
        return NULL;
    }
    if (outlen) *outlen = o.n - 1;
    return (char *)o.d;
}

/* Decode one UTF-8 code point at s[i]; *adv gets its byte length. */
static unsigned c0__utf8(const uint8_t *s, size_t len, size_t i, size_t *adv) {
    uint8_t c = s[i];
    if (c < 0x80) {
        *adv = 1;
        return c;
    } else if ((c >> 5) == 0x6 && i + 1 < len) {
        *adv = 2;
        return (unsigned)((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    } else if ((c >> 4) == 0xE && i + 2 < len) {
        *adv = 3;
        return (unsigned)((c & 0x0F) << 12) | ((unsigned)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
    } else if ((c >> 3) == 0x1E && i + 3 < len) {
        *adv = 4;
        return (unsigned)((c & 0x07) << 18) | ((unsigned)(s[i + 1] & 0x3F) << 12) |
               ((unsigned)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
    }
    *adv = 1;
    return c;
}

/* Inside text: everything verbatim until the ETX glyph; a DLE glyph takes
 * the next glyph with it. */
static size_t c0__pp_text(const uint8_t *str, size_t len, size_t i, c0__sb *o) {
    while (i < len) {
        size_t adv;
        unsigned cp = c0__utf8(str, len, i, &adv);
        if (cp >= 0x2400 && cp <= 0x241F) {
            uint8_t code = (uint8_t)(cp - 0x2400);
            c0__sb_byte(o, code);
            i += adv;
            if (code == C0_DLE && i < len) {
                unsigned nx = c0__utf8(str, len, i, &adv);
                if (nx >= 0x2400 && nx <= 0x241F) {
                    c0__sb_byte(o, (uint8_t)(nx - 0x2400));
                    i += adv;
                }
            } else if (code == C0_ETX) {
                return i;
            }
        } else {
            c0__sb_raw(o, str + i, adv);
            i += adv;
        }
    }
    return i;
}

uint8_t *c0_pretty_parse(const char *str_, size_t len, size_t *outlen) {
    const uint8_t *str = (const uint8_t *)str_;
    c0__sb o = {NULL, 0, 0, 0}, pending = {NULL, 0, 0, 0};
    size_t i = 0;
    int after_code = 1;
    while (i < len) {
        size_t adv;
        unsigned cp = c0__utf8(str, len, i, &adv);
        if (cp >= 0x2400 && cp <= 0x241F) {
            uint8_t code = (uint8_t)(cp - 0x2400);
            pending.n = 0;
            c0__sb_byte(&o, code);
            i += adv;
            if (code == C0_STX) {
                i = c0__pp_text(str, len, i, &o);
            } else if (code == C0_EM) {
                while (i < len && str[i] != '\n') i++; /* the rest of the line is a summary */
            }
            after_code = 1;
        } else if (cp == '\n' || cp == '\r') {
            pending.n = 0;
            after_code = 1;
            i += adv;
        } else if (cp == ' ' || cp == '\t') {
            if (!after_code) c0__sb_byte(&pending, (uint8_t)cp);
            i += adv;
        } else {
            if (pending.n) {
                c0__sb_raw(&o, pending.d, pending.n);
                pending.n = 0;
            }
            after_code = 0;
            c0__sb_raw(&o, str + i, adv);
            i += adv;
        }
    }
    free(pending.d);
    if (o.oom || pending.oom) {
        free(o.d);
        return NULL;
    }
    if (!o.d) o.d = (uint8_t *)malloc(1); /* non-NULL even when empty */
    if (outlen) *outlen = o.n;
    return o.d;
}

#endif /* C0_IMPLEMENTATION */
#endif /* C0_H */
