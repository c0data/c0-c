#define C0_IMPLEMENTATION
#include "../c0.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* Collect up to `cap` token types; returns count, or -1 on tokenizer error. */
static int collect(const uint8_t *buf, size_t len, c0_token_type *types, int cap) {
    c0_tokenizer tz;
    c0_token tok;
    c0_step s;
    int n = 0;
    c0_tokenizer_init(&tz, buf, len);
    while ((s = c0_tokenizer_next(&tz, &tok)) == C0_TOKEN) {
        if (n < cap) types[n] = tok.type;
        n++;
    }
    return s == C0_ERROR ? -1 : n;
}

static void test_tokenizer(void) {
    /* RS "create" US "a1b2" ETB (split literals so \x.. escapes don't eat
       the following hex-digit letters) */
    const uint8_t in[] = "\x1e" "create" "\x1f" "a1b2" "\x17";
    c0_token_type t[8];
    int n = collect(in, sizeof(in) - 1, t, 8);
    CHECK(n == 5);
    CHECK(t[0] == C0_TOK_RS);
    CHECK(t[1] == C0_TOK_DATA);
    CHECK(t[2] == C0_TOK_US);
    CHECK(t[3] == C0_TOK_DATA);
    CHECK(t[4] == C0_TOK_ETB);

    /* DLE-escaped US is data: RS, Data("a"), Data(0x1f), Data("b") */
    const uint8_t esc[] = "\x1e" "a" "\x10" "\x1f" "b";
    n = collect(esc, sizeof(esc) - 1, t, 8);
    CHECK(n == 4);
    CHECK(t[0] == C0_TOK_RS);

    /* Unassigned control code (BEL 0x07) is rejected */
    const uint8_t bad[] = "\x1e\x07";
    CHECK(collect(bad, sizeof(bad) - 1, t, 8) == -1);

    /* Dangling DLE is rejected */
    const uint8_t dangle[] = "\x1e" "a" "\x10";
    CHECK(collect(dangle, sizeof(dangle) - 1, t, 8) == -1);
}

static void test_unescape(void) {
    const uint8_t in[] = "a" "\x10" "\x1f" "b"; /* a, DLE, 0x1f, b */
    uint8_t out[8];
    size_t n;
    CHECK(c0_has_escape(in, sizeof(in) - 1) == 1);
    n = c0_unescape(in, sizeof(in) - 1, out);
    CHECK(n == 3);
    CHECK(out[0] == 'a' && out[1] == 0x1f && out[2] == 'b');

    const uint8_t clean[] = "abc";
    CHECK(c0_has_escape(clean, 3) == 0);
    CHECK(c0_unescape(clean, 3, out) == 3);
}

static void test_canonical(void) {
    CHECK(c0_canonical((const uint8_t *)"\x1e" "a" "\x10\x1e" "b", 5) == 1); /* necessary escape */
    CHECK(c0_canonical((const uint8_t *)"\x1e\x10" "A", 3) == 0);            /* gratuitous escape */
    CHECK(c0_canonical((const uint8_t *)"\x1e" "a\x04", 3) == 0);            /* EOT framing */
    CHECK(c0_canonical((const uint8_t *)"\x1e" "a\x17", 3) == 0);            /* ETB framing */
    CHECK(c0_canonical((const uint8_t *)"\x1e\x06", 2) == 0);                /* unassigned */
    CHECK(c0_canonical((const uint8_t *)"\x1e" "a\x10", 3) == 0);            /* dangling DLE */
}

static int beq(c0_bytes b, const char *s) {
    size_t n = strlen(s);
    return b.len == n && memcmp(b.ptr, s, n) == 0;
}

static int field_n(c0_bytes rec, int n, c0_bytes *out) {
    c0_field_iter fi = c0_record_fields(rec);
    int i = 0;
    c0_bytes f;
    while (c0_next_field(&fi, &f)) {
        if (i == n) {
            *out = f;
            return 1;
        }
        i++;
    }
    return 0;
}

static int field_count(c0_bytes rec) {
    c0_field_iter fi = c0_record_fields(rec);
    c0_bytes f;
    int n = 0;
    while (c0_next_field(&fi, &f)) n++;
    return n;
}

static void test_table_reader(void) {
    /* GS users SOH name US amount RS Alice US 100 RS Bob US 200 */
    const uint8_t in[] =
        "\x1d" "users" "\x01" "name" "\x1f" "amount"
        "\x1e" "Alice" "\x1f" "100" "\x1e" "Bob" "\x1f" "200";
    c0_group g = c0_table(in, sizeof(in) - 1);
    c0_iter hi, ri;
    c0_bytes h, rec, f;
    int n;

    CHECK(beq(c0_group_name(g), "users"));
    CHECK(c0_group_has_header(g));

    hi = c0_group_headers(g);
    CHECK(c0_next_header(&hi, &h) && beq(h, "name"));
    CHECK(c0_next_header(&hi, &h) && beq(h, "amount"));
    CHECK(!c0_next_header(&hi, &h));

    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &rec));
    CHECK(field_count(rec) == 2);
    CHECK(field_n(rec, 0, &f) && beq(f, "Alice"));
    CHECK(field_n(rec, 1, &f) && beq(f, "100"));
    CHECK(c0_next_record(&ri, &rec));
    CHECK(field_n(rec, 0, &f) && beq(f, "Bob"));
    CHECK(field_n(rec, 1, &f) && beq(f, "200"));
    CHECK(!c0_next_record(&ri, &rec));

    /* N separators => N+1 fields: trailing US yields an empty final field */
    {
        const uint8_t tr[] = "\x1e" "Alice" "\x1f";
        c0_group gt = c0_table(tr, sizeof(tr) - 1);
        c0_iter rt = c0_group_records(gt);
        CHECK(c0_next_record(&rt, &rec));
        n = field_count(rec);
        CHECK(n == 2);
    }
}

static void test_document_reader(void) {
    /* FS mydb GS users SOH name RS Alice GS products SOH id RS 01 */
    const uint8_t in[] =
        "\x1c" "mydb"
        "\x1d" "users" "\x01" "name" "\x1e" "Alice"
        "\x1d" "products" "\x01" "id" "\x1e" "01";
    c0_doc_iter di = c0_doc(in, sizeof(in) - 1);
    c0_group g;
    c0_bytes rec, f;
    c0_iter ri;

    CHECK(beq(c0_doc_name(in, sizeof(in) - 1), "mydb"));

    CHECK(c0_next_group(&di, &g));
    CHECK(beq(c0_group_name(g), "users"));
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &rec) && field_n(rec, 0, &f) && beq(f, "Alice"));

    CHECK(c0_next_group(&di, &g));
    CHECK(beq(c0_group_name(g), "products"));
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &rec) && field_n(rec, 0, &f) && beq(f, "01"));

    CHECK(!c0_next_group(&di, &g));
}

static void test_etb_tolerance(void) {
    /* GS g ETB SOH h ETB RS a ETB — framing tolerated, same structure */
    const uint8_t in[] = "\x1d" "g" "\x17" "\x01" "h" "\x17" "\x1e" "a" "\x17";
    c0_group g = c0_table(in, sizeof(in) - 1);
    c0_iter hi = c0_group_headers(g);
    c0_iter ri = c0_group_records(g);
    c0_bytes h, rec, f;

    CHECK(beq(c0_group_name(g), "g"));
    CHECK(c0_next_header(&hi, &h) && beq(h, "h"));
    CHECK(c0_next_record(&ri, &rec) && field_n(rec, 0, &f) && beq(f, "a"));
    CHECK(!c0_next_record(&ri, &rec));
}

static void test_builder(void) {
    c0_builder b;
    c0_bytes out, rec, f;
    c0_group g;
    c0_iter ri;

    c0_builder_init(&b);
    c0_build_group_str(&b, "users");
    {
        const char *hs[] = {"name", "amount"};
        c0_build_headers_str(&b, hs, 2);
    }
    {
        const char *r1[] = {"Alice", "1502.30"};
        const char *r2[] = {"Bob", "340.00"};
        c0_build_record_str(&b, r1, 2);
        c0_build_record_str(&b, r2, 2);
    }
    CHECK(c0_builder_status(&b) == C0_BUILD_OK);

    out = c0_builder_bytes(&b);
    CHECK(c0_canonical(out.ptr, out.len));
    g = c0_table(out.ptr, out.len);
    CHECK(beq(c0_group_name(g), "users"));
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &rec) && field_n(rec, 0, &f) && beq(f, "Alice"));
    CHECK(c0_next_record(&ri, &rec) && field_n(rec, 1, &f) && beq(f, "340.00"));
    c0_builder_free(&b);

    /* A field with a US byte is escaped on write and decodes back. */
    {
        c0_builder b2;
        c0_bytes fld[2], dec_in;
        uint8_t dec[8];
        size_t dn;
        c0_builder_init(&b2);
        c0_build_group_str(&b2, "g");
        fld[0].ptr = (const uint8_t *)"a" "\x1f" "b";
        fld[0].len = 3;
        fld[1].ptr = (const uint8_t *)"c";
        fld[1].len = 1;
        c0_build_record(&b2, fld, 2);
        out = c0_builder_bytes(&b2);
        g = c0_table(out.ptr, out.len);
        ri = c0_group_records(g);
        CHECK(c0_next_record(&ri, &rec));
        CHECK(field_count(rec) == 2);
        CHECK(field_n(rec, 0, &dec_in));
        dn = c0_unescape(dec_in.ptr, dec_in.len, dec);
        CHECK(dn == 3 && dec[0] == 'a' && dec[1] == 0x1f && dec[2] == 'b');
        c0_builder_free(&b2);
    }

    /* Names reject control bytes. */
    {
        c0_builder b3;
        c0_builder_init(&b3);
        c0_build_group(&b3, (const uint8_t *)"bad" "\x1f" "name", 8);
        CHECK(c0_builder_status(&b3) == C0_BUILD_BAD_NAME);
        c0_builder_free(&b3);
    }
}

static int count_records(c0_bytes committed) {
    c0_group g = c0_table(committed.ptr, committed.len);
    c0_iter ri = c0_group_records(g);
    c0_bytes rec;
    int n = 0;
    while (c0_next_record(&ri, &rec)) n++;
    return n;
}

static int count_blocks(const c0_stream *s) {
    c0_block_iter bi = c0_stream_blocks(s);
    c0_bytes blk;
    int n = 0;
    while (c0_next_block(&bi, &blk)) n++;
    return n;
}

static void test_stream(void) {
    /* two committed records */
    {
        const uint8_t in[] = "\x1e" "a" "\x17" "\x1e" "b" "\x17";
        c0_stream s = c0_stream_read(in, sizeof(in) - 1);
        CHECK(!s.torn);
        CHECK(count_blocks(&s) == 2);
        CHECK(count_records(c0_stream_committed(&s)) == 2);
    }
    /* torn tail: second record uncommitted, skipped */
    {
        const uint8_t t[] = "\x1e" "a" "\x17" "\x1e" "b";
        c0_stream s = c0_stream_read(t, sizeof(t) - 1);
        CHECK(s.torn);
        CHECK(c0_stream_committed(&s).len == 3); /* RS a ETB */
        CHECK(count_records(c0_stream_committed(&s)) == 1);
    }
    /* DLE-escaped ETB is data, not a commit */
    {
        const uint8_t e[] = "\x1e" "a" "\x10" "\x17";
        c0_stream s = c0_stream_read(e, sizeof(e) - 1);
        CHECK(s.torn);
        CHECK(s.committed_end == 0);
    }
    /* batch: two records under one commit => one block, two records */
    {
        const uint8_t bt[] = "\x1e" "a" "\x1e" "b" "\x17";
        c0_stream s = c0_stream_read(bt, sizeof(bt) - 1);
        CHECK(count_blocks(&s) == 1);
        CHECK(count_records(c0_stream_committed(&s)) == 2);
    }
}

int main(void) {
    test_tokenizer();
    test_unescape();
    test_canonical();
    test_table_reader();
    test_document_reader();
    test_etb_tolerance();
    test_builder();
    test_stream();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
