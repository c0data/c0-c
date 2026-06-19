/* test_csv.c — exercises the CSV ⇄ C0DATA converter. */
#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_csv.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* Compare a C0 buffer's first table back to expected CSV text. */
static void expect_csv(const uint8_t *buf, size_t len, const char *want, const char *msg) {
    size_t n = 0;
    char *got = c0_to_csv(buf, len, &n);
    CHECK(got != NULL, msg);
    if (got) {
        if (strcmp(got, want) != 0) {
            printf("FAIL: %s\n  want: %s\n  got:  %s\n", msg, want, got);
            failures++;
        }
        free(got);
    }
}

static void test_roundtrip(void) {
    const char *csv = "name,amount\nAlice,100\nBob,200\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "users", &blen);
    CHECK(buf != NULL && blen > 0, "from_csv produces output");
    /* A single group is GS-prefixed (a bare group, no FS file wrapper). */
    CHECK(buf && buf[0] == C0_GS, "from_csv emits a GS-prefixed group");
    if (buf) {
        expect_csv(buf, blen, csv, "roundtrip basic CSV");
        free(buf);
    }
}

static void test_default_group(void) {
    const char *csv = "a,b\n1,2\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), NULL, &blen);
    CHECK(buf != NULL, "from_csv with NULL group name");
    if (buf) {
        c0_doc_iter di = c0_doc(buf, blen);
        c0_group g;
        if (c0_next_group(&di, &g)) {
            c0_bytes nm = c0_group_name(g);
            CHECK(nm.len == 4 && memcmp(nm.ptr, "data", 4) == 0, "default group name is 'data'");
        } else {
            CHECK(0, "default-group document has a group");
        }
        free(buf);
    }
}

static void test_quoting(void) {
    /* Field with comma, quote, and newline must round-trip through quoting. */
    const char *csv = "h1,h2\n\"a,b\",\"c\"\"d\"\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "t", &blen);
    CHECK(buf != NULL, "from_csv with quoted fields");
    if (buf) {
        expect_csv(buf, blen, csv, "roundtrip quoted fields (comma + doubled quote)");
        free(buf);
    }
}

static void test_embedded_newline(void) {
    const char *csv = "h\n\"line1\nline2\"\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "t", &blen);
    CHECK(buf != NULL, "from_csv with embedded newline");
    if (buf) {
        expect_csv(buf, blen, csv, "roundtrip field containing newline");
        free(buf);
    }
}

static void test_crlf(void) {
    /* CRLF input should normalize to LF on output. */
    const char *csv = "a,b\r\n1,2\r\n";
    const char *want = "a,b\n1,2\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "t", &blen);
    CHECK(buf != NULL, "from_csv with CRLF");
    if (buf) {
        expect_csv(buf, blen, want, "CRLF normalized to LF");
        free(buf);
    }
}

static void test_no_trailing_newline(void) {
    const char *csv = "a,b\n1,2";   /* last row not newline-terminated */
    const char *want = "a,b\n1,2\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "t", &blen);
    CHECK(buf != NULL, "from_csv without trailing newline");
    if (buf) {
        expect_csv(buf, blen, want, "final row without trailing newline captured");
        free(buf);
    }
}

static void test_headers_only(void) {
    const char *csv = "a,b,c\n";
    size_t blen = 0;
    uint8_t *buf = c0_from_csv(csv, strlen(csv), "t", &blen);
    CHECK(buf != NULL, "from_csv headers-only");
    if (buf) {
        expect_csv(buf, blen, csv, "headers-only roundtrip");
        free(buf);
    }
}

static void test_empty(void) {
    size_t blen = 123;
    uint8_t *buf = c0_from_csv("", 0, "t", &blen);
    CHECK(buf == NULL && blen == 0, "empty CSV yields NULL output");
    if (buf) free(buf);
}

int main(void) {
    test_roundtrip();
    test_default_group();
    test_quoting();
    test_embedded_newline();
    test_crlf();
    test_no_trailing_newline();
    test_headers_only();
    test_empty();

    if (failures == 0) {
        printf("test_csv: all tests passed\n");
        return 0;
    }
    printf("test_csv: %d failure(s)\n", failures);
    return 1;
}
