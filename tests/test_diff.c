#define C0_IMPLEMENTATION
#include "../c0.h"
#include "../c0_diff.h"

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

#define S(lit) ((const uint8_t *)(lit)), (sizeof(lit) - 1)

static c0_diff_file_input in(const char *path, const char *content) {
    c0_diff_file_input f;
    f.path.ptr = (const uint8_t *)path;
    f.path.len = strlen(path);
    f.content.ptr = (const uint8_t *)content;
    f.content.len = strlen(content);
    return f;
}

static int out_is(const c0_diff_file_output *o, size_t n, size_t i, const char *path, const char *content) {
    return i < n && o[i].path_len == strlen(path) && memcmp(o[i].path, path, o[i].path_len) == 0 &&
           o[i].content_len == strlen(content) && memcmp(o[i].content, content, o[i].content_len) == 0;
}

static void test_build_parse(void) {
    c0_diff_builder b;
    c0_bytes got;
    c0_diff d;
    c0_diff_builder_init(&b, 1);
    c0_diff_file_begin_str(&b, "greeting.txt");
    c0_diff_section_begin(&b);
    c0_diff_anchor_str(&b, "Hello ");
    c0_diff_sub_str(&b, "world", "universe");
    c0_diff_anchor_str(&b, "!");
    c0_diff_file_begin_str(&b, "tally.rb");
    c0_diff_replace_all_str(&b, "cnt", "n");
    c0_diff_section_begin(&b);
    c0_diff_anchor_str(&b, "def tally");
    c0_diff_replace_all_str(&b, "count", "total");
    c0_diff_note_str(&b, "a note");
    c0_diff_eot(&b);
    got = c0_diff_builder_bytes(&b);
    {
        const uint8_t want[] = "\x16\x01" "shape" "\x1f" "diff" "\x1c" "greeting.txt" "\x1d\x02" "Hello " "\x03\x1f" "world" "\x1a" "universe" "\x1f" "!"
                               "\x1c" "tally.rb" "\x1e" "cnt" "\x1a" "n" "\x1d" "def tally" "\x1e" "count" "\x1a" "total" "\x07" "a note" "\x04";
        CHECK(got.len == sizeof(want) - 1 && memcmp(got.ptr, want, got.len) == 0);
    }
    CHECK(c0_validate(got.ptr, got.len, NULL) == C0_OK);
    d = c0_diff_parse(got.ptr, got.len);
    CHECK(d.status == C0_OK && d.nfiles == 2);
    if (d.status == C0_OK && d.nfiles == 2) {
        size_t n;
        uint8_t *s;
        CHECK(d.files[0].path_len == 12 && d.files[0].nsections == 1 && d.files[0].sections[0].nunits == 3);
        CHECK(d.files[0].sections[0].units[1].is_sub && !d.files[0].sections[0].units[0].is_sub);
        s = c0_diff_section_search(&d.files[0].sections[0], &n);
        CHECK(s && n == 12 && memcmp(s, "Hello world!", 12) == 0);
        free(s);
        s = c0_diff_section_replacement(&d.files[0].sections[0], &n);
        CHECK(s && n == 15 && memcmp(s, "Hello universe!", 15) == 0);
        free(s);
        CHECK(d.files[1].nleading == 1 && d.files[1].nsections == 1 && d.files[1].sections[0].nreplaces == 1);
    }
    c0_diff_free(&d);
    c0_diff_builder_free(&b);
}

static void test_apply(void) {
    c0_diff_builder b;
    c0_bytes diff;
    c0_diff_file_input files[2];
    c0_diff_file_output *out = NULL;
    size_t n = 0;
    c0_diff_error err;
    c0_diff_builder_init(&b, 1);
    c0_diff_file_begin_str(&b, "a.txt");
    c0_diff_replace_str(&b, "Hello ", "world", "universe", "!");
    c0_diff_file_begin_str(&b, "b.rb");
    c0_diff_section_begin(&b);
    c0_diff_anchor_str(&b, "def tally");
    c0_diff_replace_all_str(&b, "count", "total");
    c0_diff_section_begin(&b);
    c0_diff_anchor_str(&b, "end");
    c0_diff_eot(&b);
    diff = c0_diff_builder_bytes(&b);
    files[0] = in("a.txt", "Hello world!");
    files[1] = in("b.rb", "count\ndef tally\n  count += count\nend\ncount");
    CHECK(c0_diff_apply(diff.ptr, diff.len, files, 2, &out, &n, &err) == 1);
    CHECK(out_is(out, n, 0, "a.txt", "Hello universe!"));
    CHECK(out_is(out, n, 1, "b.rb", "count\ndef tally\n  total += total\nend\ncount"));
    c0_diff_free_outputs(out, n);
    c0_diff_builder_free(&b);
}

static void test_apply_errors(void) {
    c0_diff_file_input files[1];
    c0_diff_file_output *out = NULL;
    size_t n = 0;
    c0_diff_error err;
    const uint8_t missing[] = "\x1c" "nope" "\x1d" "x";
    const uint8_t ambiguous[] = "\x1c" "f" "\x1d" "a" "\x1a" "b";
    const uint8_t notfound[] = "\x1c" "f" "\x1d" "zzz" "\x1a" "b";
    const uint8_t badshape[] = "\x16\x01" "shape" "\x1f" "tabular" "\x1c" "f" "\x1d" "a" "\x1a" "b";
    const uint8_t dup[] = "\x1c" "f" "\x1c" "f";
    const uint8_t emptyold[] = "\x1c" "f" "\x1e\x1a" "b";
    const uint8_t rall[] = "\x1c" "f" "\x1e" "q" "\x1a" "b";
    const uint8_t sub_in_name[] = "\x1c" "f" "\x01" "k" "\x1f" "v";
    files[0] = in("f", "aaa");
    CHECK(c0_diff_apply(missing, sizeof(missing) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_FILE_NOT_FOUND);
    CHECK(strcmp(err.path, "nope") == 0);
    CHECK(c0_diff_apply(ambiguous, sizeof(ambiguous) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_PATTERN_AMBIGUOUS);
    CHECK(c0_diff_apply(notfound, sizeof(notfound) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_PATTERN_NOT_FOUND);
    CHECK(c0_diff_apply(badshape, sizeof(badshape) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_WRONG_SHAPE);
    CHECK(c0_diff_apply(dup, sizeof(dup) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_DUPLICATE_PATH);
    CHECK(c0_diff_apply(emptyold, sizeof(emptyold) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_SYNTAX);
    CHECK(c0_diff_apply(rall, sizeof(rall) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_REPLACE_NOT_FOUND);
    CHECK(c0_diff_apply(sub_in_name, sizeof(sub_in_name) - 1, files, 1, &out, &n, &err) == 0 && err.status == C0_DIFF_SYNTAX);
    /* a no-op diff passes files through */
    CHECK(c0_diff_apply(S(""), files, 1, &out, &n, &err) == 1 && out_is(out, n, 0, "f", "aaa"));
    c0_diff_free_outputs(out, n);
}

static void test_values_and_comments(void) {
    /* FS f GS STX "a " ETX US x SUB STX "\n" ETX BEL note RS b SUB STX "" ETX */
    const uint8_t diff[] = "\x1c" "f" "\x1d\x02" "a " "\x03\x1f" "x" "\x1a\x02" "\n" "\x03" "\x07" "note" "\x1e" "b" "\x1a";
    c0_diff_file_input files[1];
    c0_diff_file_output *out = NULL;
    size_t n = 0;
    c0_diff_error err;
    files[0] = in("f", "b a xb b");
    CHECK(c0_diff_apply(diff, sizeof(diff) - 1, files, 1, &out, &n, &err) == 1);
    CHECK(out_is(out, n, 0, "f", "b a \n "));
    c0_diff_free_outputs(out, n);
}

int main(void) {
    test_build_parse();
    test_apply();
    test_apply_errors();
    test_values_and_comments();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all diff tests passed\n");
    return 0;
}
