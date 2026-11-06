/* Unit tests for the single-header core: scanning, values, bodies,
 * documents, builder, stream, pretty. (The conformance vectors cover the
 * byte-level contract; these cover the API surface and its edge cases.) */
#define C0_IMPLEMENTATION
#include "../c0.h"

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

static int beq(c0_bytes b, const char *s) {
    size_t n = strlen(s);
    return b.len == n && (n == 0 || memcmp(b.ptr, s, n) == 0);
}

/* The decoded value of unit n of a record equals s. */
static int val_is(c0_record r, size_t n, const char *s) {
    c0_bytes raw, v;
    uint8_t scratch[256];
    if (c0_record_field(r, n, &raw) != C0_OK) return 0;
    if (raw.len > sizeof(scratch)) return 0;
    if (c0_value(raw, scratch, &v) != C0_OK) return 0;
    return beq(v, s);
}

static void test_helpers(void) {
    CHECK(c0_is_assigned(C0_SOH) && c0_is_assigned(C0_US) && c0_is_assigned(C0_BEL));
    CHECK(!c0_is_assigned(0x06) && !c0_is_assigned(0x0e) && !c0_is_assigned('a'));
    CHECK(c0_is_layout(' ') && c0_is_layout('\t') && c0_is_layout('\n') && c0_is_layout('\r'));
    CHECK(c0_is_text_escaped(C0_STX) && c0_is_text_escaped(C0_EM) && !c0_is_text_escaped(C0_US));
    CHECK(c0_is_plain(S("Alice Smith")));
    CHECK(!c0_is_plain(S(" Alice")) && !c0_is_plain(S("Alice ")) && !c0_is_plain(S("a\tb")));
    CHECK(c0_is_plain(S("")));
    CHECK(beq(c0_trim(c0_bytes_of(S("  x y \n"))), "x y"));
    {
        /* decoding removes the DLE before an escaped code */
        const uint8_t in[] = "a" "\x10\x17" "b";
        uint8_t out[8];
        CHECK(c0_decode_text(in, 4, out) == 3 && out[1] == C0_ETB);
    }
}

static void test_scan(void) {
    const uint8_t t[] = "\x02" "a" "\x1f" "b" "\x03" "rest";
    c0_error err;
    CHECK(c0_scan_text(t, 0, 9, &err) == 5 && err.code == C0_OK);
    CHECK(c0_scan_text(t, 0, 4, &err) == 4 && err.code == C0_ERR_UNEXPECTED_END);
    {
        const uint8_t lv[] = "\x10\x1e" "a" "\x02\x18\x03" "\x18" "x";
        CHECK(c0_scan_level(lv, 0, 8, &err) == 7 && err.code == C0_OK); /* CAN inside text is data */
    }
    {
        const uint8_t u[] = "  a b  \x1f" "c";
        CHECK(c0_scan_unit(u, 0, 9, &err) == 7);
        CHECK(c0_scan_layout(u, 0, 9) == 2);
        CHECK(c0_scan_plain(u, 2, 9) == 7);
    }
}

static void test_value(void) {
    c0_bytes v;
    uint8_t scratch[64];
    CHECK(c0_value(c0_bytes_of(S(" plain ")), NULL, &v) == C0_OK && beq(v, "plain"));
    CHECK(c0_value(c0_bytes_of(S("\x02 edge \x03")), NULL, &v) == C0_OK && beq(v, " edge "));
    CHECK(c0_value(c0_bytes_of(S("\x02" "a\x10\x17" "b\x03")), NULL, &v) == C0_ERR_NEEDS_DECODE);
    CHECK(c0_value(c0_bytes_of(S("\x02" "a\x10\x17" "b\x03")), scratch, &v) == C0_OK && v.len == 3 && v.ptr[1] == C0_ETB);
    CHECK(c0_value(c0_bytes_of(S("\x07note")), NULL, &v) == C0_OK && v.len == 0);
    CHECK(c0_value(c0_bytes_of(S("x\x07note")), NULL, &v) == C0_OK && beq(v, "x"));
    CHECK(c0_value(c0_bytes_of(S("\x10\x18")), NULL, &v) == C0_ERR_NOT_SCALAR);
    CHECK(c0_value(c0_bytes_of(S("\x05tags")), NULL, &v) == C0_ERR_NOT_SCALAR);
    CHECK(c0_value(c0_bytes_of(S("a\nb")), NULL, &v) == C0_ERR_SYNTAX);
    CHECK(c0_unit_type(c0_bytes_of(S(" \x02x\x03"))) == C0_UNIT_TEXT);
    CHECK(c0_unit_type(c0_bytes_of(S("\x10\x18"))) == C0_UNIT_LEVEL);
    CHECK(c0_unit_type(c0_bytes_of(S("\x05g"))) == C0_UNIT_REF);
    CHECK(c0_unit_type(c0_bytes_of(S("x"))) == C0_UNIT_PLAIN);
    {
        c0_ref r;
        CHECK(c0_unit_ref(c0_bytes_of(S("\x05tags")), &r) == C0_OK && r.count == 1 && beq(r.seg[0], "tags"));
        CHECK(c0_unit_ref(c0_bytes_of(S("\x05\x02tags\x1f" "001\x1flabel\x03")), &r) == C0_OK && r.count == 3 && beq(r.seg[2], "label"));
        CHECK(c0_unit_ref(c0_bytes_of(S("\x05\x02" "a\x1f\x1f" "b\x03")), &r) == C0_ERR_SYNTAX);
        CHECK(c0_unit_ref(c0_bytes_of(S("\x05\x02" "a\x1f" "b\x1f" "c\x1f" "d\x03")), &r) == C0_ERR_SYNTAX);
        CHECK(c0_unit_ref(c0_bytes_of(S("x")), &r) == C0_ERR_NOT_REFERENCE);
    }
}

static void test_body(void) {
    /* FS mydb GS users SOH RS name US amount RS Alice US 100 RS Bob US 200 GS GS sub RS p */
    const uint8_t in[] =
        "\x1c" "mydb" "\x1d" "users" "\x01\x1e" "name" "\x1f" "amount"
        "\x1e" "Alice" "\x1f" "100" "\x1e" "Bob" "\x1f" "200" "\x1d\x1d" "sub" "\x1e" "p";
    c0_body body = c0_body_view(in, sizeof(in) - 1);
    c0_file_iter fi = c0_body_files(body);
    c0_file f;
    c0_group_iter gi;
    c0_group g;
    c0_record h, r;
    c0_record_iter ri;
    size_t idx;

    CHECK(c0_validate(in, sizeof(in) - 1, NULL) == C0_OK);
    CHECK(c0_next_file(&fi, &f) && beq(c0_file_name(f), "mydb"));
    CHECK(!c0_next_file(&fi, &f));
    CHECK(!c0_body_bare(body));

    gi = c0_file_groups(f);
    CHECK(c0_next_group(&gi, &g) && beq(c0_group_name(g), "users") && g.depth == 1);
    CHECK(c0_group_header(g, &h) && c0_record_count(h) == 2 && val_is(h, 1, "amount"));
    CHECK(c0_group_column(g, S("amount"), &idx) == C0_OK && idx == 1);
    CHECK(c0_group_column(g, S("nope"), &idx) == C0_ERR_NOT_FOUND);
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &r) && !c0_record_keyed(r) && val_is(r, 0, "Alice") && val_is(r, 1, "100"));
    CHECK(c0_next_record(&ri, &r) && val_is(r, 0, "Bob"));
    CHECK(!c0_next_record(&ri, &r) && ri.err.code == C0_OK);
    CHECK(c0_next_group(&gi, &g) && beq(c0_group_name(g), "sub") && g.depth == 2);
    CHECK(!c0_group_header(g, &h));
    CHECK(!c0_next_group(&gi, &g));

    CHECK(c0_body_find_group_str(body, "sub", &g) == C0_OK && g.depth == 2);
    CHECK(c0_body_find_group_str(body, "none", &g) == C0_ERR_NOT_FOUND);
}

static void test_keyed_and_lists(void) {
    /* RS SOH host US localhost SOH port US 5432 SOH host US again */
    const uint8_t in[] = "\x1e\x01" "host" "\x1f" "localhost" "\x01" "port" "\x1f" "5432" "\x01" "host" "\x1f" "again";
    c0_body body = c0_body_view(in, sizeof(in) - 1);
    c0_record_iter ri = c0_body_records(body);
    c0_record r;
    c0_pair_iter pi;
    c0_bytes k, v;
    CHECK(c0_body_bare(body));
    CHECK(c0_next_record(&ri, &r) && c0_record_keyed(r));
    CHECK(c0_record_count(r) == 0); /* no positional units */
    pi = c0_record_pairs(r);
    CHECK(c0_next_pair(&pi, &k, &v) && beq(k, "host") && beq(v, "localhost"));
    CHECK(c0_next_pair(&pi, &k, &v) && beq(k, "port"));
    CHECK(c0_next_pair(&pi, &k, &v) && beq(k, "host") && beq(v, "again"));
    CHECK(!c0_next_pair(&pi, &k, &v));
    CHECK(c0_record_get_str(r, "host", &v) == C0_OK && beq(v, "again")); /* last wins */
    CHECK(c0_record_get_str(r, "nope", &v) == C0_ERR_NOT_FOUND);
    CHECK(!c0_canonical(in, sizeof(in) - 1)); /* duplicate key, not adjacent */
    {
        /* RS a US DLE RS x US y CAN US DLE CAN US DLE SOH RS h RS 1 CAN */
        const uint8_t lst[] = "\x1e" "a" "\x1f\x10\x1e" "x" "\x1f" "y" "\x18" "\x1f\x10\x18" "\x1f\x10\x01\x1e" "h" "\x1e" "1" "\x18";
        c0_body b2 = c0_body_view(lst, sizeof(lst) - 1);
        c0_record_iter ri2 = c0_body_records(b2);
        c0_unit_iter items;
        c0_bytes raw, item;
        c0_body lv;
        CHECK(c0_validate(lst, sizeof(lst) - 1, NULL) == C0_OK);
        CHECK(c0_next_record(&ri2, &r) && c0_record_count(r) == 4);
        CHECK(c0_record_field(r, 1, &raw) == C0_OK && c0_unit_list(raw, &items) == C0_OK);
        CHECK(c0_next_unit(&items, &item) && beq(item, "x"));
        CHECK(c0_next_unit(&items, &item) && beq(item, "y"));
        CHECK(!c0_next_unit(&items, &item));
        CHECK(c0_record_field(r, 2, &raw) == C0_OK && c0_unit_list(raw, &items) == C0_OK && !c0_next_unit(&items, &item));
        CHECK(c0_record_field(r, 3, &raw) == C0_OK && c0_unit_list(raw, &items) == C0_ERR_NOT_LIST); /* a header row */
        CHECK(c0_unit_level(raw, &lv) == C0_OK);
        CHECK(c0_record_field(r, 0, &raw) == C0_OK && c0_unit_list(raw, &items) == C0_ERR_NOT_LEVEL);
    }
}

static void test_comments(void) {
    /* GS g BEL note RS a BEL RS dead RS b US BEL n US c BEL FS f RS d */
    const uint8_t in[] = "\x1d" "g" "\x07" "note" "\x1e" "a" "\x07\x1e" "dead" "\x1e" "b" "\x1f\x07" "n" "\x1f" "c" "\x07\x1c" "f" "\x1e" "d";
    c0_body body = c0_body_view(in, sizeof(in) - 1);
    c0_group_iter gi = c0_body_groups(body);
    c0_group g;
    c0_record_iter ri;
    c0_record r;
    c0_file_iter fi = c0_body_files(body);
    c0_file f;
    CHECK(c0_validate(in, sizeof(in) - 1, NULL) == C0_OK);
    CHECK(!c0_canonical(in, sizeof(in) - 1));
    CHECK(c0_next_file(&fi, &f) && !c0_next_file(&fi, &f)); /* the dead FS line is not a boundary */
    CHECK(c0_next_group(&gi, &g));
    ri = c0_group_records(g);
    CHECK(c0_next_record(&ri, &r) && val_is(r, 0, "a"));
    CHECK(c0_next_record(&ri, &r) && c0_record_count(r) == 3 && val_is(r, 1, "") && val_is(r, 2, "c"));
    CHECK(c0_next_record(&ri, &r) && val_is(r, 0, "d")); /* stays in the group before the dead FS */
    CHECK(!c0_next_record(&ri, &r));
    CHECK(!c0_next_group(&gi, &g));
}

static void test_document(void) {
    /* SYN SOH shape US tabular SOH x US 1 GS g RS a EOT SYN GS g RS b EOT EM raw */
    const uint8_t in[] = "\x16\x01" "shape" "\x1f" "tabular" "\x01" "x" "\x1f" "1" "\x1d" "g" "\x1e" "a" "\x04"
                         "\x16\x1d" "g" "\x1e" "b" "\x04" "\x19" "raw";
    c0_doc_iter di = c0_documents(in, sizeof(in) - 1);
    c0_document d;
    c0_bytes v, tail;
    int has_em;
    CHECK(c0_validate(in, sizeof(in) - 1, NULL) == C0_OK);
    CHECK(c0_next_document(&di, &d) && d.has_syn && d.has_eot);
    CHECK(beq(c0_heading_shape(&d), "tabular"));
    CHECK(c0_heading_version(&d) == 1);
    CHECK(c0_heading_get_str(&d, "x", &v) == C0_OK && beq(v, "1"));
    CHECK((d.warnings & C0_WARN_UNKNOWN_KEY) && !(d.warnings & C0_WARN_NO_SYN) && !(d.warnings & C0_WARN_NO_EOT));
    CHECK(c0_next_document(&di, &d) && d.has_syn);
    CHECK(!c0_next_document(&di, &d) && di.has_em && beq(c0_doc_iter_tail(&di), "raw"));
    CHECK(c0_container_tail(in, sizeof(in) - 1, &tail, &has_em, NULL) == C0_OK && has_em && beq(tail, "raw"));
    {
        const uint8_t bare[] = "\x1e" "a";
        CHECK(c0_document_read(bare, 2, 0, &d, NULL) == C0_OK);
        CHECK((d.warnings & C0_WARN_NO_SYN) && (d.warnings & C0_WARN_NO_EOT) && !d.has_syn);
        CHECK(d.body.start == 0 && d.body.stop == 2);
    }
    {
        const uint8_t bad[] = "\x16\x1e" "a" "\x04" "x";
        CHECK(c0_documents(bad, 5).err.code == C0_OK);
        di = c0_documents(bad, 5);
        CHECK(c0_next_document(&di, &d) == 0 && di.err.code == C0_ERR_SYNTAX);
    }
}

static void test_blob(void) {
    /* SYN SOH blob US DLE SOH RS name US offset US length RS img US 1 US 2 CAN GS g RS a EOT EM xABy */
    const uint8_t in[] = "\x16\x01" "blob" "\x1f\x10\x01\x1e" "name" "\x1f" "offset" "\x1f" "length"
                         "\x1e" "img" "\x1f" "1" "\x1f" "2" "\x18" "\x1d" "g" "\x1e" "a" "\x04" "\x19" "xABy";
    c0_bytes b;
    CHECK(c0_blob_str(in, sizeof(in) - 1, "img", &b, NULL) == C0_OK && beq(b, "AB"));
    CHECK(c0_blob_str(in, sizeof(in) - 1, "none", &b, NULL) == C0_ERR_NOT_FOUND);
}

static void test_builder(void) {
    c0_builder b;
    c0_bytes got;
    const char *hdr[] = {"name", "amount"};
    const char *row[] = {"Alice", "100"};
    const char *keys[] = {"host", "port"};
    const char *vals[] = {"localhost", "5432"};
    const char *items[] = {"Admin", "Editor"};
    const char *path[] = {"tags", "001"};
    c0_builder_init(&b);
    c0_build_syn(&b);
    c0_build_key_str(&b, "shape", "tabular");
    c0_build_file_str(&b, "db");
    c0_build_group_str(&b, "users");
    c0_build_header_str(&b, hdr, 2);
    c0_build_record_str(&b, row, 2);
    c0_build_field_str(&b, " edge ");
    c0_build_map_str(&b, keys, vals, 2);
    c0_build_record(&b, NULL, 0);
    c0_build_list_str(&b, items, 2);
    c0_build_ref_str(&b, "tags");
    c0_build_ref_path_str(&b, path, 2);
    c0_build_level_open(&b);
    c0_build_record_str(&b, row, 1);
    c0_build_level_close(&b);
    c0_build_field_str(&b, "after");
    c0_build_note_str(&b, "a note");
    c0_build_etb_str(&b, "sha256:00");
    c0_build_eot(&b);
    CHECK(c0_builder_status(&b) == C0_OK);
    got = c0_builder_bytes(&b);
    {
        const uint8_t want[] =
            "\x16\x01" "shape" "\x1f" "tabular" "\x1c" "db" "\x1d" "users" "\x01\x1e" "name" "\x1f" "amount"
            "\x1e" "Alice" "\x1f" "100" "\x1f\x02" " edge " "\x03"
            "\x1e\x01" "host" "\x1f" "localhost" "\x01" "port" "\x1f" "5432"
            "\x1e\x10\x1e" "Admin" "\x1f" "Editor" "\x18" "\x1f\x05" "tags" "\x1f\x05\x02" "tags" "\x1f" "001" "\x03"
            "\x1f\x10\x1e" "Alice" "\x18" "\x1f" "after" "\x07" "a note" "\x17" "sha256:00" "\x04";
        CHECK(got.len == sizeof(want) - 1 && memcmp(got.ptr, want, got.len) == 0);
        CHECK(c0_validate(got.ptr, got.len, NULL) == C0_OK);
    }
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_build_group_str(&b, "bad\x1fname");
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_NAME);
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_build_level_close(&b);
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_STATE);
    c0_builder_free(&b);

    /* a value that needs text: control bytes escaped only when they are one of the seven */
    c0_builder_init(&b);
    c0_build_field(&b, S("a\x1f" "b\x17" "c"));
    got = c0_builder_bytes(&b);
    CHECK(got.len == 9 && memcmp(got.ptr, "\x1e\x02" "a\x1f" "b\x10\x17" "c\x03", 9) == 0);
    CHECK(c0_canonical(got.ptr, got.len));
    c0_builder_free(&b);
}

/* The builder's bytes equal the pretty-form glyph string `want`. */
static int built_is(const c0_builder *b, const char *want) {
    size_t n;
    uint8_t *w = c0_pretty_parse(want, strlen(want), &n);
    c0_bytes got = c0_builder_bytes(b);
    int ok = w != NULL && got.len == n && (n == 0 || memcmp(got.ptr, w, n) == 0);
    if (!ok) {
        char *s = c0_pretty_format(got.ptr, got.len, NULL, C0_PRETTY_COMPACT, &n, NULL);
        printf("  want %s\n  got  %s", want, s ? s : "(unformattable)\n");
        free(s);
    }
    free(w);
    return ok;
}

/* The primitives: one function per code, spelled as the bytes read; they
 * share the record state with the composed functions, so the two mix. */
static void test_builder_primitives(void) {
    c0_builder b, lo;

    /* refuses an empty note, through either layer */
    c0_builder_init(&b);
    c0_build_record_str(&b, NULL, 0);
    c0_build_note_str(&b, "");
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_STATE);
    c0_builder_free(&b);
    c0_builder_init(&b);
    c0_builder_rs_str(&b, "a");
    c0_builder_bel_str(&b, "");
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_STATE);
    c0_builder_free(&b);

    /* spells primitives as the bytes read */
    c0_builder_init(&b);
    c0_builder_fs_str(&b, "inv"); c0_builder_gs_str(&b, "users"); c0_builder_soh(&b);
    c0_builder_rs_str(&b, "name"); c0_builder_us_str(&b, "amount");
    CHECK(built_is(&b, "\xe2\x90\x9c" "inv" "\xe2\x90\x9d" "users" "\xe2\x90\x81\xe2\x90\x9e" "name" "\xe2\x90\x9f" "amount"));
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_builder_rs_str(&b, "Alice"); c0_builder_us_str(&b, "1502.30");
    CHECK(built_is(&b, "\xe2\x90\x9e" "Alice" "\xe2\x90\x9f" "1502.30"));
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_builder_rs(&b);
    c0_builder_soh_str(&b, "host"); c0_builder_us_str(&b, "localhost");
    c0_builder_soh_str(&b, "port"); c0_builder_us_str(&b, "5432");
    CHECK(built_is(&b, "\xe2\x90\x9e\xe2\x90\x81" "host" "\xe2\x90\x9f" "localhost" "\xe2\x90\x81" "port" "\xe2\x90\x9f" "5432"));
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_builder_rs_str(&b, "cfg"); c0_builder_us(&b); c0_builder_dle(&b);
    c0_builder_rs(&b); c0_builder_soh_str(&b, "b"); c0_builder_us_str(&b, "2"); c0_builder_can(&b);
    CHECK(built_is(&b, "\xe2\x90\x9e" "cfg" "\xe2\x90\x9f\xe2\x90\x90\xe2\x90\x9e\xe2\x90\x81" "b" "\xe2\x90\x9f" "2" "\xe2\x90\x98"));
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_builder_bel_str(&b, "checked"); c0_builder_rs_str(&b, "x");
    CHECK(built_is(&b, "\xe2\x90\x87" "checked" "\xe2\x90\x9e" "x"));
    c0_builder_free(&b);

    c0_builder_init(&b);
    c0_builder_bel(&b); c0_builder_gs_str(&b, "old"); c0_builder_rs_str(&b, "1");
    c0_builder_gs_str(&b, "new"); c0_builder_rs_str(&b, "2");
    CHECK(built_is(&b, "\xe2\x90\x87\xe2\x90\x9d" "old" "\xe2\x90\x9e" "1" "\xe2\x90\x9d" "new" "\xe2\x90\x9e" "2"));
    c0_builder_free(&b);

    {
        const char *path[] = {"users", "1"};
        c0_builder_init(&b);
        c0_builder_gs(&b); c0_builder_gs_str(&b, "sub"); c0_builder_rs(&b);
        c0_builder_enq_str(&b, "users"); c0_builder_us(&b); c0_builder_enq_path_str(&b, path, 2);
        CHECK(built_is(&b, "\xe2\x90\x9d\xe2\x90\x9d" "sub" "\xe2\x90\x9e\xe2\x90\x85" "users" "\xe2\x90\x9f\xe2\x90\x85\xe2\x90\x82" "users" "\xe2\x90\x9f" "1" "\xe2\x90\x83"));
        c0_builder_free(&b);
    }

    c0_builder_init(&b);
    c0_builder_syn(&b); c0_builder_soh_str(&b, "version"); c0_builder_us_str(&b, "1");
    c0_builder_rs_str(&b, "a"); c0_builder_eot(&b);
    CHECK(built_is(&b, "\xe2\x90\x96\xe2\x90\x81" "version" "\xe2\x90\x9f" "1" "\xe2\x90\x9e" "a" "\xe2\x90\x84"));
    c0_builder_free(&b);

    /* etb bare and with a payload; em and a raw tail (pretty form cannot show
     * a tail, so this one is checked as bytes); values keep the one-spelling rule */
    c0_builder_init(&b);
    c0_builder_rs_str(&b, " a "); c0_builder_etb(&b); c0_builder_rs_str(&b, "b"); c0_builder_etb_str(&b, "sha:1");
    c0_builder_em_str(&b, "raw\x1e" "tail");
    {
        const uint8_t want[] = "\x1e\x02" " a " "\x03\x17\x1e" "b" "\x17" "sha:1" "\x19" "raw\x1e" "tail";
        c0_bytes got = c0_builder_bytes(&b);
        CHECK(c0_builder_status(&b) == C0_OK);
        CHECK(got.len == sizeof(want) - 1 && memcmp(got.ptr, want, got.len) == 0);
    }
    c0_builder_free(&b);

    /* mixes primitives with composed functions */
    {
        const char *h[] = {"h"};
        c0_builder_init(&b);
        c0_builder_rs_str(&b, "a");
        c0_build_field_str(&b, "b");
        c0_build_record(&b, NULL, 0);
        c0_builder_us_str(&b, "c");
        c0_builder_gs_str(&b, "g");
        c0_build_key_str(&b, "k", "v");
        c0_build_group_str(&b, "h");
        c0_build_header_str(&b, h, 1);
        c0_build_field_str(&b, "x");
        CHECK(c0_builder_status(&b) == C0_OK);
        CHECK(built_is(&b, "\xe2\x90\x9e" "a" "\xe2\x90\x9f" "b" "\xe2\x90\x9e\xe2\x90\x9f" "c" "\xe2\x90\x9d" "g" "\xe2\x90\x9e\xe2\x90\x81" "k" "\xe2\x90\x9f" "v"
                            "\xe2\x90\x9d" "h" "\xe2\x90\x81\xe2\x90\x9e" "h" "\xe2\x90\x9e" "x"));
        c0_builder_free(&b);
    }

    /* composed level after a primitive record; primitive us after a composed key */
    c0_builder_init(&b);
    c0_builder_rs_str(&b, "a");
    c0_build_level_open(&b); c0_builder_rs_str(&b, "in"); c0_build_level_close(&b);
    c0_build_key_str(&b, "k", "v"); c0_builder_us_str(&b, "w");
    CHECK(built_is(&b, "\xe2\x90\x9e" "a" "\xe2\x90\x9f\xe2\x90\x90\xe2\x90\x9e" "in" "\xe2\x90\x98\xe2\x90\x81" "k" "\xe2\x90\x9f" "v" "\xe2\x90\x9f" "w"));
    c0_builder_free(&b);

    /* a CAN with no open level; a reference of four segments */
    c0_builder_init(&b);
    c0_builder_can(&b);
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_STATE);
    c0_builder_free(&b);
    {
        const char *four[] = {"a", "b", "c", "d"};
        c0_builder_init(&b);
        c0_builder_rs(&b);
        c0_builder_enq_path_str(&b, four, 4);
        CHECK(c0_builder_status(&b) == C0_ERR_BAD_NAME);
        c0_builder_free(&b);
    }

    /* names are validated in the primitives too */
    c0_builder_init(&b);
    c0_builder_soh_str(&b, "bad key");
    CHECK(c0_builder_status(&b) == C0_OK); /* interior space is a plain name */
    c0_builder_soh_str(&b, " edge");
    CHECK(c0_builder_status(&b) == C0_ERR_BAD_NAME);
    c0_builder_free(&b);

    /* writes the same bytes through either layer */
    {
        const char *hdr[] = {"name", "amount"};
        const char *alice[] = {"Alice", "1502.30"}, *bob[] = {"Bob", "3 40"};
        const char *keys[] = {"host", "port"}, *vals[] = {"localhost", "5432"};
        c0_bytes hb, lb;
        c0_builder_init(&b);
        c0_build_syn(&b);
        c0_build_key_str(&b, "shape", "tabular");
        c0_build_group_str(&b, "users");
        c0_build_header_str(&b, hdr, 2);
        c0_build_record_str(&b, alice, 2);
        c0_build_record_str(&b, bob, 2);
        c0_build_group_str(&b, "cfg");
        c0_build_map_str(&b, keys, vals, 2);
        c0_build_eot(&b);

        c0_builder_init(&lo);
        c0_builder_syn(&lo); c0_builder_soh_str(&lo, "shape"); c0_builder_us_str(&lo, "tabular");
        c0_builder_gs_str(&lo, "users"); c0_builder_soh(&lo); c0_builder_rs_str(&lo, "name"); c0_builder_us_str(&lo, "amount");
        c0_builder_rs_str(&lo, "Alice"); c0_builder_us_str(&lo, "1502.30");
        c0_builder_rs_str(&lo, "Bob"); c0_builder_us_str(&lo, "3 40");
        c0_builder_gs_str(&lo, "cfg"); c0_builder_rs(&lo);
        c0_builder_soh_str(&lo, "host"); c0_builder_us_str(&lo, "localhost");
        c0_builder_soh_str(&lo, "port"); c0_builder_us_str(&lo, "5432");
        c0_builder_eot(&lo);

        CHECK(c0_builder_status(&b) == C0_OK && c0_builder_status(&lo) == C0_OK);
        hb = c0_builder_bytes(&b);
        lb = c0_builder_bytes(&lo);
        CHECK(hb.len == lb.len && memcmp(hb.ptr, lb.ptr, hb.len) == 0);
        CHECK(c0_validate(lb.ptr, lb.len, NULL) == C0_OK);
        {
            c0_document doc;
            CHECK(c0_document_read(lb.ptr, lb.len, 0, &doc, NULL) == C0_OK);
            CHECK(c0_canonical(doc.body.buf + doc.body.start, doc.body.stop - doc.body.start));
        }
        c0_builder_free(&b);
        c0_builder_free(&lo);
    }
}

static void test_stream(void) {
    const uint8_t in[] = "\x1e" "a" "\x17" "sha:1" "\x1e\x02" "b" "\x17" "\x1e" "c" "\x17" "\x1e" "d";
    c0_stream s = c0_stream_read(in, sizeof(in) - 1);
    c0_block_iter bi = c0_stream_blocks(&s);
    c0_bytes blk;
    int dmg;
    uint8_t out[32];
    CHECK(s.block_count == 3 && s.damaged_count == 1 && s.torn);
    CHECK(s.committed_end == 15);
    CHECK(c0_next_block(&bi, &blk, &dmg) && beq(blk, "\x1e" "a") && !dmg);
    CHECK(c0_next_block(&bi, &blk, &dmg) && dmg);
    CHECK(c0_next_block(&bi, &blk, &dmg) && beq(blk, "\x1e" "c") && !dmg);
    CHECK(!c0_next_block(&bi, &blk, &dmg));
    CHECK(beq(c0_stream_tail(&s), "\x1e" "d"));
    CHECK(c0_stream_undamaged(&s, out) == 4 && memcmp(out, "\x1e" "a\x1e" "c", 4) == 0);
    CHECK(c0_stream_last_commit_end(in, sizeof(in) - 1) == 15);
    {
        const uint8_t esc[] = "\x1e\x02" "a\x10\x17" "b\x03";
        CHECK(c0_stream_last_commit_end(esc, sizeof(esc) - 1) == 0);
        s = c0_stream_read(esc, sizeof(esc) - 1);
        CHECK(s.block_count == 0 && s.torn);
    }
    {
        const uint8_t em[] = "\x1e" "a" "\x17" "\x19" "raw";
        s = c0_stream_read(em, sizeof(em) - 1);
        CHECK(!s.torn); /* the raw region is not data */
    }
}

static void test_pretty(void) {
    const uint8_t in[] = "\x16\x01" "shape" "\x1f" "tabular" "\x1d" "users" "\x01\x1e" "name" "\x1f" "amount"
                         "\x1e" "Alice Smith" "\x1f" "100" "\x1e" "Bob" "\x1f\x10\x1e" "x" "\x1f" "y" "\x18" "\x1f\x02" "a\nb" "\x03" "\x04";
    size_t n, m;
    char *s = c0_pretty_format(in, sizeof(in) - 1, NULL, C0_PRETTY_COMPACT, &n, NULL);
    uint8_t *back;
    CHECK(s != NULL);
    if (s) {
        const char *want =
            "\xe2\x90\x96" "\xe2\x90\x81" "shape" "\xe2\x90\x9f" "tabular\n"
            "\xe2\x90\x9d" "users\n"
            "  " "\xe2\x90\x81" "\xe2\x90\x9e" "name" "\xe2\x90\x9f" "amount\n"
            "  " "\xe2\x90\x9e" "Alice Smith" "\xe2\x90\x9f" "100\n"
            "  " "\xe2\x90\x9e" "Bob" "\xe2\x90\x9f" "\xe2\x90\x90" "\n"
            "    " "\xe2\x90\x9e" "x" "\xe2\x90\x9f" "y\n"
            "  " "\xe2\x90\x98" "\xe2\x90\x9f" "\xe2\x90\x82" "a\nb" "\xe2\x90\x83" "\n"
            "\xe2\x90\x84" "\n";
        CHECK(strcmp(s, want) == 0);
        if (strcmp(s, want) != 0) printf("got:\n%s", s);
        back = c0_pretty_parse(s, n, &m);
        CHECK(back && m == sizeof(in) - 1 && memcmp(back, in, m) == 0);
        free(back);
        free(s);
    }
    s = c0_pretty_format(in, sizeof(in) - 1, NULL, C0_PRETTY_SPACED, &n, NULL);
    CHECK(s != NULL);
    if (s) {
        CHECK(strstr(s, "\xe2\x90\x9e" " Alice Smith " "\xe2\x90\x9f" " 100") != NULL);
        CHECK(strstr(s, "\xe2\x90\x9e" " Bob" "\xe2\x90\x9f" "\xe2\x90\x90") != NULL); /* a line holding a level is not aligned */
        back = c0_pretty_parse(s, n, &m);
        CHECK(back && m == sizeof(in) - 1 && memcmp(back, in, m) == 0);
        free(back);
        free(s);
    }
    {
        /* a value holding a glyph cannot be shown */
        const uint8_t g[] = "\x1e" "\xe2\x90\x9e";
        c0_error err;
        CHECK(c0_pretty_format(g, 4, NULL, C0_PRETTY_COMPACT, &n, &err) == NULL && err.code == C0_ERR_SYNTAX);
    }
    {
        /* layout around glyphs is dropped on parse; interior spaces kept */
        const char *p = "\xe2\x90\x9e" "  Alice Smith  " "\xe2\x90\x9f" " 1\n";
        back = c0_pretty_parse(p, strlen(p), &m);
        CHECK(back && m == 14 && memcmp(back, "\x1e" "Alice Smith" "\x1f" "1", 14) == 0);
        free(back);
    }
}

static void test_validate_errors(void) {
    c0_error err;
    CHECK(c0_validate(S("\x1e" "a" "\x0e" "b"), &err) == C0_ERR_UNASSIGNED && err.pos == 2);
    CHECK(c0_validate(S("\x1e\x02" "abc"), &err) == C0_ERR_UNEXPECTED_END);
    CHECK(c0_validate(S("\x1e" "a" "\x01" "k" "\x1f" "v"), &err) == C0_ERR_SYNTAX);
    CHECK(c0_validate(S(""), &err) == C0_OK);
    CHECK(c0_canonical(S("")));
    CHECK(!c0_canonical(S("\x1e" "a ")));
    CHECK(c0_canonical(S("\x1d" "g" "\x1e" "a" "\x1f\x02" " b" "\x03")));
}

int main(void) {
    test_helpers();
    test_scan();
    test_value();
    test_body();
    test_keyed_and_lists();
    test_comments();
    test_document();
    test_blob();
    test_builder();
    test_builder_primitives();
    test_stream();
    test_pretty();
    test_validate_errors();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all core tests passed\n");
    return 0;
}
