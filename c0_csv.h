/* c0_csv.h — CSV ⇄ C0DATA conversion. */
#ifndef C0_CSV_H
#define C0_CSV_H

#include "c0.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert CSV text to C0DATA compact bytes: the first row becomes SOH headers,
 * the rest become records, in a single group. `group_name` defaults to "data"
 * when NULL. Returns a malloc'd buffer the caller frees; *out_len gets the
 * length. Returns NULL with *out_len = 0 for empty input (or on allocation
 * failure).
 */
uint8_t *c0_from_csv(const char *csv, size_t csv_len, const char *group_name, size_t *out_len);

/*
 * Convert C0DATA bytes to CSV text (the first table: an FS-prefixed document's
 * first group, or a bare group). Returns a malloc'd, NUL-terminated string the
 * caller frees; *out_len (if not NULL) gets the byte length. NULL on allocation
 * failure.
 */
char *c0_to_csv(const uint8_t *buf, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* C0_CSV_H */
