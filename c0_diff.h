/* c0_diff.h — C0-DIFF: atomic multi-file edits with anchored patterns.
 *
 * A diff is a C0DATA document whose heading declares `shape` `diff`
 * (DIFF.md): FS begins a file (its path), GS a section (one anchored
 * pattern of US-separated units, each an anchor or `old SUB new`), RS a
 * replace-all (`old SUB new`) within the scope of the section before it.
 * Anchors, olds and news are values under the ordinary rules (plain or
 * text); paths are names. Notes are skipped; a dead FS/GS/RS line is parsed
 * and dropped, and what follows attaches to the previous live element.
 */
#ifndef C0_DIFF_H
#define C0_DIFF_H

#include "c0.h"
#include "c0_strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Parsed model (all buffers malloc'd; free the root with c0_diff_free) --- */

/** One unit of a pattern: an anchor (is_sub 0, new_ NULL) or a substitution. */
typedef struct {
    uint8_t *old;
    size_t   old_len;
    uint8_t *new_;
    size_t   new_len;
    int      is_sub;
} c0_diff_unit;

/** A replace-all: every `old` in scope becomes `new_`. */
typedef struct {
    uint8_t *old;
    size_t   old_len;
    uint8_t *new_;
    size_t   new_len;
} c0_diff_replacement;

/** A section: one anchored pattern, plus the replace-alls that follow it. */
typedef struct {
    c0_diff_unit    *units;
    size_t           nunits;
    c0_diff_replacement *replaces;
    size_t           nreplaces;
} c0_diff_section;

/** A file's edits. `leading` are the replace-alls before any section: their
 * scope is the start of the file up to the first section's match. */
typedef struct {
    uint8_t         *path;
    size_t           path_len;
    c0_diff_replacement *leading;
    size_t           nleading;
    c0_diff_section *sections;
    size_t           nsections;
} c0_diff_file;

/** A parsed diff. `status` is C0_OK, or the error that stopped parsing
 * (C0_ERR_SYNTAX with detail in `err`; C0_ERR_OOM). */
typedef struct {
    c0_diff_file *files;
    size_t        nfiles;
    c0_status     status;
    c0_error      err;
} c0_diff;

/** Parse a diff document (with or without its SYN heading). A heading whose
 * shape is present and not `diff` is an error. */
c0_diff c0_diff_parse(const uint8_t *buf, size_t len);
void    c0_diff_free(c0_diff *d);

/** A section's search text (its anchors and old parts) or replacement (with
 * each old replaced by its new), concatenated into a malloc'd buffer the
 * caller frees. NULL only on allocation failure; *out_len gets the length. */
uint8_t *c0_diff_section_search(const c0_diff_section *s, size_t *out_len);
uint8_t *c0_diff_section_replacement(const c0_diff_section *s, size_t *out_len);

/* --- Apply --- */

typedef enum {
    C0_DIFF_OK = 0,
    C0_DIFF_SYNTAX,            /**< the diff is malformed (see msg/pos)            */
    C0_DIFF_WRONG_SHAPE,       /**< the heading's shape is not diff                */
    C0_DIFF_DUPLICATE_PATH,    /**< the same path twice                            */
    C0_DIFF_FILE_NOT_FOUND,    /**< a path with no input file                      */
    C0_DIFF_EMPTY_SEARCH,      /**< a section whose search text is empty           */
    C0_DIFF_PATTERN_NOT_FOUND, /**< a section's search text does not occur         */
    C0_DIFF_PATTERN_AMBIGUOUS, /**< it occurs more than once after the cursor      */
    C0_DIFF_REPLACE_NOT_FOUND, /**< a replace-all with zero occurrences in scope   */
    C0_DIFF_OOM
} c0_diff_status;

typedef struct {
    c0_diff_status status;
    char           path[256]; /**< offending file (truncated, NUL-terminated) */
    size_t         section;   /**< offending section index                    */
    size_t         pos;       /**< byte offset in the diff, for SYNTAX        */
    const char    *msg;       /**< static message                             */
} c0_diff_error;

/** An input file (borrowed). */
typedef struct {
    c0_bytes path;
    c0_bytes content;
} c0_diff_file_input;

/** An output file (malloc'd path and content). */
typedef struct {
    uint8_t *path;
    size_t   path_len;
    uint8_t *content;
    size_t   content_len;
} c0_diff_file_output;

/** Apply a diff to in-memory files. Atomic: everything is validated before
 * anything is produced. On success returns 1, allocates *out (one entry per
 * input file, in input order, edited or copied unchanged) and sets
 * *out_count; free with c0_diff_free_outputs. On failure returns 0, fills
 * *err (when given) and allocates nothing. */
int c0_diff_apply(const uint8_t *diff_buf, size_t diff_len,
                  const c0_diff_file_input *files, size_t nfiles,
                  c0_diff_file_output **out, size_t *out_count,
                  c0_diff_error *err);

void c0_diff_free_outputs(c0_diff_file_output *out, size_t count);

/* --- Builder --- */

typedef struct {
    c0_strbuf buf;
    int       unit_first; /**< at the start of a section's pattern */
} c0_diff_builder;

/** Start a diff; with `heading` the SYN heading `shape diff` is written first. */
void c0_diff_builder_init(c0_diff_builder *b, int heading);
void c0_diff_builder_free(c0_diff_builder *b);

/** FS and the path (a name). */
void c0_diff_file_begin(c0_diff_builder *b, const uint8_t *path, size_t path_len);
/** GS: a new section. */
void c0_diff_section_begin(c0_diff_builder *b);
/** An anchor / a substitution unit of the current section. */
void c0_diff_anchor(c0_diff_builder *b, const uint8_t *text, size_t len);
void c0_diff_sub(c0_diff_builder *b, const uint8_t *old, size_t old_len, const uint8_t *new_, size_t new_len);
/** One section: anchor, old SUB new, anchor (empty anchors are omitted). */
void c0_diff_replace(c0_diff_builder *b,
                     const uint8_t *before, size_t before_len,
                     const uint8_t *old, size_t old_len,
                     const uint8_t *new_, size_t new_len,
                     const uint8_t *after, size_t after_len);
/** RS old SUB new: every occurrence within the current section's scope. */
void c0_diff_replace_all(c0_diff_builder *b, const uint8_t *old, size_t old_len, const uint8_t *new_, size_t new_len);
/** BEL and a note. */
void c0_diff_note(c0_diff_builder *b, const uint8_t *text, size_t len);
/** EOT. */
void c0_diff_eot(c0_diff_builder *b);

/** Borrow the builder's bytes (valid until it is freed or extended). */
c0_bytes c0_diff_builder_bytes(const c0_diff_builder *b);

/* NUL-terminated convenience wrappers. */
void c0_diff_file_begin_str(c0_diff_builder *b, const char *path);
void c0_diff_anchor_str(c0_diff_builder *b, const char *text);
void c0_diff_sub_str(c0_diff_builder *b, const char *old, const char *new_);
void c0_diff_replace_str(c0_diff_builder *b, const char *before, const char *old, const char *new_, const char *after);
void c0_diff_replace_all_str(c0_diff_builder *b, const char *old, const char *new_);
void c0_diff_note_str(c0_diff_builder *b, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* C0_DIFF_H */
