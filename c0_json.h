/* c0_json.h — JSON ⇄ C0DATA conversion.
 *
 * An intermediate value tree (c0_json_value) mirrors the Crystal reference: every
 * scalar is a string, objects keep insertion order. c0_to_value / c0_from_value
 * are the structural conversions; c0_to_json / c0_from_json add a
 * dependency-free JSON text codec. Export never guesses: a positional record
 * is an array, a keyed record an object, a group with a header row an array
 * of objects, a nested level the same shapes one level down, a reference
 * {"$ref": "group/record/field"}. The round-trip is stringly typed: JSON
 * numbers, booleans and null become strings, and every leaf serializes back
 * out as a quoted JSON string.
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
} c0_json_kind;

typedef struct c0_json_value c0_json_value;

typedef struct {
    char     *key;
    size_t    key_len;
    c0_json_value *val;
} c0_json_pair;

struct c0_json_value {
    c0_json_kind kind;
    char     *s;       /**< STR: malloc'd, not NUL-terminated (s_len authoritative) */
    size_t    s_len;
    c0_json_value **items;  /**< ARRAY */
    size_t    n_items;
    size_t    cap_items;
    c0_json_pair  *pairs;   /**< OBJECT */
    size_t    n_pairs;
    size_t    cap_pairs;
};

/* --- Value constructors (all malloc'd; free the root with c0_json_free) --- */
c0_json_value *c0_json_str(const char *s, size_t len);
c0_json_value *c0_json_array(void);
c0_json_value *c0_json_object(void);
int  c0_json_array_push(c0_json_value *arr, c0_json_value *item);                                   /**< takes ownership of item */
int  c0_json_object_push(c0_json_value *obj, const char *key, size_t key_len, c0_json_value *val);  /**< copies key, owns val */
void c0_json_free(c0_json_value *v);

/* --- Structural conversion --- */

/** A value tree from C0DATA bytes (the first document's body). NULL on
 * malformed input (detail in *err when given) or allocation failure. */
c0_json_value *c0_to_value(const uint8_t *buf, size_t len, c0_error *err);

/** A value tree as C0DATA compact bytes (malloc'd; caller frees).
 * group_name names the group for data that has none ("data" when NULL).
 * NULL on allocation failure. */
uint8_t *c0_from_value(const c0_json_value *v, const char *group_name, size_t *out_len);

/* --- JSON text --- */

/** Parse JSON text into a value tree (numbers/bools become strings, null
 * the empty string). NULL on parse error or allocation failure. */
c0_json_value *c0_json_parse(const char *json, size_t len);

/** Serialize a value tree to pretty JSON (2-space indent), NUL-terminated
 * and malloc'd (caller frees). NULL on allocation failure. */
char *c0_json_print(const c0_json_value *v, size_t *out_len);

/** C0DATA bytes → pretty JSON string (malloc'd, NUL-terminated). */
char *c0_to_json(const uint8_t *buf, size_t len, size_t *out_len, c0_error *err);

/** JSON text → C0DATA compact bytes (malloc'd). group_name as for
 * c0_from_value. NULL on parse error or allocation failure. */
uint8_t *c0_from_json(const char *json, size_t len, const char *group_name, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* C0_JSON_H */
