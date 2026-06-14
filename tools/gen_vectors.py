#!/usr/bin/env python3
"""Generate tests/vectors_gen.h from the vendored conformance vectors.

Emits straight-line C (functions cf_decode/cf_encode/cf_canonical/cf_invalid/
cf_stream) that drive the c0 API and CHECK against expected values. The output
is committed, so `make test` needs no Python; only `make gen` does.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
VEC = os.path.join(HERE, "..", "c0-spec", "vectors")
OUT = os.path.join(HERE, "..", "tests", "vectors_gen.h")

_gid = [0]


def load(name):
    with open(os.path.join(VEC, name)) as f:
        return json.load(f)["cases"]


def hexbytes(h):
    return bytes(int(h[i:i + 2], 16) for i in range(0, len(h), 2))


def field_bytes(f):
    if isinstance(f, str):
        return f.encode("utf-8")
    return hexbytes(f["hex"])


def arr(L, ind, b):
    """Append a byte-array declaration; return (cname, length)."""
    _gid[0] += 1
    nm = "v%d" % _gid[0]
    if len(b) == 0:
        L.append("%sconst unsigned char %s[1] = {0};" % (ind, nm))
    else:
        body = ",".join("0x%02x" % x for x in b)
        L.append("%sconst unsigned char %s[] = {%s};" % (ind, nm, body))
    return nm, len(b)


def group_checks(L, ind, gv, g):
    nn, nl = arr(L, ind, g["name"].encode("utf-8"))
    L.append("%sCHECK(cf_eq(c0_group_name(%s), %s, %d));" % (ind, gv, nn, nl))

    L.append("%s{ c0_iter hi = c0_group_headers(%s); c0_bytes h;" % (ind, gv))
    if g["headers"]:
        for h in g["headers"]:
            hn, hl = arr(L, ind + "  ", h.encode("utf-8"))
            L.append("%s  CHECK(c0_next_header(&hi,&h) && cf_eq(h,%s,%d));" % (ind, hn, hl))
    L.append("%s  CHECK(!c0_next_header(&hi,&h)); }" % ind)

    L.append("%s{ c0_iter ri = c0_group_records(%s); c0_bytes rec;" % (ind, gv))
    for rec in g["records"]:
        L.append("%s  CHECK(c0_next_record(&ri,&rec));" % ind)
        L.append("%s  CHECK(cf_arity(rec)==%d);" % (ind, len(rec)))
        for idx, f in enumerate(rec):
            fn, fl = arr(L, ind + "  ", field_bytes(f))
            L.append("%s  CHECK(cf_val_eq(rec,%d,%s,%d));" % (ind, idx, fn, fl))
    L.append("%s  CHECK(!c0_next_record(&ri,&rec)); }" % ind)


def gen_decode(cases):
    L = ["static void cf_decode(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        groups = c["groups"]
        if c["file"] is None and len(groups) == 1 and groups[0]["name"] == "":
            L.append("    c0_group g = c0_table(%s, %d);" % (bn, bl))
            group_checks(L, "    ", "g", groups[0])
        else:
            fn, fl = arr(L, "    ", (c["file"] or "").encode("utf-8"))
            L.append("    CHECK(cf_eq(c0_doc_name(%s,%d), %s, %d));" % (bn, bl, fn, fl))
            L.append("    { c0_doc_iter di = c0_doc(%s, %d); c0_group g;" % (bn, bl))
            for g in groups:
                L.append("      CHECK(c0_next_group(&di,&g));")
                group_checks(L, "      ", "g", g)
            L.append("      CHECK(!c0_next_group(&di,&g)); }")
        L.append("  }")
    L.append("}")
    return L


def gen_encode(cases):
    L = ["static void cf_encode(void) {"]
    for c in cases:
        build = c["build"]
        L.append("  { /* %s */" % c["name"])
        L.append("    c0_builder bld; c0_builder_init(&bld);")
        if build["file"] is not None:
            fn, fl = arr(L, "    ", build["file"].encode("utf-8"))
            L.append("    c0_build_file(&bld,%s,%d);" % (fn, fl))
        for g in build["groups"]:
            gn, gl = arr(L, "    ", g["name"].encode("utf-8"))
            L.append("    c0_build_group(&bld,%s,%d);" % (gn, gl))
            if g["headers"]:
                items = [arr(L, "    ", h.encode("utf-8")) for h in g["headers"]]
                lst = ",".join("{%s,%d}" % (n, l) for n, l in items)
                L.append("    { c0_bytes H[] = {%s}; c0_build_headers(&bld,H,%d); }"
                         % (lst, len(items)))
            for rec in g["records"]:
                if rec:
                    items = [arr(L, "    ", field_bytes(f)) for f in rec]
                    lst = ",".join("{%s,%d}" % (n, l) for n, l in items)
                    L.append("    { c0_bytes F[] = {%s}; c0_build_record(&bld,F,%d); }"
                             % (lst, len(items)))
                else:
                    L.append("    c0_build_record(&bld,(c0_bytes*)0,0);")
        en, el = arr(L, "    ", hexbytes(c["canonical"]))
        L.append("    { c0_bytes got=c0_builder_bytes(&bld);")
        L.append("      CHECK(got.len==%d && (got.len==0||memcmp(got.ptr,%s,%d)==0));"
                 % (el, en, el))
        L.append("      CHECK(c0_canonical(got.ptr,got.len)); }")
        L.append("    c0_builder_free(&bld); }")
    L.append("}")
    return L


def gen_canonical(cases):
    L = ["static void cf_canonical(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    CHECK(cf_wellformed(%s,%d)==%d);" % (bn, bl, 1 if c["wellformed"] else 0))
        L.append("    CHECK(c0_canonical(%s,%d)==%d);" % (bn, bl, 1 if c["canonical"] else 0))
        L.append("  }")
    L.append("}")
    return L


def gen_invalid(cases):
    L = ["static void cf_invalid(void) {"]
    for c in cases:
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("  { /* %s */ CHECK(cf_wellformed(%s,%d)==0); }" % (c["name"], bn, bl))
    L.append("}")
    return L


def gen_stream(cases):
    L = ["static void cf_stream(void) {"]
    for c in cases:
        L.append("  { /* %s */" % c["name"])
        bn, bl = arr(L, "    ", hexbytes(c["bytes"]))
        L.append("    c0_stream s = c0_stream_read(%s, %d);" % (bn, bl))
        L.append("    CHECK(s.committed_end==%d);" % c["committed_end"])
        L.append("    CHECK(s.torn==%d);" % (1 if c["torn"] else 0))
        L.append("    CHECK(cf_block_count(&s)==%d);" % len(c["blocks"]))
        for i, h in enumerate(c["blocks"]):
            kn, kl = arr(L, "    ", hexbytes(h))
            L.append("    CHECK(cf_block_eq(&s,%d,%s,%d));" % (i, kn, kl))
        if "records" in c:
            L.append("    { c0_bytes cm=c0_stream_committed(&s);"
                     " c0_group g=c0_table(cm.ptr,cm.len);"
                     " c0_iter ri=c0_group_records(g); c0_bytes rec;")
            for rec in c["records"]:
                L.append("      CHECK(c0_next_record(&ri,&rec));")
                L.append("      CHECK(cf_arity(rec)==%d);" % len(rec))
                for idx, f in enumerate(rec):
                    fn, fl = arr(L, "      ", field_bytes(f))
                    L.append("      CHECK(cf_val_eq(rec,%d,%s,%d));" % (idx, fn, fl))
            L.append("      CHECK(!c0_next_record(&ri,&rec)); }")
        L.append("  }")
    L.append("}")
    return L


def main():
    out = ["/* Generated by tools/gen_vectors.py from the conformance vectors.",
           "   Do not edit; run `make gen` to regenerate. */", ""]
    out += gen_decode(load("decode.json")) + [""]
    out += gen_encode(load("encode.json")) + [""]
    out += gen_canonical(load("canonical.json")) + [""]
    out += gen_invalid(load("invalid.json")) + [""]
    out += gen_stream(load("stream.json")) + [""]
    with open(OUT, "w") as f:
        f.write("\n".join(out))
    print("wrote", os.path.relpath(OUT))


if __name__ == "__main__":
    main()
