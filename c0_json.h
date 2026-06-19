/* c0_json.h — JSON <-> C0DATA conversion.
 *
 * An intermediate Value tree (c0_value) mirrors the Rust/Crystal model: every
 * scalar is a string, objects preserve insertion order. to_value / from_value
 * are the structural conversions; to_json / from_json add a dependency-free
 * JSON text codec (C has no stdlib JSON). As in the reference implementations,
 * the round-trip is "stringly typed": JSON numbers, booleans and null become
 * strings, and every leaf serializes back out as a quoted JSON string.
 */
#ifndef C0_JSON_H
#define C0_JSON_H

#include "c0.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    C0_JSON_STR,
    C0_JSON_ARRAY,
    C0_JSON_OBJECT
} c0_value_kind;

typedef struct c0_value c0_value;

typedef struct {
    char     *key;
    size_t    key_len;
    c0_value *val;
} c0_pair;

struct c0_value {
    c0_value_kind kind;
    char     *s;       /* STR: malloc'd, not NUL-required (s_len authoritative) */
    size_t    s_len;
    c0_value **items;  /* ARRAY */
    size_t    n_items;
    size_t    cap_items;
    c0_pair  *pairs;   /* OBJECT */
    size_t    n_pairs;
    size_t    cap_pairs;
};

/* --- Value constructors (all malloc'd; free the root with c0_value_free) --- */
c0_value *c0_value_str(const char *s, size_t len);
c0_value *c0_value_array(void);
c0_value *c0_value_object(void);
int  c0_value_array_push(c0_value *arr, c0_value *item);             /* takes ownership of item */
int  c0_value_object_push(c0_value *obj, const char *key, size_t key_len, c0_value *val); /* copies key, owns val */
void c0_value_free(c0_value *v);

/* --- Structural conversion --- */

/* Build a Value tree from C0DATA bytes (detects tabular / key-value / nested /
 * document shapes). Returns NULL only on allocation failure. */
c0_value *c0_to_value(const uint8_t *buf, size_t len);

/* Emit a Value tree as C0DATA compact bytes (malloc'd; caller frees).
 * group_name defaults to "data" when NULL. NULL on allocation failure. */
uint8_t *c0_from_value(const c0_value *v, const char *group_name, size_t *out_len);

/* --- JSON text --- */

/* Parse JSON text into a Value tree (numbers/bools become strings, null becomes
 * the empty string). Returns NULL on parse error or allocation failure. */
c0_value *c0_json_parse(const char *json, size_t len);

/* Serialize a Value tree to pretty JSON (2-space indent), NUL-terminated and
 * malloc'd (caller frees). NULL on allocation failure. */
char *c0_json_print(const c0_value *v, size_t *out_len);

/* Convenience: C0DATA bytes -> pretty JSON string (malloc'd, NUL-terminated). */
char *c0_to_json(const uint8_t *buf, size_t len, size_t *out_len);

/* Convenience: JSON text -> C0DATA compact bytes (malloc'd). group_name
 * defaults to "data" when NULL. NULL on parse error or allocation failure. */
uint8_t *c0_from_json(const char *json, size_t len, const char *group_name, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* C0_JSON_H */
