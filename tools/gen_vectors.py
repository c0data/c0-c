#!/usr/bin/env python3
"""Generate tests/vectors_gen.h from the conformance vectors (schema v2).

Emits straight-line C (cf_decode / cf_encode / cf_canonical / cf_invalid /
cf_stream / cf_nested / cf_diff) that drives the c0 API and CHECKs against
the expected values; the helpers it calls live in tests/test_conformance.c.
The output is committed, so `make test` needs no Python; only `make gen`
does. Run as `python3 -I tools/gen_vectors.py`.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "..", "c0-spec", "vectors")
OUT = os.path.join(HERE, "..", "tests", "vectors_gen.h")

_gid = [0]


def uid(prefix):
    _gid[0] += 1
    return "%s%d" % (prefix, _gid[0])


def load(name):
    with open(os.path.join(VEC, name)) as f:
        return json.load(f)["cases"]


def hexbytes(h):
    return bytes(int(h[i:i + 2], 16) for i in range(0, len(h), 2))


def scalar(f):
    return isinstance(f, str) or (isinstance(f, dict) and "hex" in f)


def scalar_bytes(f):
    if isinstance(f, str):
        return f.encode("utf-8")
    return hexbytes(f["hex"])


def arr(L, ind, b):
    """Append a byte-array declaration; return (cname, length)."""
    nm = uid("v")
    if len(b) == 0:
        L.append("%sstatic const unsigned char %s[1] = {0};" % (ind, nm))
    else:
        body = ",".join("0x%02x" % x for x in b)
        L.append("%sstatic const unsigned char %s[] = {%s};" % (ind, nm, body))
    return nm, len(b)


def sarr(L, ind, s):
    return arr(L, ind, s.encode("utf-8"))


# ---------------------------------------------------------------- checking

def check_value(L, ind, rec, idx, f):
    if scalar(f):
        n, l = arr(L, ind, scalar_bytes(f))
        L.append("%sCHECK(cf_val_eq(%s,%d,%s,%d));" % (ind, rec, idx, n, l))
    elif "level" in f:
        lv = uid("lv")
        L.append("%s{ c0_body %s; CHECK(cf_level(%s,%d,&%s));" % (ind, lv, rec, idx, lv))
        check_body(L, ind + "  ", lv, f["level"])
        L.append("%s}" % ind)
    elif "ref" in f:
        rf = uid("rf")
        segs = [f["ref"]] if isinstance(f["ref"], str) else f["ref"]
        L.append("%s{ c0_ref %s; CHECK(cf_ref(%s,%d,&%s) && %s.count==%d);" % (ind, rf, rec, idx, rf, rf, len(segs)))
        for i, s in enumerate(segs):
            n, l = sarr(L, ind + "  ", s)
            L.append("%s  CHECK(%s.count>%d && cf_eq(%s.seg[%d],%s,%d));" % (ind, rf, i, rf, i, n, l))
        L.append("%s}" % ind)
    else:
        raise ValueError("unknown value form %r" % (f,))


def check_record(L, ind, rec, r):
    if isinstance(r, list):
        L.append("%sCHECK(!c0_record_keyed(%s));" % (ind, rec))
        L.append("%sCHECK(c0_record_count(%s)==%d);" % (ind, rec, len(r)))
        for j, f in enumerate(r):
            check_value(L, ind, rec, j, f)
    else:
        pi = uid("pi")
        L.append("%sCHECK(c0_record_keyed(%s));" % (ind, rec))
        L.append("%s{ c0_pair_iter %s = c0_record_pairs(%s); c0_bytes k, v;" % (ind, pi, rec))
        for k, v in r["keys"]:
            kn, kl = sarr(L, ind + "  ", k)
            vn, vl = arr(L, ind + "  ", scalar_bytes(v))
            L.append("%s  CHECK(c0_next_pair(&%s,&k,&v) && cf_eq(k,%s,%d) && cf_raw_eq(v,%s,%d));"
                     % (ind, pi, kn, kl, vn, vl))
        L.append("%s  CHECK(!c0_next_pair(&%s,&k,&v)); }" % (ind, pi))


def check_group(L, ind, g, e, depth):
    nn, nl = sarr(L, ind, e["name"])
    L.append("%sCHECK(cf_eq(c0_group_name(%s),%s,%d));" % (ind, g, nn, nl))
    if depth is not None:
        L.append("%sCHECK(%s.depth==%d);" % (ind, g, depth))
    h = uid("h")
    L.append("%s{ c0_record %s;" % (ind, h))
    if e.get("headers") is not None:
        L.append("%s  CHECK(c0_group_header(%s,&%s));" % (ind, g, h))
        L.append("%s  CHECK(c0_record_count(%s)==%d);" % (ind, h, len(e["headers"])))
        for i, name in enumerate(e["headers"]):
            n, l = sarr(L, ind + "  ", name)
            L.append("%s  CHECK(cf_val_eq(%s,%d,%s,%d));" % (ind, h, i, n, l))
    else:
        L.append("%s  CHECK(!c0_group_header(%s,&%s));" % (ind, g, h))
    L.append("%s}" % ind)
    ri = uid("ri")
    r = uid("r")
    L.append("%s{ c0_record_iter %s = c0_group_records(%s); c0_record %s;" % (ind, ri, g, r))
    for rec in e["records"]:
        L.append("%s  CHECK(c0_next_record(&%s,&%s));" % (ind, ri, r))
        check_record(L, ind + "  ", r, rec)
    L.append("%s  CHECK(!c0_next_record(&%s,&%s)); CHECK(%s.err.code==C0_OK); }" % (ind, ri, r, ri))


def flat_groups(groups, depth=1):
    out = []
    for g in groups:
        out.append((depth, g))
        out.extend(flat_groups(g.get("sections", []), depth + 1))
    return out


def check_groups(L, ind, iter_expr, groups):
    gi = uid("gi")
    g = uid("g")
    L.append("%s{ c0_group_iter %s = %s; c0_group %s;" % (ind, gi, iter_expr, g))
    for i, (depth, e) in enumerate(flat_groups(groups)):
        L.append("%s  CHECK(c0_next_group(&%s,&%s));" % (ind, gi, g))
        # the unnamed leading group has depth 0 in the reader
        check_group(L, ind + "  ", g, e, None if (e["name"] == "" and i == 0) else depth)
    L.append("%s  CHECK(!c0_next_group(&%s,&%s)); CHECK(%s.err.code==C0_OK); }" % (ind, gi, g, gi))


def check_files(L, ind, body, files):
    fi = uid("fi")
    f = uid("f")
    L.append("%s{ c0_file_iter %s = c0_body_files(%s); c0_file %s;" % (ind, fi, body, f))
    for e in files:
        nn, nl = sarr(L, ind + "  ", e["name"])
        L.append("%s  CHECK(c0_next_file(&%s,&%s)); CHECK(cf_eq(c0_file_name(%s),%s,%d));" % (ind, fi, f, f, nn, nl))
        check_groups(L, ind + "  ", "c0_file_groups(%s)" % f, e["groups"])
    L.append("%s  CHECK(!c0_next_file(&%s,&%s)); CHECK(%s.err.code==C0_OK); }" % (ind, fi, f, fi))


def check_body(L, ind, body, e):
    if "files" in e:
        check_files(L, ind, body, e["files"])
    elif "records" in e:
        # a level described as a single table: one unnamed file, at most one group
        fi = uid("fi")
        f = uid("f")
        L.append("%s{ c0_file_iter %s = c0_body_files(%s); c0_file %s;" % (ind, fi, body, f))
        L.append("%s  CHECK(c0_next_file(&%s,&%s)); CHECK(c0_file_name(%s).len==0);" % (ind, fi, f, f))
        if not e["records"] and e.get("headers") is None:
            check_groups(L, ind + "  ", "c0_file_groups(%s)" % f, [])
        else:
            check_groups(L, ind + "  ", "c0_file_groups(%s)" % f,
                         [{"name": "", "headers": e.get("headers"), "records": e["records"]}])
        L.append("%s  CHECK(!c0_next_file(&%s,&%s)); }" % (ind, fi, f))
    else:
        check_groups(L, ind, "c0_body_groups(%s)" % body, e["groups"])


def check_heading(L, ind, doc, pairs):
    hi = uid("hi")
    L.append("%s{ c0_pair_iter %s = c0_document_heading(&%s); c0_bytes k, v;" % (ind, hi, doc))
    for k, v in pairs:
        kn, kl = sarr(L, ind + "  ", k)
        L.append("%s  CHECK(c0_next_pair(&%s,&k,&v) && cf_eq(k,%s,%d));" % (ind, hi, kn, kl))
        if scalar(v):
            vn, vl = arr(L, ind + "  ", scalar_bytes(v))
            L.append("%s  CHECK(cf_raw_eq(v,%s,%d));" % (ind, vn, vl))
        elif "level" in v:
            lv = uid("lv")
            L.append("%s  { c0_body %s; CHECK(c0_unit_level(v,&%s)==C0_OK);" % (ind, lv, lv))
            check_body(L, ind + "    ", lv, v["level"])
            L.append("%s  }" % ind)
    L.append("%s  CHECK(!c0_next_pair(&%s,&k,&v)); CHECK(%s.err.code==C0_OK); }" % (ind, hi, hi))


# ---------------------------------------------------------------- building

def bytes_list(L, ind, items):
    """Declare a c0_bytes array for a list of byte strings; return (name, count)."""
    decls = [arr(L, ind, b) for b in items]
    nm = uid("B")
    if decls:
        L.append("%sc0_bytes %s[] = {%s};" % (ind, nm, ",".join("{%s,%d}" % d for d in decls)))
    else:
        L.append("%sc0_bytes %s[1] = {{0,0}};" % (ind, nm))
    return nm, len(decls)


def build_value(L, ind, f):
    if scalar(f):
        n, l = arr(L, ind, scalar_bytes(f))
        L.append("%sc0_build_field(&bld,%s,%d);" % (ind, n, l))
    elif "level" in f:
        L.append("%sc0_build_level_open(&bld);" % ind)
        build_body(L, ind, f["level"])
        L.append("%sc0_build_level_close(&bld);" % ind)
    elif "ref" in f:
        segs = [f["ref"]] if isinstance(f["ref"], str) else f["ref"]
        if len(segs) == 1:
            n, l = sarr(L, ind, segs[0])
            L.append("%sc0_build_ref(&bld,%s,%d);" % (ind, n, l))
        else:
            L.append("%s{" % ind)
            nm, cnt = bytes_list(L, ind + "  ", [s.encode("utf-8") for s in segs])
            L.append("%s  c0_build_ref_path(&bld,%s,%d); }" % (ind, nm, cnt))
    elif "map" in f:
        L.append("%sc0_build_level_open(&bld);" % ind)
        build_record(L, ind, {"keys": f["map"]})
        L.append("%sc0_build_level_close(&bld);" % ind)
    else:
        raise ValueError("unknown value form %r" % (f,))


def build_record(L, ind, r):
    L.append("%sc0_build_record(&bld,(const c0_bytes*)0,0);" % ind)
    if isinstance(r, list):
        for f in r:
            build_value(L, ind, f)
    else:
        for k, v in r["keys"]:
            kn, kl = sarr(L, ind, k)
            if scalar(v):
                vn, vl = arr(L, ind, scalar_bytes(v))
                L.append("%sc0_build_key(&bld,%s,%d,%s,%d);" % (ind, kn, kl, vn, vl))
            elif "map" in v:
                L.append("%sc0_build_key_level_open(&bld,%s,%d);" % (ind, kn, kl))
                build_record(L, ind, {"keys": v["map"]})
                L.append("%sc0_build_level_close(&bld);" % ind)
            elif "level" in v:
                L.append("%sc0_build_key_level_open(&bld,%s,%d);" % (ind, kn, kl))
                build_body(L, ind, v["level"])
                L.append("%sc0_build_level_close(&bld);" % ind)


def build_header(L, ind, headers):
    L.append("%s{" % ind)
    nm, cnt = bytes_list(L, ind + "  ", [h.encode("utf-8") for h in headers])
    L.append("%s  c0_build_header(&bld,%s,%d); }" % (ind, nm, cnt))


def build_groups(L, ind, groups):
    for i, g in enumerate(groups):
        headers = g.get("headers")
        if g["name"] == "" and i == 0:
            if headers is not None:
                build_header(L, ind, headers)  # the unnamed leading group: no GS
        else:
            n, l = sarr(L, ind, g["name"])
            L.append("%sc0_build_group(&bld,%s,%d);" % (ind, n, l))
            if headers is not None:
                build_header(L, ind, headers)
        for r in g["records"]:
            build_record(L, ind, r)


def build_body(L, ind, e):
    if "records" in e:
        if e.get("headers") is not None:
            build_header(L, ind, e["headers"])
        for r in e["records"]:
            build_record(L, ind, r)
    elif "groups" in e:
        build_groups(L, ind, e["groups"])


def check_built(L, ind, expected_hex, canonical=True):
    en, el = arr(L, ind, hexbytes(expected_hex))
    L.append("%sCHECK(c0_builder_status(&bld)==C0_OK);" % ind)
    L.append("%s{ c0_bytes got=c0_builder_bytes(&bld); CHECK(cf_eq(got,%s,%d));" % (ind, en, el))
    if canonical:
        L.append("%s  CHECK(c0_canonical(got.ptr,got.len)); }" % ind)
    else:
        L.append("%s}" % ind)


# ------------------------------------------------------------------- files

def gen_decode(cases):
    L = ["static void cf_decode(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    c0_doc_iter di = c0_documents(%s,%d); c0_document doc;" % (bn, bl))
        L.append("    CHECK(c0_validate(%s,%d,(c0_error*)0)==C0_OK);" % (bn, bl))
        if "documents" in c:
            for d in c["documents"]:
                L.append("    CHECK(c0_next_document(&di,&doc));")
                if "heading" in d:
                    check_heading(L, "    ", "doc", d["heading"])
                fn, fl = sarr(L, "    ", d["file"] or "")
                L.append("    CHECK(cf_eq(cf_doc_name(&doc),%s,%d));" % (fn, fl))
                check_groups(L, "    ", "c0_body_groups(doc.body)", d["groups"])
            L.append("    CHECK(!c0_next_document(&di,&doc)); CHECK(di.err.code==C0_OK);")
        else:
            L.append("    CHECK(c0_next_document(&di,&doc));")
            if "heading" in c:
                L.append("    CHECK(doc.has_syn);")
                check_heading(L, "    ", "doc", c["heading"])
            if "files" in c:
                check_files(L, "    ", "doc.body", c["files"])
            else:
                fn, fl = sarr(L, "    ", c["file"] or "")
                L.append("    CHECK(cf_eq(cf_doc_name(&doc),%s,%d));" % (fn, fl))
                check_groups(L, "    ", "c0_body_groups(doc.body)", c["groups"])
            if "tail" in c:
                tn, tl = arr(L, "    ", hexbytes(c["tail"]))
                L.append("    while (c0_next_document(&di,&doc)) {}")
                L.append("    CHECK(di.has_em); CHECK(cf_eq(c0_doc_iter_tail(&di),%s,%d));" % (tn, tl))
        L.append("  }")
    L.append("}")
    return L


def gen_encode(cases):
    L = ["static void cf_encode(void) {"]
    for c in cases:
        build = c["build"]
        L.append("  { /* %s */" % c["name"])
        L.append("    c0_builder bld; c0_builder_init(&bld);")
        if build.get("file") is not None:
            fn, fl = sarr(L, "    ", build["file"])
            L.append("    c0_build_file(&bld,%s,%d);" % (fn, fl))
        build_groups(L, "    ", build["groups"])
        check_built(L, "    ", c["canonical"])
        L.append("    c0_builder_free(&bld);")
        if "heading" in build:
            L.append("    c0_builder_init(&bld);")
            L.append("    c0_build_syn(&bld);")
            for k, v in build["heading"]:
                kn, kl = sarr(L, "    ", k)
                vn, vl = sarr(L, "    ", v)
                L.append("    c0_build_key(&bld,%s,%d,%s,%d);" % (kn, kl, vn, vl))
            if build.get("file") is not None:
                fn, fl = sarr(L, "    ", build["file"])
                L.append("    c0_build_file(&bld,%s,%d);" % (fn, fl))
            build_groups(L, "    ", build["groups"])
            L.append("    c0_build_eot(&bld);")
            check_built(L, "    ", c["bytes"], canonical=False)
            L.append("    c0_builder_free(&bld);")
        L.append("  }")
    L.append("}")
    return L


def gen_canonical(cases):
    L = ["static void cf_canonical(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    CHECK((c0_validate(%s,%d,(c0_error*)0)==C0_OK)==%d);" % (bn, bl, 1 if c["wellformed"] else 0))
        L.append("    CHECK(c0_canonical(%s,%d)==%d);" % (bn, bl, 1 if c["canonical"] else 0))
        L.append("  }")
    L.append("}")
    return L


def gen_invalid(cases):
    L = ["static void cf_invalid(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    CHECK(c0_validate(%s,%d,(c0_error*)0)!=C0_OK); }" % (bn, bl))
    L.append("}")
    return L


def gen_stream(cases):
    L = ["static void cf_stream(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    c0_stream s = c0_stream_read(%s,%d);" % (bn, bl))
        L.append("    CHECK(s.committed_end==%d);" % c["committed_end"])
        L.append("    CHECK(s.torn==%d);" % (1 if c["torn"] else 0))
        L.append("    CHECK(s.block_count==%d);" % len(c["blocks"]))
        damaged = set(c.get("damaged", []))
        L.append("    CHECK(s.damaged_count==%d);" % len(damaged))
        L.append("    { c0_block_iter bi = c0_stream_blocks(&s); c0_bytes blk; int dmg;")
        for i, h in enumerate(c["blocks"]):
            kn, kl = arr(L, "      ", hexbytes(h))
            L.append("      CHECK(c0_next_block(&bi,&blk,&dmg) && cf_eq(blk,%s,%d) && dmg==%d);"
                     % (kn, kl, 1 if i in damaged else 0))
        L.append("      CHECK(!c0_next_block(&bi,&blk,&dmg)); }")
        if "records" in c:
            L.append("    { cf_recs rr; c0_record r; CHECK(cf_recs_init(&rr,&s));")
            for rec in c["records"]:
                L.append("      CHECK(cf_recs_next(&rr,&r));")
                check_record(L, "      ", "r", rec)
            L.append("      CHECK(!cf_recs_next(&rr,&r)); cf_recs_free(&rr); }")
        L.append("  }")
    L.append("}")
    return L


def all_scalar(items):
    return all(scalar(x) for x in items)


def check_list(L, ind, rec, idx, items):
    li = uid("li")
    L.append("%s{ c0_unit_iter %s; c0_bytes raw, item; CHECK(c0_record_field(%s,%d,&raw)==C0_OK);"
             % (ind, li, rec, idx))
    L.append("%s  CHECK(c0_unit_list(raw,&%s)==C0_OK);" % (ind, li))
    for it in items:
        n, l = arr(L, ind + "  ", scalar_bytes(it))
        L.append("%s  CHECK(c0_next_unit(&%s,&item) && cf_raw_eq(item,%s,%d));" % (ind, li, n, l))
    L.append("%s  CHECK(!c0_next_unit(&%s,&item)); }" % (ind, li))


def gen_nested(cases):
    L = ["static void cf_nested(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        rec = c["record"]
        L.append("    c0_body body = c0_body_view(%s,%d);" % (bn, bl))
        L.append("    CHECK(c0_validate(%s,%d,(c0_error*)0)==C0_OK);" % (bn, bl))
        L.append("    { c0_record_iter ri = c0_body_records(body); c0_record r;")
        L.append("      CHECK(c0_next_record(&ri,&r));")
        L.append("      CHECK(c0_record_count(r)==%d);" % len(rec))
        for i, f in enumerate(rec):
            if scalar(f):
                n, l = arr(L, "      ", scalar_bytes(f))
                L.append("      CHECK(cf_val_eq(r,%d,%s,%d));" % (i, n, l))
            elif "list" in f:
                items = f["list"]
                if all_scalar(items):
                    check_list(L, "      ", "r", i, items)
                else:
                    L.append("      { c0_body lv; c0_record_iter ri2; c0_record r2; CHECK(cf_level(r,%d,&lv));" % i)
                    L.append("        ri2 = c0_body_records(lv); CHECK(c0_next_record(&ri2,&r2));")
                    L.append("        CHECK(c0_record_count(r2)==%d);" % len(items))
                    for j, x in enumerate(items):
                        if scalar(x):
                            n, l = arr(L, "        ", scalar_bytes(x))
                            L.append("        CHECK(cf_val_eq(r2,%d,%s,%d));" % (j, n, l))
                        else:
                            check_list(L, "        ", "r2", j, x["list"])
                    L.append("        CHECK(!c0_next_record(&ri2,&r2)); }")
            elif "level" in f:
                lv = uid("lv")
                L.append("      { c0_body %s; CHECK(cf_level(r,%d,&%s));" % (lv, i, lv))
                L.append("        { c0_unit_iter li; c0_bytes raw; CHECK(c0_record_field(r,%d,&raw)==C0_OK);" % i)
                L.append("          CHECK(c0_unit_list(raw,&li)==C0_ERR_NOT_LIST); }")
                check_body(L, "        ", lv, f["level"])
                L.append("      }")
        L.append("      CHECK(!c0_next_record(&ri,&r)); }")
        if c["canonical"]:
            L.append("    { c0_builder bld; c0_builder_init(&bld);")
            L.append("      c0_build_record(&bld,(const c0_bytes*)0,0);")
            for f in rec:
                if not scalar(f) and "list" in f and all_scalar(f["list"]):
                    L.append("      {")
                    nm, cnt = bytes_list(L, "        ", [scalar_bytes(x) for x in f["list"]])
                    L.append("        c0_build_list(&bld,%s,%d); }" % (nm, cnt))
                elif not scalar(f) and "list" in f:
                    L.append("      c0_build_level_open(&bld);")
                    L.append("      c0_build_record(&bld,(const c0_bytes*)0,0);")
                    for x in f["list"]:
                        if scalar(x):
                            n, l = arr(L, "      ", scalar_bytes(x))
                            L.append("      c0_build_field(&bld,%s,%d);" % (n, l))
                        else:
                            L.append("      {")
                            nm, cnt = bytes_list(L, "        ", [scalar_bytes(y) for y in x["list"]])
                            L.append("        c0_build_list(&bld,%s,%d); }" % (nm, cnt))
                    L.append("      c0_build_level_close(&bld);")
                else:
                    build_value(L, "      ", f)
            check_built(L, "      ", c["bytes"])
            L.append("      c0_builder_free(&bld); }")
        L.append("  }")
    L.append("}")
    return L


def gen_diff(cases):
    L = ["static void cf_diff(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        dn, dl = arr(L, "    ", hexbytes(c["diff"]))
        files = list(c["files"].items())
        entries = []
        for path, content in files:
            pn, pl = sarr(L, "    ", path)
            cn, cl = sarr(L, "    ", content)
            entries.append("{{%s,%d},{%s,%d}}" % (pn, pl, cn, cl))
        if entries:
            L.append("    c0_diff_file_input F[] = {%s};" % ",".join(entries))
        else:
            L.append("    c0_diff_file_input F[1] = {{{0,0},{0,0}}};")
        L.append("    c0_diff_file_output *out = 0; size_t n = 0; c0_diff_error err;")
        L.append("    int rc = c0_diff_apply(%s,%d,F,%d,&out,&n,&err);" % (dn, dl, len(files)))
        if c.get("error"):
            L.append("    CHECK(rc==0 && err.status!=C0_DIFF_OK);")
        else:
            L.append("    CHECK(rc==1 && n==%d);" % len(files))
            L.append("    if (rc) {")
            for path, content in c["result"].items():
                pn, pl = sarr(L, "      ", path)
                cn, cl = sarr(L, "      ", content)
                L.append("      CHECK(cf_out_eq(out,n,%s,%d,%s,%d));" % (pn, pl, cn, cl))
            L.append("      c0_diff_free_outputs(out,n); }")
        L.append("  }")
    L.append("}")
    return L


def main():
    out = ["/* Generated by tools/gen_vectors.py from the conformance vectors",
           "   (c0-spec/vectors, schema v2). Do not edit; run `make gen`. */", ""]
    out += gen_decode(load("decode.json")) + [""]
    out += gen_encode(load("encode.json")) + [""]
    out += gen_canonical(load("canonical.json")) + [""]
    out += gen_invalid(load("invalid.json")) + [""]
    out += gen_stream(load("stream.json")) + [""]
    out += gen_nested(load("nested.json")) + [""]
    out += gen_diff(load("diff.json")) + [""]
    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print("wrote", os.path.relpath(OUT))


if __name__ == "__main__":
    main()
