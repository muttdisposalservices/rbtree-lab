CC := gcc
CFLAGS := -std=c2x -Wall -Wextra -Werror -g -O1 -Iinclude
SRC := src/rbtree.c
TSRC := tests/test_rbtree.c tests/fault_malloc.c
BIN := build/test_rbtree
FUZZBIN := build/fuzz

all: $(BIN) $(FUZZBIN)

$(BIN): $(SRC) $(TSRC) include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) $(TSRC) -o $@
$(FUZZBIN): $(SRC) tests/fuzz.c include/rbtree.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) tests/fuzz.c -o $@
test: $(BIN) $(FUZZBIN)
	./$(BIN) && ./$(FUZZBIN) 100000

# TODO: make has no CFLAGS-sensitivity, so running e.g. `make asan` then
# `make memcheck`/`make test` without an intervening `make clean` silently
# reuses the ASan/UBSan-instrumented binary instead of rebuilding plain.
# Deferred for now since no real tree logic exists yet to trigger it; fix
# later with a separate build dir per config, or by adding clean as a
# dependency of test/memcheck.
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

memcheck: all
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(BIN)
	valgrind --leak-check=full --show-leak-kinds=all \
	--error-exitcode=1 ./$(FUZZBIN) 20000
clean:
	rm -rf build

.PHONY: all test asan memcheck clean
