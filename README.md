# c0

A C implementation of [C0DATA](https://github.com/trans/c0data) — structured
data built on ASCII C0 control codes. Single-header, C99, no dependencies
beyond libc.

The read path is **zero-copy and allocation-free**: cursors walk a
caller-owned buffer and hand back `(pointer, length)` views into it. The hot
loop is a single comparison, `byte < 0x20` — this is the implementation C0 was
designed for, and the natural core for other languages to bind to via FFI.

## Usage

Drop `c0.h` into your project. In exactly one translation unit:

```c
#define C0_IMPLEMENTATION
#include "c0.h"
```

Everywhere else, just `#include "c0.h"` for the declarations.

```c
/* Write */
c0_builder b;
c0_builder_init(&b);
c0_build_group_str(&b, "users");
{
    const char *hdrs[] = {"name", "amount"};
    const char *row[]  = {"Alice", "100"};
    c0_build_headers_str(&b, hdrs, 2);
    c0_build_record_str(&b, row, 2);
}
c0_bytes buf = c0_builder_bytes(&b);

/* Read (zero-copy) */
c0_group g = c0_table(buf.ptr, buf.len);
c0_iter ri = c0_group_records(g);
c0_bytes rec, field;
while (c0_next_record(&ri, &rec)) {
    c0_field_iter fi = c0_record_fields(rec);
    while (c0_next_field(&fi, &field)) {
        /* field.ptr / field.len point into buf; c0_unescape to decode */
    }
}
c0_builder_free(&b);
```

## Status

Core (single-header) — done and tested:

- tokenizer; table/record and document/group readers (cursor API)
- builder (byte-transparent fields; names reject control bytes)
- canonical-form helpers (`c0_canonical`, `c0_unescape`, `c0_is_assigned`)
- ETB stream mode (`c0_stream_read`: committed region, torn-tail detection,
  block cursor)

Planned: pretty (Unicode Control Pictures) formatting, the shared conformance
vectors, and the converters (CSV / JSON / C0DIFF) as separate `.c` files.

## Build & test

```sh
make test
```

Compiles with `-std=c99 -Wall -Wextra -Wpedantic`.

## License

MIT
