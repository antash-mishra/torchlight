# C17 library, local CLI, sanitizer checks and warm-engine benchmarks.
-include machine.mk
CC ?= cc
PKG_CONFIG ?= pkg-config
DEPS_PREFIX ?=
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS ?= -std=c17 -O0 -g3
ifeq ($(strip $(DEPS_PREFIX)),)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 libutf8proc)
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 libutf8proc)
else
CPPFLAGS += -I$(DEPS_PREFIX)/include
LDFLAGS += -L$(DEPS_PREFIX)/lib/x86_64-linux-gnu -Wl,-rpath,$(DEPS_PREFIX)/lib/x86_64-linux-gnu
LDLIBS += -lsqlite3 -lutf8proc
endif
SOURCES = src/core/common.c src/core/vec.c src/core/config.c src/index/tokenize.c src/index/prefix.c \
          src/index/subseq.c src/index/fuzzy.c src/index/lexical.c src/fs/crawl.c src/storage/store.c
OBJECTS = $(SOURCES:%.c=build/%.o)
HEADERS = $(wildcard include/torchlight/*.h)
TEST_SOURCES = $(wildcard tests/unit/test_*.c)
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
.PHONY: all test lint format bench clean
all: build/torchlight
build/torchlight: $(OBJECTS) build/src/bin/torchlight.o
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@
build/tests: $(SOURCES) $(TEST_SOURCES) $(HEADERS) tests/unit/test.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/torchlight-sanitized: $(SOURCES) src/bin/torchlight.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) src/bin/torchlight.c $(LDFLAGS) $(LDLIBS) -o $@
test: build/tests build/torchlight-sanitized
	ASAN_OPTIONS=detect_leaks=1 ./build/tests
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_cli.py ./build/torchlight-sanitized
lint:
	@command -v $(CLANG_TIDY) >/dev/null || { echo 'clang-tidy is required'; exit 1; }
	@command -v $(CPPCHECK) >/dev/null || { echo 'cppcheck is required'; exit 1; }
	$(CLANG_TIDY) $(SOURCES) src/bin/torchlight.c $(TEST_SOURCES) tests/bench/bench_lexical.c --warnings-as-errors='*' -- $(CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -Iinclude $(SOURCES) src/bin/torchlight.c $(TEST_SOURCES) tests/bench/bench_lexical.c
format:
	clang-format -i $(SOURCES) src/bin/torchlight.c $(HEADERS) tests/unit/*.h $(TEST_SOURCES) tests/bench/bench_lexical.c
build/bench_lexical: $(SOURCES) tests/bench/bench_lexical.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) tests/bench/bench_lexical.c $(LDFLAGS) $(LDLIBS) -o $@
bench: build/bench_lexical
	./build/bench_lexical 50000
	./build/bench_lexical 500000
clean:
	$(RM) -r build
-include $(OBJECTS:.o=.d) build/src/bin/torchlight.d
