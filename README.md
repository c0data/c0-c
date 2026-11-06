# c0

A C implementation of [C0DATA](https://github.com/c0data) 1.0 — structured
data built on ASCII C0 control codes. Single-header, C99, no dependencies
beyond libc.

The read path is **zero-copy and allocation-free**: views and cursors walk a
caller-owned buffer and hand back `(pointer, length)` slices into it. The hot
loop outside text is a single comparison, `byte < 0x20` — this is the
implementation C0 was designed for, and the natural core for other languages
to bind to via FFI (the Python and Ruby packages wrap this header).

## Usage

Drop `c0.h` into your project. In exactly one translation unit:

```c
#define C0_IMPLEMENTATION
#include "c0.h"
```

Everywhere else, just `#include "c0.h"` for the declarations.

```c
/* Write: ␖␁shape␟tabular␝users␁␞name␟amount␞Alice␟100␞Bob␟200␄ */
c0_builder b;
c0_builder_init(&b);
c0_build_syn(&b);
c0_build_key_str(&b, "shape", "tabular");
c0_build_group_str(&b, "users");
{
    const char *hdrs[] = {"name", "amount"};
    const char *row[]  = {"Alice", "100"};
    c0_build_header_str(&b, hdrs, 2);          /* ␁␞name␟amount */
    c0_build_record_str(&b, row, 2);           /* ␞Alice␟100    */
}
c0_build_eot(&b);
c0_bytes buf = c0_builder_bytes(&b);

/* Read (zero-copy) */
c0_document doc;
if (c0_document_read(buf.ptr, buf.len, 0, &doc, NULL) == C0_OK) {
    c0_group_iter gi = c0_body_groups(doc.body);
    c0_group g;
    while (c0_next_group(&gi, &g)) {
        c0_record_iter ri = c0_group_records(g);
        c0_record rec;
        while (c0_next_record(&ri, &rec)) {
            c0_unit_iter ui = c0_record_units(rec);
            c0_bytes raw, value;
            while (c0_next_unit(&ui, &raw)) {
                /* raw points into buf; c0_value decodes text in place when it can */
                if (c0_value(raw, NULL, &value) == C0_OK) { /* value.ptr / value.len */ }
            }
        }
    }
}
c0_builder_free(&b);
```

Values are written in their one spelling: plain when they can be, text
(`␂…␃`) when they hold a control byte or edge whitespace. `c0_validate`
checks a buffer against the grammar once; the navigators then assume
well-formed input. `c0_canonical` answers whether bytes are a canonical unit.

### Keyed records, nesting, references

```c
const char *keys[] = {"host", "port"}, *vals[] = {"localhost", "5432"};
c0_build_map_str(&b, keys, vals, 2);        /* ␞␁host␟localhost␁port␟5432 */

const char *roles[] = {"Admin", "Editor"};
c0_build_record_str(&b, row, 1);
c0_build_list_str(&b, roles, 2);            /* one unit: ␐␞Admin␟Editor␘ */
c0_build_level_open(&b);                    /* ␐ … a whole body … ␘   */
c0_build_map_str(&b, keys, vals, 2);
c0_build_level_close(&b);
c0_build_ref_str(&b, "tags");               /* ␅tags */
```

Reading back: `c0_record_keyed` / `c0_record_pairs` / `c0_record_get` for
maps, `c0_unit_level` for a nested body, `c0_unit_list` for the flat-list
case, `c0_unit_ref` for a reference. Documents come from `c0_documents` /
`c0_next_document` (SYN heading, body, EOT, warnings); `c0_blob` reads a
trailing blob declared in the heading's `blob` table.

### Primitives

Underneath, the builder has one function per code: `c0_builder_fs`, `_gs`,
`_rs`, `_us`, `_sub` (C0-DIFF only), `_soh`, `_dle`, `_can`, `_bel`, `_enq`,
`_syn`, `_eot`, `_etb`, `_em`. Each writes its code and, when given, what follows it, so a sequence
of calls reads like the bytes, and anything the grammar allows can be
written. The bare form writes the code alone; `_name` / `_value` / `_text` /
`_payload` (or `_str` on a C string) write the code and what follows it:

```c
c0_builder_rs_str(&b, "Alice"); c0_builder_us_str(&b, "1502.30");   /* ␞Alice␟1502.30 */
c0_builder_rs(&b);                                                  /* ␞␁host␟localhost␁port␟5432 */
c0_builder_soh_str(&b, "host"); c0_builder_us_str(&b, "localhost");
c0_builder_soh_str(&b, "port"); c0_builder_us_str(&b, "5432");
c0_builder_gs_str(&b, "users"); c0_builder_soh(&b);                 /* ␝users␁␞name␟amount: a header row */
c0_builder_rs_str(&b, "name"); c0_builder_us_str(&b, "amount");
c0_builder_bel(&b); c0_builder_gs_str(&b, "old");                   /* ␇␝old: a dead group */
c0_builder_gs(&b); c0_builder_gs_str(&b, "sub");                    /* ␝␝sub: a group at depth 2 */
c0_builder_rs_str(&b, "old"); c0_builder_sub_str(&b, "new");       /* ␞old␚new: a C0-DIFF replace-all */
```

The composed `c0_build_*` functions above are built on these and cannot
write ill-formed output; with the primitives, well-formedness is yours.
Values keep their one spelling either way (there is no STX/ETX primitive),
and names are still checked. The two layers share the record state and mix
freely: `c0_builder_rs_str(&b, "a"); c0_build_field_str(&b, "b");` writes
`␞a␟b`.

## Status

Core (single-header) — done and tested:

- scanning primitives, values (`c0_value`, `c0_decode_text`), bodies with
  files / groups / records / units as cursors
- validator (`c0_validate`) and canonical check (`c0_canonical`)
- documents and containers (SYN heading, EOT, EM tail, warnings as bits)
- builder: one primitive per code (`c0_builder_rs` …) and composed `c0_build_*`
  functions over them (canonical spelling; names must be plain)
- ETB stream mode (`c0_stream_read`: committed region, damaged blocks,
  torn-tail detection, block cursor)
- pretty form (Unicode Control Pictures): `c0_pretty_format` in compact,
  aligned and spaced modes, `c0_pretty_parse`
- passes every shared conformance vector (decode, encode, canonical, invalid,
  stream, nested, diff)

Converters — separate `.c` files that `#include "c0.h"`, compiled in only when
you need them (the core header stays dependency-free):

- `c0_csv.{h,c}` — `c0_from_csv` / `c0_to_csv` (RFC-4180 quoting)
- `c0_json.{h,c}` — a `c0_json_value` tree plus `c0_to_value` /
  `c0_from_value` and a dependency-free JSON text codec (`c0_to_json` /
  `c0_from_json`, `c0_json_parse` / `c0_json_print`); scalars are strings,
  as in the reference implementations
- `c0_diff.{h,c}` — C0-DIFF parse / build / atomic `c0_diff_apply`
  (anchored sections, replace-all, dead lines)

Each shares the tiny `c0_strbuf.h` growable-buffer helper. The converters emit
byte-identical output to the Crystal reference implementation.

## Build & test

```sh
make test    # builds and runs the unit + conformance tests
make gen     # regenerate the conformance driver from vectors (needs python3)
```

Compiles with `-std=c99 -Wall -Wextra -Wpedantic`. The conformance driver
(`tests/vectors_gen.h`) is generated from the shared vectors and checked in, so
`make test` needs neither Python nor the submodule. The vectors come from
[c0-spec](https://github.com/c0data/c0-spec), included as a git submodule at
`c0-spec/` and only needed when regenerating:

```sh
git submodule update --init   # once after cloning
make gen                      # regenerate tests/vectors_gen.h
```

## License

MIT
