#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static int bytes_eq(const uint8_t *got, size_t glen, const char *want, size_t wlen) {
    return glen == wlen && memcmp(got, want, wlen) == 0;
}

/* JSON text → C0 bytes must equal want; then C0 → JSON must equal back. */
static void roundtrip(const char *json, const char *want, size_t wlen, const char *back) {
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_json(json, strlen(json), NULL, &n);
    char *out;
    CHECK(c0 != NULL);
    if (!c0) return;
    CHECK(bytes_eq(c0, n, want, wlen));
    if (!bytes_eq(c0, n, want, wlen)) {
        size_t i;
        printf("  got:");
        for (i = 0; i < n; i++) printf(" %02x", c0[i]);
        printf("\n");
    }
    CHECK(c0_canonical(c0, n));
    out = c0_to_json(c0, n, &m, NULL);
    CHECK(out && strcmp(out, back) == 0);
    if (out && strcmp(out, back) != 0) printf("  json:\n%s\n", out);
    free(out);
    free(c0);
}

#define RT(json, want, back) roundtrip(json, want, sizeof(want) - 1, back)

static void test_kv_object(void) {
    /* an object of scalars: one group holding one keyed record */
    RT("{\"host\": \"localhost\", \"port\": 5432}",
       "\x1d" "data" "\x1e\x01" "host" "\x1f" "localhost" "\x01" "port" "\x1f" "5432",
       "{\n  \"data\": {\n    \"host\": \"localhost\",\n    \"port\": \"5432\"\n  }\n}");
}

static void test_table(void) {
    /* uniform objects: a group with a header row */
    RT("[{\"name\": \"Alice\", \"amount\": 100}, {\"name\": \"Bob\", \"amount\": 200}]",
       "\x1d" "data" "\x01\x1e" "name" "\x1f" "amount" "\x1e" "Alice" "\x1f" "100" "\x1e" "Bob" "\x1f" "200",
       "{\n  \"data\": [\n    {\n      \"name\": \"Alice\",\n      \"amount\": \"100\"\n    },\n    {\n      \"name\": \"Bob\",\n      \"amount\": \"200\"\n    }\n  ]\n}");
}

static void test_groups_and_file(void) {
    /* an object of groups; one key wrapping an object of groups is a file */
    RT("{\"users\": [[\"a\", \"1\"]], \"cfg\": {\"k\": \"v\"}}",
       "\x1d" "users" "\x1e" "a" "\x1f" "1" "\x1d" "cfg" "\x1e\x01" "k" "\x1f" "v",
       "{\n  \"users\": [\n    [\n      \"a\",\n      \"1\"\n    ]\n  ],\n  \"cfg\": {\n    \"k\": \"v\"\n  }\n}");
    RT("{\"db\": {\"t\": [\"x\"]}}",
       "\x1c" "db" "\x1d" "t" "\x1e" "x",
       "{\n  \"db\": {\n    \"t\": [\n      [\n        \"x\"\n      ]\n    ]\n  }\n}");
}

static void test_nested(void) {
    /* arrays and objects in unit position are levels: a list, a map, a table */
    RT("{\"name\": \"x\", \"roles\": [\"Admin\", \"Editor\"], \"addr\": {\"city\": \"X\"}, \"rows\": [{\"a\": \"1\"}, {\"a\": \"2\"}]}",
       "\x1d" "data" "\x1e\x01" "name" "\x1f" "x" "\x01" "roles" "\x1f\x10\x1e" "Admin" "\x1f" "Editor" "\x18"
       "\x01" "addr" "\x1f\x10\x1e\x01" "city" "\x1f" "X" "\x18"
       "\x01" "rows" "\x1f\x10\x01\x1e" "a" "\x1e" "1" "\x1e" "2" "\x18",
       "{\n  \"data\": {\n    \"name\": \"x\",\n    \"roles\": [\n      \"Admin\",\n      \"Editor\"\n    ],\n"
       "    \"addr\": {\n      \"city\": \"X\"\n    },\n"
       "    \"rows\": [\n      {\n        \"a\": \"1\"\n      },\n      {\n        \"a\": \"2\"\n      }\n    ]\n  }\n}");
    /* an object of arrays/objects is a set of groups, not a map */
    RT("{\"roles\": [\"Admin\"], \"cfg\": {\"k\": \"v\"}}",
       "\x1d" "roles" "\x1e" "Admin" "\x1d" "cfg" "\x1e\x01" "k" "\x1f" "v",
       "{\n  \"roles\": [\n    [\n      \"Admin\"\n    ]\n  ],\n  \"cfg\": {\n    \"k\": \"v\"\n  }\n}");
    RT("[[\"1\", [\"2\", \"3\"]], \"x\"]",
       "\x1d" "data" "\x1e" "1" "\x1f\x10\x1e" "2" "\x1f" "3" "\x18" "\x1e" "x",
       "{\n  \"data\": [\n    [\n      \"1\",\n      [\n        \"2\",\n        \"3\"\n      ]\n    ],\n    [\n      \"x\"\n    ]\n  ]\n}");
}

static void test_numbers_and_escapes(void) {
    RT("[\"a\\nb\", true, null, 1.5e3, \"\\u00e9\"]",
       "\x1d" "data" "\x1e\x02" "a\nb" "\x03" "\x1e" "true" "\x1e" "\x1e" "1.5e3" "\x1e" "\xc3\xa9",
       "{\n  \"data\": [\n    [\n      \"a\\nb\"\n    ],\n    [\n      \"true\"\n    ],\n    [\n      \"\"\n    ],\n    [\n      \"1.5e3\"\n    ],\n    [\n      \"\xc3\xa9\"\n    ]\n  ]\n}");
}

static void test_export_shapes(void) {
    /* a reference exports as {"$ref": path}; duplicate keys: last wins */
    const uint8_t in[] = "\x1d" "g" "\x1e\x01" "r" "\x1f\x05\x02" "tags" "\x1f" "001" "\x03" "\x01" "k" "\x1f" "1" "\x01" "k" "\x1f" "2";
    size_t m = 0;
    char *out = c0_to_json(in, sizeof(in) - 1, &m, NULL);
    CHECK(out && strcmp(out, "{\n  \"g\": {\n    \"r\": {\n      \"$ref\": \"tags/001\"\n    },\n    \"k\": \"2\"\n  }\n}") == 0);
    free(out);
    {
        /* a document with a heading exports its body; a bare record stream is an array */
        const uint8_t doc[] = "\x16\x01" "shape" "\x1f" "tabular" "\x1e" "a" "\x1f" "b" "\x04";
        out = c0_to_json(doc, sizeof(doc) - 1, &m, NULL);
        CHECK(out && strcmp(out, "[\n  [\n    \"a\",\n    \"b\"\n  ]\n]") == 0);
        free(out);
    }
    {
        c0_error err;
        const uint8_t bad[] = "\x1e\x02" "open";
        CHECK(c0_to_json(bad, sizeof(bad) - 1, &m, &err) == NULL && err.code == C0_ERR_UNEXPECTED_END);
    }
}

static void test_value_api(void) {
    c0_json_value *obj = c0_json_object();
    c0_json_value *arr = c0_json_array();
    size_t n = 0;
    uint8_t *c0;
    char *json;
    const char want[] = "\x1d" "g" "\x1e\x01" "name" "\x1f" "n" "\x01" "list" "\x1f\x10\x1e" "x" "\x18";
    CHECK(c0_json_object_push(obj, "name", 4, c0_json_str("n", 1)));
    CHECK(c0_json_array_push(arr, c0_json_str("x", 1)));
    CHECK(c0_json_object_push(obj, "list", 4, arr));
    c0 = c0_from_value(obj, "g", &n);
    CHECK(c0 && bytes_eq(c0, n, want, sizeof(want) - 1));
    json = c0_json_print(obj, &n);
    CHECK(json && strcmp(json, "{\n  \"name\": \"n\",\n  \"list\": [\n    \"x\"\n  ]\n}") == 0);
    free(json);
    free(c0);
    c0_json_free(obj);
}

static void test_parse_errors(void) {
    CHECK(c0_json_parse("{", 1) == NULL);
    CHECK(c0_json_parse("[1,]", 4) == NULL);
    CHECK(c0_json_parse("\"abc", 4) == NULL);
    CHECK(c0_json_parse("tru", 3) == NULL);
    CHECK(c0_json_parse("{} x", 4) == NULL);
    {
        c0_json_value *v = c0_json_parse(" [ ] ", 5);
        CHECK(v && v->kind == C0_JSON_ARRAY && v->n_items == 0);
        c0_json_free(v);
    }
}

int main(void) {
    test_kv_object();
    test_table();
    test_groups_and_file();
    test_nested();
    test_numbers_and_escapes();
    test_export_shapes();
    test_value_api();
    test_parse_errors();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all json tests passed\n");
    return 0;
}
