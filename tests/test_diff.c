/* test_diff.c — exercises the C0DIFF parser, builder, and apply. */
#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_diff.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static int eq(const char *a, size_t alen, const char *b) {
    return alen == strlen(b) && memcmp(a, b, alen) == 0;
}

/* Build: FS "a.txt" GS "foo" SUB "bar" — replace foo with bar in a.txt. */
static void test_build_parse(void) {
    c0_diff_builder b;
    c0_bytes bytes;
    c0_diff d;
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "a.txt", 5);
    c0_diff_replace(&b, "", 0, "foo", 3, "bar", 3, "", 0);
    bytes = c0_diff_builder_bytes(&b);

    CHECK(bytes.len >= 1 && bytes.ptr[0] == C0_FS, "build emits FS first");

    d = c0_diff_parse(bytes.ptr, bytes.len);
    CHECK(!d.oom, "parse ok");
    CHECK(d.nfiles == 1, "one file");
    if (d.nfiles == 1) {
        c0_diff_file *f = &d.files[0];
        CHECK(eq((const char *)f->path, f->path_len, "a.txt"), "path is a.txt");
        CHECK(f->nsections == 1, "one section");
        if (f->nsections == 1) {
            size_t sl, rl;
            uint8_t *s = c0_diff_section_search(&f->sections[0], &sl);
            uint8_t *r = c0_diff_section_replace(&f->sections[0], &rl);
            CHECK(s && eq((const char *)s, sl, "foo"), "search pattern is foo");
            CHECK(r && eq((const char *)r, rl, "bar"), "replacement is bar");
            free(s); free(r);
        }
    }
    c0_diff_free(&d);
    c0_diff_builder_free(&b);
}

/* Anchored: context_before + sub + context_after. */
static void test_anchored(void) {
    c0_diff_builder b;
    c0_bytes bytes;
    c0_diff d;
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "f", 1);
    c0_diff_replace(&b, "ctx ", 4, "old", 3, "new", 3, " end", 4);
    bytes = c0_diff_builder_bytes(&b);
    d = c0_diff_parse(bytes.ptr, bytes.len);
    CHECK(d.nfiles == 1 && d.files[0].nsections == 1, "anchored parse shape");
    if (d.nfiles == 1 && d.files[0].nsections == 1) {
        size_t sl, rl;
        uint8_t *s = c0_diff_section_search(&d.files[0].sections[0], &sl);
        uint8_t *r = c0_diff_section_replace(&d.files[0].sections[0], &rl);
        CHECK(s && eq((const char *)s, sl, "ctx old end"), "search = anchor+old+anchor");
        CHECK(r && eq((const char *)r, rl, "ctx new end"), "replace = anchor+new+anchor");
        free(s); free(r);
    }
    c0_diff_free(&d);
    c0_diff_builder_free(&b);
}

static void test_apply(void) {
    c0_diff_builder b;
    c0_bytes bytes;
    c0_diff_file_input in[2];
    c0_diff_file_output *out = NULL;
    size_t ocount = 0, i;
    c0_diff_error err;
    int ok;

    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "greet.txt", 9);
    c0_diff_replace(&b, "", 0, "Hello", 5, "Goodbye", 7, "", 0);
    bytes = c0_diff_builder_bytes(&b);

    in[0].path = "greet.txt"; in[0].content = "Hello, world"; in[0].content_len = 12;
    in[1].path = "other.txt"; in[1].content = "untouched"; in[1].content_len = 9;

    ok = c0_diff_apply(bytes.ptr, bytes.len, in, 2, &out, &ocount, &err);
    CHECK(ok, "apply succeeds");
    CHECK(ocount == 2, "two output files");
    if (ok) {
        for (i = 0; i < ocount; i++) {
            if (strcmp(out[i].path, "greet.txt") == 0)
                CHECK(eq(out[i].content, out[i].content_len, "Goodbye, world"), "greet.txt edited");
            if (strcmp(out[i].path, "other.txt") == 0)
                CHECK(eq(out[i].content, out[i].content_len, "untouched"), "other.txt passed through");
        }
        c0_diff_free_outputs(out, ocount);
    }
    c0_diff_builder_free(&b);
}

static void test_apply_errors(void) {
    c0_diff_builder b;
    c0_bytes bytes;
    c0_diff_file_input in[1];
    c0_diff_file_output *out = NULL;
    size_t ocount = 0;
    c0_diff_error err;
    int ok;

    /* Pattern not found. */
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "f", 1);
    c0_diff_replace(&b, "", 0, "zzz", 3, "y", 1, "", 0);
    bytes = c0_diff_builder_bytes(&b);
    in[0].path = "f"; in[0].content = "abc"; in[0].content_len = 3;
    ok = c0_diff_apply(bytes.ptr, bytes.len, in, 1, &out, &ocount, &err);
    CHECK(!ok && err.status == C0_DIFF_PATTERN_NOT_FOUND, "pattern-not-found error");
    c0_diff_builder_free(&b);

    /* File not found. */
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "missing", 7);
    c0_diff_replace(&b, "", 0, "a", 1, "b", 1, "", 0);
    bytes = c0_diff_builder_bytes(&b);
    in[0].path = "f"; in[0].content = "abc"; in[0].content_len = 3;
    ok = c0_diff_apply(bytes.ptr, bytes.len, in, 1, &out, &ocount, &err);
    CHECK(!ok && err.status == C0_DIFF_FILE_NOT_FOUND, "file-not-found error");
    CHECK(strcmp(err.path, "missing") == 0, "error names the missing file");
    c0_diff_builder_free(&b);

    /* Ambiguous pattern (matches twice). */
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "f", 1);
    c0_diff_replace(&b, "", 0, "x", 1, "y", 1, "", 0);
    bytes = c0_diff_builder_bytes(&b);
    in[0].path = "f"; in[0].content = "xax"; in[0].content_len = 3;
    ok = c0_diff_apply(bytes.ptr, bytes.len, in, 1, &out, &ocount, &err);
    CHECK(!ok && err.status == C0_DIFF_PATTERN_AMBIGUOUS && err.count == 2, "ambiguous-pattern error");
    c0_diff_builder_free(&b);
}

/* Sectioned builder with multiple units, plus DLE escaping of a control byte. */
static void test_units_and_escape(void) {
    c0_diff_builder b;
    c0_bytes bytes;
    c0_diff d;
    const char withctl[] = {'a', 0x1f, 'b'}; /* contains US, must be escaped */
    c0_diff_builder_init(&b);
    c0_diff_file_begin(&b, "f", 1);
    c0_diff_section_begin(&b);
    c0_diff_anchor(&b, "lead", 4);
    c0_diff_sub(&b, withctl, 3, "z", 1);
    c0_diff_anchor(&b, "tail", 4);
    bytes = c0_diff_builder_bytes(&b);

    d = c0_diff_parse(bytes.ptr, bytes.len);
    CHECK(d.nfiles == 1 && d.files[0].nsections == 1, "sectioned parse shape");
    if (d.nfiles == 1 && d.files[0].nsections == 1) {
        size_t sl, rl;
        uint8_t *s = c0_diff_section_search(&d.files[0].sections[0], &sl);
        uint8_t *r = c0_diff_section_replace(&d.files[0].sections[0], &rl);
        /* search = "lead" + "a\x1fb" + "tail"; the US survives unescaping */
        CHECK(s && sl == 11 && memcmp(s, "lead", 4) == 0 && s[4] == 'a' && s[5] == 0x1f
              && s[6] == 'b' && memcmp(s + 7, "tail", 4) == 0, "search reconstructs escaped US");
        CHECK(r && eq((const char *)r, rl, "leadztail"), "replace concatenates new text");
        free(s); free(r);
    }
    c0_diff_free(&d);
    c0_diff_builder_free(&b);
}

int main(void) {
    test_build_parse();
    test_anchored();
    test_apply();
    test_apply_errors();
    test_units_and_escape();
    if (failures == 0) { printf("test_diff: all tests passed\n"); return 0; }
    printf("test_diff: %d failure(s)\n", failures);
    return 1;
}
