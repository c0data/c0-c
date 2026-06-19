/* c0_diff.h — C0DIFF: atomic, anchored multi-file edits.
 *
 * Format: [FS]<path>[GS]<literal>[US]<old>[SUB]<new>[US]<literal>.
 * FS starts a file block, GS a section, US separates pattern units
 * (anchor <-> replacement), SUB separates old from new within a
 * substitution, and DLE escapes literal control codes.
 */
#ifndef C0_DIFF_H
#define C0_DIFF_H

#include "c0.h"
#include "c0_strbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Parsed model --- */

/* A pattern unit. `search` is the text matched in the target; `replace` is
 * what it becomes. For a pure anchor the two are identical (is_sub == 0). */
typedef struct {
    uint8_t *search;
    size_t   search_len;
    uint8_t *replace;
    size_t   replace_len;
    int      is_sub;
} c0_diff_unit;

/* A section: a sequential pattern of units. */
typedef struct {
    c0_diff_unit *units;
    size_t        nunits;
} c0_diff_section;

/* A file edit: a path and its sections. */
typedef struct {
    uint8_t        *path;
    size_t          path_len;
    c0_diff_section *sections;
    size_t          nsections;
} c0_diff_file;

/* A parsed C0DIFF document. */
typedef struct {
    c0_diff_file *files;
    size_t        nfiles;
    int           oom;
} c0_diff;

/* Parse a C0DIFF buffer. Free the result with c0_diff_free. On allocation
 * failure the returned doc has .oom = 1 (and partial contents already freed). */
c0_diff c0_diff_parse(const uint8_t *buf, size_t len);
void    c0_diff_free(c0_diff *d);

/* Concatenate a section's search pattern / replacement into a fresh malloc'd
 * buffer (caller frees). Returns NULL only on allocation failure; *out_len gets
 * the length (0 for an empty section yields a 1-byte allocation, len 0). */
uint8_t *c0_diff_section_search(const c0_diff_section *s, size_t *out_len);
uint8_t *c0_diff_section_replace(const c0_diff_section *s, size_t *out_len);

/* --- Apply --- */

typedef enum {
    C0_DIFF_OK = 0,
    C0_DIFF_FILE_NOT_FOUND,
    C0_DIFF_PATTERN_NOT_FOUND,
    C0_DIFF_PATTERN_AMBIGUOUS,
    C0_DIFF_OOM
} c0_diff_status;

typedef struct {
    c0_diff_status status;
    char           path[256]; /* offending file (truncated, NUL-terminated) */
    size_t         section;   /* offending section index */
    size_t         count;     /* match count (for AMBIGUOUS) */
} c0_diff_error;

/* An input file (borrowed, NUL-terminated path; content need not be). */
typedef struct {
    const char *path;
    const char *content;
    size_t      content_len;
} c0_diff_file_input;

/* An output file (malloc'd; free path and content). */
typedef struct {
    char  *path;
    char  *content;
    size_t content_len;
} c0_diff_file_output;

/* Apply a C0DIFF to a set of in-memory files. Atomic: every section's search
 * pattern must occur exactly once in its target before any edit is made.
 * On success returns 1, allocates *out (one entry per input file, in input
 * order, modified or passed through unchanged) and sets *out_count; the caller
 * frees each entry's path/content and then the array via c0_diff_free_outputs.
 * On failure returns 0, fills *err, and allocates nothing. */
int c0_diff_apply(const uint8_t *diff_buf, size_t diff_len,
                  const c0_diff_file_input *files, size_t nfiles,
                  c0_diff_file_output **out, size_t *out_count,
                  c0_diff_error *err);

void c0_diff_free_outputs(c0_diff_file_output *out, size_t count);

/* --- Builder --- */

typedef struct {
    c0_strbuf buf;
    int       unit_first; /* true at the start of a section */
} c0_diff_builder;

void c0_diff_builder_init(c0_diff_builder *b);
void c0_diff_builder_free(c0_diff_builder *b);

/* Begin a file block (FS + path). */
void c0_diff_file_begin(c0_diff_builder *b, const char *path, size_t path_len);
/* Begin a section (GS). */
void c0_diff_section_begin(c0_diff_builder *b);
/* Add a literal anchor / a substitution to the current section. */
void c0_diff_anchor(c0_diff_builder *b, const char *text, size_t len);
void c0_diff_sub(c0_diff_builder *b, const char *old, size_t old_len,
                 const char *new_, size_t new_len);
/* Convenience: a single anchored find/replace section (begins its own GS). */
void c0_diff_replace(c0_diff_builder *b,
                     const char *before, size_t before_len,
                     const char *old, size_t old_len,
                     const char *new_, size_t new_len,
                     const char *after, size_t after_len);

/* Borrow the builder's bytes (valid until the builder is freed/extended). */
c0_bytes c0_diff_builder_bytes(const c0_diff_builder *b);

#ifdef __cplusplus
}
#endif

#endif /* C0_DIFF_H */
