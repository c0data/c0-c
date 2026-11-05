/* c0_csv.h — CSV ⇄ C0DATA conversion. */
#ifndef C0_CSV_H
#define C0_CSV_H

#include "c0.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CSV text (RFC 4180 quoting) → C0DATA: one group with a header row from the
 * first CSV row and a positional record per further row. `group_name`
 * defaults to "data" when NULL. Returns a malloc'd buffer the caller frees;
 * *out_len gets the length. NULL with *out_len = 0 for empty input, or on
 * allocation failure.
 */
uint8_t *c0_from_csv(const char *csv, size_t csv_len, const char *group_name, size_t *out_len);

/**
 * A group → CSV text: the header row first when the group has one, then its
 * records. `group_name` selects a group by name (any file); NULL takes the
 * first group of the buffer. A reference is written as its path joined by
 * '/' (`tags/001`); a nested level has no CSV form and is an error. Returns
 * a malloc'd, NUL-terminated string the caller frees; *out_len (if not
 * NULL) gets its length. NULL on error, with detail in *err when given
 * (C0_ERR_NOT_FOUND: no such group; C0_ERR_SYNTAX: a level or malformed
 * input; C0_ERR_OOM).
 */
char *c0_to_csv(const uint8_t *buf, size_t len, const char *group_name, size_t *out_len, c0_error *err);

#ifdef __cplusplus
}
#endif

#endif /* C0_CSV_H */
