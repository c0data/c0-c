CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic

TESTS := tests/test_core

.PHONY: test clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

tests/test_core: tests/test_core.c c0.h
	$(CC) $(CFLAGS) -o $@ tests/test_core.c

clean:
	rm -f $(TESTS)
