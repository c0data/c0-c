/* test_json.c — exercises JSON <-> C0DATA conversion and the Value tree. */
#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_json.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static int streq(const char *a, const char *b) { return a && strcmp(a, b) == 0; }

/* JSON text -> C0DATA -> JSON text should be stable for stringly data. */
static void roundtrip(const char *json, const char *group, const char *msg) {
    size_t blen = 0, jlen = 0;
    uint8_t *buf = c0_from_json(json, strlen(json), group, &blen);
    char *back;
    CHECK(buf != NULL, msg);
    if (!buf) return;
    back = c0_to_json(buf, blen, &jlen);
    CHECK(back != NULL, msg);
    if (back) {
        if (!streq(back, json)) {
            printf("FAIL: %s\n  want: %s\n  got:  %s\n", msg, json, back);
            failures++;
        }
        free(back);
    }
    free(buf);
}

static void test_kv_object(void) {
    /* A flat object round-trips as a key-value group. */
    roundtrip("{\n  \"data\": {\n    \"name\": \"Alice\",\n    \"city\": \"NYC\"\n  }\n}",
              NULL, "flat kv object roundtrip");
}

static void test_table(void) {
    /* An array of uniform objects becomes a table. */
    const char *json =
        "{\n  \"users\": [\n    {\n      \"name\": \"Alice\",\n      \"age\": \"30\"\n    },\n"
        "    {\n      \"name\": \"Bob\",\n      \"age\": \"25\"\n    }\n  ]\n}";
    roundtrip(json, NULL, "table roundtrip");
}

static void test_numbers_become_strings(void) {
    /* Numbers/bools/null become strings (the documented lossy contract). */
    size_t blen = 0, jlen = 0;
    const char *json = "{\"n\": 42, \"b\": true, \"z\": null}";
    uint8_t *buf = c0_from_json(json, strlen(json), "g", &blen);
    char *back;
    CHECK(buf != NULL, "from_json with number/bool/null");
    if (!buf) return;
    back = c0_to_json(buf, blen, &jlen);
    CHECK(back != NULL, "to_json after numeric input");
    if (back) {
        /* 42 -> "42", true -> "true", null -> "" */
        CHECK(strstr(back, "\"42\"") != NULL, "number serialized as string");
        CHECK(strstr(back, "\"true\"") != NULL, "bool serialized as string");
        CHECK(strstr(back, "\"\"") != NULL, "null serialized as empty string");
        free(back);
    }
    free(buf);
}

static void test_nested(void) {
    /* Nested object as a field value round-trips through STX/ETX. */
    const char *json =
        "{\n  \"g\": {\n    \"item\": {\n      \"x\": \"1\",\n      \"y\": \"2\"\n    }\n  }\n}";
    roundtrip(json, NULL, "nested object field roundtrip");
}

static void test_value_api(void) {
    /* Build a Value tree by hand and emit it. */
    c0_value *obj = c0_value_object();
    c0_value *inner = c0_value_object();
    size_t blen = 0;
    uint8_t *buf;
    c0_value *back;
    char *js;
    c0_value_object_push(inner, "k", 1, c0_value_str("v", 1));
    c0_value_object_push(obj, "grp", 3, inner);
    buf = c0_from_value(obj, "data", &blen);
    CHECK(buf != NULL && blen > 0, "from_value emits bytes");
    if (buf) {
        back = c0_to_value(buf, blen);
        CHECK(back != NULL && back->kind == C0_JSON_OBJECT, "to_value yields an object");
        if (back) {
            js = c0_json_print(back, NULL);
            CHECK(js != NULL && strstr(js, "\"k\"") && strstr(js, "\"v\""), "value survives round-trip");
            free(js);
            c0_value_free(back);
        }
        free(buf);
    }
    c0_value_free(obj);
}

static void test_string_escapes(void) {
    /* Quote, backslash, newline, and a unicode escape decode then re-encode. */
    size_t blen = 0, jlen = 0;
    const char *json = "{\"k\": \"a\\\"b\\\\c\\nd\\u00e9\"}";
    uint8_t *buf = c0_from_json(json, strlen(json), "g", &blen);
    char *back;
    CHECK(buf != NULL, "from_json with escapes");
    if (!buf) return;
    back = c0_to_json(buf, blen, &jlen);
    CHECK(back != NULL, "to_json with escapes");
    if (back) {
        CHECK(strstr(back, "\\\"") != NULL, "quote re-escaped");
        CHECK(strstr(back, "\\n") != NULL, "newline re-escaped");
        /* é (U+00E9) is emitted as raw UTF-8, not \u */
        CHECK(strstr(back, "\xc3\xa9") != NULL, "unicode decoded to UTF-8");
        free(back);
    }
    free(buf);
}

static void test_parse_errors(void) {
    CHECK(c0_json_parse("{bad}", 5) == NULL, "reject malformed object");
    CHECK(c0_json_parse("[1,2", 4) == NULL, "reject unterminated array");
    CHECK(c0_json_parse("\"x\" trailing", 12) == NULL, "reject trailing junk");
    {
        c0_value *v = c0_json_parse("  [ ]  ", 7);
        CHECK(v != NULL && v->kind == C0_JSON_ARRAY && v->n_items == 0, "accept empty array with ws");
        c0_value_free(v);
    }
}

int main(void) {
    test_kv_object();
    test_table();
    test_numbers_become_strings();
    test_nested();
    test_value_api();
    test_string_escapes();
    test_parse_errors();
    if (failures == 0) { printf("test_json: all tests passed\n"); return 0; }
    printf("test_json: %d failure(s)\n", failures);
    return 1;
}
