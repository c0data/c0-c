CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic

TESTS := tests/test_core tests/test_conformance

.PHONY: test gen clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

tests/test_core: tests/test_core.c c0.h
	$(CC) $(CFLAGS) -o $@ tests/test_core.c

tests/test_conformance: tests/test_conformance.c tests/vectors_gen.h c0.h
	$(CC) $(CFLAGS) -o $@ tests/test_conformance.c

# Regenerate the conformance driver from the vendored vectors (needs python3).
gen:
	python3 tools/gen_vectors.py

clean:
	rm -f $(TESTS)
