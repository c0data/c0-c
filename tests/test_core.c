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

static int item_count(c0_bytes field) {
    c0_list_iter li = c0_field_items(field);
    c0_bytes item;
    int n = 0;
    while (c0_next_item(&li, &item)) n++;
    return n;
}

static void test_list_field(void) {
    c0_builder b;
    c0_bytes out, rec, f, item;
    c0_group g;
    c0_iter ri;
    c0_list_iter li;

    /* GS users RS Alice US STX Admin US Editor US User ETX US 1502.30 */
    c0_builder_init(&b);
    c0_build_group_str(&b, "users");
    {
        const char *r[] = {"Alice"};
        const char *roles[] = {"Admin", "Editor", "User"};
        c0_build_record_str(&b, r, 1);
        c0_build_list_field_str(&b, roles, 3);
        c0_build_field_str(&b, "1502.30");
    }
    CHECK(c0_builder_status(&b) == C0_BUILD_OK);
    out = c0_builder_bytes(&b);
    {
        const uint8_t want[] =
            "\x1d" "users" "\x1e" "Alice"
            "\x1f" "\x02" "Admin" "\x1f" "Editor" "\x1f" "User" "\x03"
            "\x1f" "1502.30";
        CHECK(out.len == sizeof(want) - 1 && memcmp(out.ptr, want, out.len) == 0);
    }
    CHECK(c0_canonical(out.ptr, out.len));

    g = c0_table(out.ptr, out.len);
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &rec));
    CHECK(field_count(rec) == 3);
    CHECK(field_n(rec, 0, &f) && beq(f, "Alice"));
    CHECK(field_n(rec, 2, &f) && beq(f, "1502.30"));
    CHECK(field_n(rec, 1, &f));
    li = c0_field_items(f);
    CHECK(c0_next_item(&li, &item) && beq(item, "Admin"));
    CHECK(c0_next_item(&li, &item) && beq(item, "Editor"));
    CHECK(c0_next_item(&li, &item) && beq(item, "User"));
    CHECK(!c0_next_item(&li, &item));
    CHECK(!c0_next_record(&ri, &rec));
    c0_builder_free(&b);

    /* Items with control bytes (US, STX) are escaped and round-trip. */
    {
        c0_builder b2;
        c0_bytes items[2];
        uint8_t dec[8];
        size_t dn;
        c0_builder_init(&b2);
        c0_build_group_str(&b2, "g");
        {
            const char *r[] = {"x"};
            c0_build_record_str(&b2, r, 1);
        }
        items[0].ptr = (const uint8_t *)"a" "\x1f" "b";
        items[0].len = 3;
        items[1].ptr = (const uint8_t *)"c" "\x02" "d";
        items[1].len = 3;
        c0_build_list_field(&b2, items, 2);
        CHECK(c0_builder_status(&b2) == C0_BUILD_OK);
        out = c0_builder_bytes(&b2);
        {
            const uint8_t want[] =
                "\x1d" "g" "\x1e" "x"
                "\x1f" "\x02" "a" "\x10\x1f" "b" "\x1f" "c" "\x10\x02" "d" "\x03";
            CHECK(out.len == sizeof(want) - 1 && memcmp(out.ptr, want, out.len) == 0);
        }
        g = c0_table(out.ptr, out.len);
        ri = c0_group_records(g);
        CHECK(c0_next_record(&ri, &rec));
        CHECK(field_count(rec) == 2);
        CHECK(field_n(rec, 1, &f));
        CHECK(item_count(f) == 2);
        li = c0_field_items(f);
        CHECK(c0_next_item(&li, &item));
        dn = c0_unescape(item.ptr, item.len, dec);
        CHECK(dn == 3 && dec[0] == 'a' && dec[1] == 0x1f && dec[2] == 'b');
        CHECK(c0_next_item(&li, &item));
        dn = c0_unescape(item.ptr, item.len, dec);
        CHECK(dn == 3 && dec[0] == 'c' && dec[1] == 0x02 && dec[2] == 'd');
        CHECK(!c0_next_item(&li, &item));
        c0_builder_free(&b2);
    }

    /* An empty list is US STX ETX and reads back as zero items. */
    {
        c0_builder b3;
        c0_builder_init(&b3);
        {
            const char *r[] = {"x"};
            c0_build_record_str(&b3, r, 1);
        }
        c0_build_list_field(&b3, NULL, 0);
        out = c0_builder_bytes(&b3);
        CHECK(out.len == 5 && memcmp(out.ptr, "\x1e" "x" "\x1f\x02\x03", 5) == 0);
        g = c0_table(out.ptr, out.len);
        ri = c0_group_records(g);
        CHECK(c0_next_record(&ri, &rec));
        CHECK(field_count(rec) == 2);
        CHECK(field_n(rec, 1, &f) && f.len == 2);
        CHECK(item_count(f) == 0);
        c0_builder_free(&b3);
    }

    /* A nested scope inside an item is kept intact (its US is not a split). */
    {
        const uint8_t in[] =
            "\x02" "a" "\x02" "x" "\x1f" "y" "\x03" "b" "\x1f" "c" "\x03";
        f.ptr = in;
        f.len = sizeof(in) - 1;
        CHECK(item_count(f) == 2);
        li = c0_field_items(f);
        CHECK(c0_next_item(&li, &item));
        CHECK(item.len == 7 && memcmp(item.ptr, "a" "\x02" "x" "\x1f" "y" "\x03" "b", 7) == 0);
        CHECK(c0_next_item(&li, &item) && beq(item, "c"));
        CHECK(!c0_next_item(&li, &item));
    }

    /* A plain (non-scope) field yields itself as a single item. */
    {
        f.ptr = (const uint8_t *)"Alice";
        f.len = 5;
        CHECK(item_count(f) == 1);
        li = c0_field_items(f);
        CHECK(c0_next_item(&li, &item) && beq(item, "Alice"));
        CHECK(!c0_next_item(&li, &item));

        f.len = 0; /* an empty plain field is one empty item */
        li = c0_field_items(f);
        CHECK(c0_next_item(&li, &item) && item.len == 0);
        CHECK(!c0_next_item(&li, &item));
    }

    /* A scope whose ETX is missing (malformed) still splits its items. */
    {
        const uint8_t in[] = "\x02" "p" "\x1f" "q";
        f.ptr = in;
        f.len = sizeof(in) - 1;
        li = c0_field_items(f);
        CHECK(c0_next_item(&li, &item) && beq(item, "p"));
        CHECK(c0_next_item(&li, &item) && beq(item, "q"));
        CHECK(!c0_next_item(&li, &item));
    }
}

static void test_builder_writers(void) {
    c0_builder b;
    c0_bytes out;

    /* Byte-level smoke test of each writer, in the order written. */
    c0_builder_init(&b);
    c0_build_file_str(&b, "db");
    c0_build_section_str(&b, "intro", 2);
    c0_build_block_str(&b, "line" "\x1e" "1");
    c0_build_item_str(&b, "it" "\x1f" "em");
    c0_build_group_str(&b, "orders");
    {
        const char *r[] = {"1"};
        c0_build_record_str(&b, r, 1);
    }
    c0_build_ref_str(&b, "users");
    {
        const char *path[] = {"users", "1", "name"};
        c0_build_ref_path_str(&b, path, 3);
    }
    c0_build_field_str(&b, "v" "\x02" "w");
    c0_build_nested_open(&b);
    c0_build_field_str(&b, "inner");
    c0_build_nested_close(&b);
    c0_build_etb_payload_str(&b, "a1b2");
    c0_build_etb(&b);
    c0_build_eot(&b);
    CHECK(c0_builder_status(&b) == C0_BUILD_OK);
    out = c0_builder_bytes(&b);
    {
        const uint8_t want[] =
            "\x1c" "db"
            "\x1d\x1d" "intro"
            "\x1e" "line" "\x10\x1e" "1"
            "\x1f" "it" "\x10\x1f" "em"
            "\x1d" "orders"
            "\x1e" "1"
            "\x05" "users"
            "\x05\x02" "users" "\x1f" "1" "\x1f" "name" "\x03"
            "\x1f" "v" "\x10\x02" "w"
            "\x02" "\x1f" "inner" "\x03"
            "\x17" "a1b2"
            "\x17"
            "\x04";
        CHECK(out.len == sizeof(want) - 1 && memcmp(out.ptr, want, out.len) == 0);
    }
    c0_builder_free(&b);

    /* (ptr,len) forms carry binary. */
    {
        c0_builder b2;
        c0_builder_init(&b2);
        c0_build_section(&b2, (const uint8_t *)"s", 1, 1);
        c0_build_block(&b2, (const uint8_t *)"\x00" "t", 2);
        c0_build_item(&b2, (const uint8_t *)"i", 1);
        c0_build_field(&b2, (const uint8_t *)"f", 1);
        c0_build_ref(&b2, (const uint8_t *)"r", 1);
        {
            c0_bytes segs[2];
            segs[0].ptr = (const uint8_t *)"g";
            segs[0].len = 1;
            segs[1].ptr = (const uint8_t *)"id";
            segs[1].len = 2;
            c0_build_ref_path(&b2, segs, 2);
        }
        c0_build_etb_payload(&b2, (const uint8_t *)"p", 1);
        CHECK(c0_builder_status(&b2) == C0_BUILD_OK);
        out = c0_builder_bytes(&b2);
        {
            const uint8_t want[] =
                "\x1d" "s" "\x1e" "\x10\x00" "t" "\x1f" "i" "\x1f" "f"
                "\x05" "r" "\x05\x02" "g" "\x1f" "id" "\x03" "\x17" "p";
            CHECK(out.len == sizeof(want) - 1 && memcmp(out.ptr, want, out.len) == 0);
        }
        c0_builder_free(&b2);
    }

    /* Section and ref names, and ref path segments, reject control bytes. */
    {
        c0_builder b3;
        c0_builder_init(&b3);
        c0_build_section_str(&b3, "a" "\x1f" "b", 1);
        CHECK(c0_builder_status(&b3) == C0_BUILD_BAD_NAME);
        c0_builder_free(&b3);

        c0_builder_init(&b3);
        c0_build_ref_str(&b3, "a" "\x1f" "b");
        CHECK(c0_builder_status(&b3) == C0_BUILD_BAD_NAME);
        c0_builder_free(&b3);

        c0_builder_init(&b3);
        {
            const char *path[] = {"ok", "b" "\x02" "ad"};
            c0_build_ref_path_str(&b3, path, 2);
        }
        CHECK(c0_builder_status(&b3) == C0_BUILD_BAD_NAME);
        c0_builder_free(&b3);
    }

    /* An ETB payload with a control byte sets C0_BUILD_BAD_PAYLOAD; later
       writes are no-ops. */
    {
        c0_builder b4;
        size_t before;
        c0_builder_init(&b4);
        c0_build_etb_payload_str(&b4, "a" "\x1f" "b");
        CHECK(c0_builder_status(&b4) == C0_BUILD_BAD_PAYLOAD);
        before = c0_builder_bytes(&b4).len;
        c0_build_eot(&b4);
        CHECK(c0_builder_bytes(&b4).len == before);
        c0_builder_free(&b4);
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

static void test_pretty(void) {
    c0_builder b;
    c0_bytes buf;
    char *p;
    size_t plen, blen;
    uint8_t *back;

    c0_builder_init(&b);
    c0_build_group_str(&b, "users");
    {
        const char *hs[] = {"name", "amount"};
        c0_build_headers_str(&b, hs, 2);
    }
    {
        const char *r[] = {"Alice", "100"};
        c0_build_record_str(&b, r, 2);
    }
    buf = c0_builder_bytes(&b);

    p = c0_pretty_format(buf.ptr, buf.len, NULL, &plen);
    CHECK(p != NULL);
    /* the RS glyph U+241E encodes as E2 90 9E */
    CHECK(strstr(p, "\xe2\x90\x9e") != NULL);
    back = c0_pretty_parse(p, plen, &blen);
    CHECK(back != NULL);
    CHECK(blen == buf.len && memcmp(back, buf.ptr, blen) == 0);
    free(p);
    free(back);
    c0_builder_free(&b);

    /* ETB renders (U+2417 => E2 90 97) and round-trips */
    {
        const uint8_t in[] = "\x1e" "create" "\x1f" "a1b2" "\x17";
        char *pp;
        uint8_t *bb;
        size_t pl, bl;
        pp = c0_pretty_format(in, sizeof(in) - 1, NULL, &pl);
        CHECK(pp != NULL && strstr(pp, "\xe2\x90\x97") != NULL);
        bb = c0_pretty_parse(pp, pl, &bl);
        CHECK(bb != NULL && bl == sizeof(in) - 1 && memcmp(bb, in, bl) == 0);
        free(pp);
        free(bb);
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
    test_list_field();
    test_builder_writers();
    test_stream();
    test_pretty();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
