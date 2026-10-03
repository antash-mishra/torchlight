# C17 library, resident daemon/CLI, sanitizer checks and engine/IPC benchmarks.
-include machine.mk
CC ?= cc
PKG_CONFIG ?= pkg-config
DEPS_PREFIX ?=
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_GNU_SOURCE
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS ?= -std=c17 -O0 -g3
CFLAGS += -pthread
LDLIBS += -pthread
ifeq ($(strip $(DEPS_PREFIX)),)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 libutf8proc)
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 libutf8proc)
else
CPPFLAGS += -I$(DEPS_PREFIX)/include
LDFLAGS += -L$(DEPS_PREFIX)/lib/x86_64-linux-gnu -Wl,-rpath,$(DEPS_PREFIX)/lib/x86_64-linux-gnu
LDLIBS += -lsqlite3 -lutf8proc
endif
SOURCES = src/core/common.c src/core/vec.c src/core/hashmap.c src/core/config.c src/core/json.c src/core/path.c src/core/sort.c src/core/mask.c src/core/parallel.c \
          src/index/tokenize.c src/index/prefix.c src/index/subseq.c src/index/fuzzy.c \
          src/index/trigram.c src/index/typo.c src/index/dirtree.c src/index/lexical.c \
          src/index/lexical_query.c src/index/catalog.c src/fs/crawl.c src/fs/watch.c src/storage/store.c \
          src/ipc/ipc.c src/ipc/client.c src/service/writer.c src/service/daemon.c
BIN_SOURCES = src/bin/torchlight.c src/bin/torchlightd.c
OBJECTS = $(SOURCES:%.c=build/%.o)
HEADERS = $(wildcard include/torchlight/*.h) $(wildcard src/*/*.h)
TEST_SOURCES = $(wildcard tests/unit/test_*.c)
BENCH_SOURCES = $(wildcard tests/bench/*.c)
FIXTURE_SOURCE = tests/bench/fixture/export.c
ALLOC_SOURCE = tests/alloc/query.c
# Optional NUL-separated real path list for `make bench` (see scripts/make_corpus.sh).
BENCH_PATHS ?=
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
.PHONY: all test lint format bench bench-daemon clean
all: build/torchlight build/torchlightd
build/torchlight: $(OBJECTS) build/src/bin/torchlight.o
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/torchlightd: $(OBJECTS) build/src/bin/torchlightd.o
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
build/torchlightd-sanitized: $(SOURCES) src/bin/torchlightd.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/test_query_alloc: $(SOURCES) $(ALLOC_SOURCE) $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O2 -g $(WARNINGS) $(SOURCES) $(ALLOC_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
test: build/tests build/torchlight-sanitized build/torchlightd-sanitized build/test_query_alloc
	ASAN_OPTIONS=detect_leaks=1 ./build/tests
	./build/test_query_alloc
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_cli.py ./build/torchlight-sanitized
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_daemon.py ./build/torchlight-sanitized ./build/torchlightd-sanitized
lint:
	@command -v $(CLANG_TIDY) >/dev/null || { echo 'clang-tidy is required'; exit 1; }
	@command -v $(CPPCHECK) >/dev/null || { echo 'cppcheck is required'; exit 1; }
	$(CLANG_TIDY) $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(FIXTURE_SOURCE) $(ALLOC_SOURCE) --warnings-as-errors='*' -- $(CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -D_GNU_SOURCE -Iinclude $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(FIXTURE_SOURCE) $(ALLOC_SOURCE)
format:
	clang-format -i $(SOURCES) $(BIN_SOURCES) $(HEADERS) tests/unit/*.h $(TEST_SOURCES) tests/bench/*.c tests/bench/*.h $(FIXTURE_SOURCE) $(ALLOC_SOURCE)
build/bench_lexical: $(SOURCES) $(BENCH_SOURCES) $(HEADERS) $(wildcard tests/bench/*.h)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) $(BENCH_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
bench: build/bench_lexical
	./build/bench_lexical --synthetic 50000
	./build/bench_lexical --synthetic 500000
	$(if $(BENCH_PATHS),./build/bench_lexical --paths $(BENCH_PATHS) --limit 500000)
build/torchlightd-release: $(SOURCES) src/bin/torchlightd.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/bench_fixture: src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
bench-daemon: build/torchlightd-release build/bench_fixture
	python3 tests/bench/bench_daemon.py ./build/torchlightd-release ./build/bench_fixture --sizes 50000 500000
clean:
	$(RM) -r build
-include $(OBJECTS:.o=.d) build/src/bin/torchlight.d build/src/bin/torchlightd.d
