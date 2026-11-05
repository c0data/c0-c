#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_csv.h"

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

static void test_roundtrip(void) {
    const char *csv = "name,amount\nAlice,100\nBob,200\n";
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_csv(csv, strlen(csv), "users", &n);
    const char want[] = "\x1d" "users" "\x01\x1e" "name" "\x1f" "amount" "\x1e" "Alice" "\x1f" "100" "\x1e" "Bob" "\x1f" "200";
    char *back;
    CHECK(c0 && bytes_eq(c0, n, want, sizeof(want) - 1));
    CHECK(c0 && c0_canonical(c0, n));
    back = c0_to_csv(c0, n, NULL, &m, NULL);
    CHECK(back && strcmp(back, csv) == 0);
    free(back);
    back = c0_to_csv(c0, n, "users", &m, NULL);
    CHECK(back && strcmp(back, csv) == 0);
    free(back);
    free(c0);
}

static void test_default_group(void) {
    const char *csv = "a,b\n1,2\n";
    size_t n = 0;
    uint8_t *c0 = c0_from_csv(csv, strlen(csv), NULL, &n);
    CHECK(c0 && n > 5 && memcmp(c0, "\x1d" "data", 5) == 0);
    free(c0);
}

static void test_quoting(void) {
    const char *csv = "note,n\n\"a, b\",\"say \"\"hi\"\"\"\n";
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_csv(csv, strlen(csv), "t", &n);
    char *back = c0_to_csv(c0, n, NULL, &m, NULL);
    CHECK(back && strcmp(back, csv) == 0);
    free(back);
    free(c0);
}

static void test_embedded_newline(void) {
    /* a value with a line break becomes text in C0 and is quoted in CSV */
    const char *csv = "k\n\"line one\nline two\"\n";
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_csv(csv, strlen(csv), "t", &n);
    char *back;
    const char want[] = "\x1d" "t" "\x01\x1e" "k" "\x1e\x02" "line one\nline two" "\x03";
    CHECK(c0 && bytes_eq(c0, n, want, sizeof(want) - 1));
    back = c0_to_csv(c0, n, NULL, &m, NULL);
    CHECK(back && strcmp(back, csv) == 0);
    free(back);
    free(c0);
}

static void test_crlf_and_no_trailing_newline(void) {
    const char *csv = "a,b\r\n1,2\r\n3,4";
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_csv(csv, strlen(csv), "t", &n);
    char *back = c0_to_csv(c0, n, NULL, &m, NULL);
    CHECK(back && strcmp(back, "a,b\n1,2\n3,4\n") == 0);
    free(back);
    free(c0);
}

static void test_headers_only_and_empty(void) {
    size_t n = 0, m = 0;
    uint8_t *c0 = c0_from_csv("a,b\n", 4, "t", &n);
    char *back = c0_to_csv(c0, n, NULL, &m, NULL);
    CHECK(back && strcmp(back, "a,b\n") == 0);
    free(back);
    free(c0);
    CHECK(c0_from_csv("", 0, "t", &n) == NULL && n == 0);
}

static void test_refs_levels_and_errors(void) {
    /* GS g RS ENQ tags US ENQ STX tags US 001 ETX */
    const uint8_t in[] = "\x1d" "g" "\x1e\x05" "tags" "\x1f\x05\x02" "tags" "\x1f" "001" "\x03";
    size_t m = 0;
    c0_error err;
    char *back = c0_to_csv(in, sizeof(in) - 1, NULL, &m, NULL);
    CHECK(back && strcmp(back, "tags,tags/001\n") == 0);
    free(back);
    {
        const uint8_t lv[] = "\x1d" "g" "\x1e" "a" "\x1f\x10\x1e" "x" "\x18";
        CHECK(c0_to_csv(lv, sizeof(lv) - 1, NULL, &m, &err) == NULL && err.code == C0_ERR_SYNTAX);
    }
    CHECK(c0_to_csv(in, sizeof(in) - 1, "none", &m, &err) == NULL && err.code == C0_ERR_NOT_FOUND);
    {
        /* a document with SYN heading and a named file still exports its first group */
        const uint8_t doc[] = "\x16\x1c" "db" "\x1d" "g" "\x01\x1e" "k" "\x1e" "v" "\x04";
        back = c0_to_csv(doc, sizeof(doc) - 1, NULL, &m, NULL);
        CHECK(back && strcmp(back, "k\nv\n") == 0);
        free(back);
    }
}

int main(void) {
    test_roundtrip();
    test_default_group();
    test_quoting();
    test_embedded_newline();
    test_crlf_and_no_trailing_newline();
    test_headers_only_and_empty();
    test_refs_levels_and_errors();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all csv tests passed\n");
    return 0;
}
