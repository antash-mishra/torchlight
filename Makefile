# C17 library, resident daemon/CLI, sanitizer checks and engine/IPC benchmarks.
-include machine.mk
CC ?= cc
PKG_CONFIG ?= pkg-config
DEPS_PREFIX ?=
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags gio-unix-2.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gio-unix-2.0)
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_GNU_SOURCE
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS ?= -std=c17 -O0 -g3
CFLAGS += -pthread
LDLIBS += -pthread
LDLIBS += -lm
ifeq ($(strip $(DEPS_PREFIX)),)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags sqlite3 libutf8proc)
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 libutf8proc)
else
CPPFLAGS += -I$(DEPS_PREFIX)/include
LDFLAGS += -L$(DEPS_PREFIX)/lib/x86_64-linux-gnu -Wl,-rpath,$(DEPS_PREFIX)/lib/x86_64-linux-gnu
LDLIBS += -lsqlite3 -lutf8proc
endif
# Frizbee SIMD fuzzy matcher: vendored Rust source built offline into a static
# library by Torchlight-owned manifests (see third_party/README.md).
CARGO ?= cargo
FRIZBEE_SOURCE = third_party/frizbee
FRIZBEE_MANIFEST = third_party/frizbee-build/Cargo.toml
FRIZBEE_LIB = build/frizbee/release/libfrizbee.a
FRIZBEE_INPUTS = $(FRIZBEE_MANIFEST) third_party/frizbee-build/Cargo.lock \
                 third_party/frizbee-build/core/Cargo.toml \
                 $(shell find $(FRIZBEE_SOURCE)/src $(FRIZBEE_SOURCE)/bindings/frizbee-c/src -name '*.rs')
CPPFLAGS += -isystem $(FRIZBEE_SOURCE)/bindings/frizbee-c/include
# Rust's standard library needs these system libraries when linked statically.
LDLIBS += $(FRIZBEE_LIB) -lgcc_s -lutil -lrt -ldl
SOURCES = src/core/common.c src/core/vec.c src/core/hashmap.c src/core/config.c src/core/json.c src/core/path.c src/core/sort.c src/core/mask.c src/core/parallel.c \
          src/index/tokenize.c src/index/prefix.c src/index/subseq.c src/index/fuzzy.c \
          src/index/trigram.c src/index/typo.c src/index/dirtree.c src/index/lexical.c \
          src/index/embed.c src/index/potion.c src/index/vector.c src/index/rank.c \
          src/index/desktop.c src/index/lexical_query.c src/index/catalog.c src/fs/crawl.c src/fs/watch.c src/storage/store.c \
          src/ipc/ipc.c src/ipc/async.c src/ipc/client.c src/service/delta.c src/service/writer.c src/service/semantic.c src/service/daemon.c
BIN_SOURCES = src/bin/torchlight.c src/bin/torchlightd.c
OBJECTS = $(SOURCES:%.c=build/%.o)
HEADERS = $(wildcard include/torchlight/*.h) $(wildcard src/*/*.h)
TEST_SOURCES = $(wildcard tests/unit/test_*.c)
BENCH_SOURCES = tests/bench/bench_lexical.c tests/bench/corpus.c tests/bench/queries.c
VECTOR_BENCH_SOURCE = tests/bench/bench_vector.c
FIXTURE_SOURCE = tests/bench/fixture/export.c
DESKTOP_FIXTURE_SOURCE = tests/fixtures/desktop_replace.c
SEMANTIC_FIXTURE_SOURCE = tests/fixtures/semantic_stall.c
ALLOC_SOURCE = tests/alloc/query.c
# Optional NUL-separated real path list for `make bench` (see scripts/make_corpus.sh).
BENCH_PATHS ?=
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
.PHONY: all test lint format bench bench-vector bench-daemon clean
GTK_CPPFLAGS = $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags 'gtk4 >= 4.14' x11))
GTK_LDLIBS = $(shell $(PKG_CONFIG) --libs 'gtk4 >= 4.14' x11)
UI_SOURCES = ui/gtk/model.c ui/gtk/actions.c ui/gtk/launcher.c ui/gtk/view.c ui/gtk/path_label.c src/bin/torchlight-gtk.c
POPUP_FIXTURE_SOURCE = tests/fixtures/popup_probe.c
all: build/torchlight build/torchlightd build/torchlight-gtk
$(FRIZBEE_LIB): $(FRIZBEE_INPUTS)
	$(CARGO) build --release --offline --locked --manifest-path $(FRIZBEE_MANIFEST) --target-dir build/frizbee
	@touch $@
UI_HEADERS = $(wildcard ui/gtk/*.h)
UI_RESOURCES = build/ui/gtk/resources.c
$(UI_RESOURCES): ui/gtk/resources.xml ui/gtk/quiet-system.css
	@mkdir -p $(@D)
	glib-compile-resources --sourcedir=ui/gtk --generate-source --target=$@ --c-name=torchlight_ui $<
build/torchlight-gtk: $(OBJECTS) $(UI_SOURCES) $(UI_HEADERS) $(UI_RESOURCES) $(HEADERS) $(FRIZBEE_LIB)
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(CFLAGS) $(WARNINGS) $(UI_SOURCES) $(UI_RESOURCES) $(OBJECTS) $(LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
build/test_popup_view: ui/gtk/view.c ui/gtk/path_label.c tests/gtk/test_view.c $(UI_HEADERS) $(UI_RESOURCES) $(FRIZBEE_LIB)
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SAN_FLAGS) ui/gtk/view.c ui/gtk/path_label.c tests/gtk/test_view.c $(UI_RESOURCES) $(LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
build/test_popup_probe.so: $(POPUP_FIXTURE_SOURCE)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) -fPIC -shared $< $(GTK_LDLIBS) -ldl -o $@
build/torchlight: $(OBJECTS) build/src/bin/torchlight.o $(FRIZBEE_LIB)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/torchlightd: $(OBJECTS) build/src/bin/torchlightd.o $(FRIZBEE_LIB)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@
build/tests: $(SOURCES) ui/gtk/model.c ui/gtk/actions.c $(TEST_SOURCES) $(HEADERS) ui/gtk/actions.h tests/unit/test.h $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) ui/gtk/model.c ui/gtk/actions.c $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/torchlight-sanitized: $(SOURCES) src/bin/torchlight.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) src/bin/torchlight.c $(LDFLAGS) $(LDLIBS) -o $@
build/torchlightd-sanitized: $(SOURCES) src/bin/torchlightd.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/test_query_alloc: $(SOURCES) $(ALLOC_SOURCE) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O2 -g $(WARNINGS) $(SOURCES) $(ALLOC_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
build/test_desktop_replace.so: $(DESKTOP_FIXTURE_SOURCE)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) -fPIC -shared $< -ldl -o $@
build/test_semantic_stall.so: $(SEMANTIC_FIXTURE_SOURCE)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) -fPIC -shared $< -ldl -o $@
test: build/tests build/torchlight-sanitized build/torchlightd-sanitized build/test_query_alloc build/test_desktop_replace.so build/test_semantic_stall.so
	ASAN_OPTIONS=detect_leaks=1 ./build/tests
	./build/test_query_alloc
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_cli.py ./build/torchlight-sanitized
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_desktop.py ./build/torchlightd-sanitized ./build/test_desktop_replace.so
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_daemon.py ./build/torchlight-sanitized ./build/torchlightd-sanitized
	ASAN_OPTIONS=detect_leaks=1 python3 tests/test_semantic.py ./build/torchlightd-sanitized ./build/test_semantic_stall.so ./build/torchlight-sanitized
lint:
	@command -v $(CLANG_TIDY) >/dev/null || { echo 'clang-tidy is required'; exit 1; }
	@command -v $(CPPCHECK) >/dev/null || { echo 'cppcheck is required'; exit 1; }
	$(CLANG_TIDY) $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(VECTOR_BENCH_SOURCE) $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE) --warnings-as-errors='*' -- $(CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -D_GNU_SOURCE -Iinclude $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(VECTOR_BENCH_SOURCE) $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE)
	$(CLANG_TIDY) $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) tests/gtk/test_view.c --warnings-as-errors='*' -- $(CPPFLAGS) $(GTK_CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -D_GNU_SOURCE --library=gtk -Iinclude $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) tests/gtk/test_view.c
format:
	clang-format -i tests/gtk/test_view.c $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) $(UI_HEADERS) $(SOURCES) $(BIN_SOURCES) $(HEADERS) tests/unit/*.h $(TEST_SOURCES) tests/bench/*.c tests/bench/*.h $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE)
build/bench_lexical: $(SOURCES) $(BENCH_SOURCES) $(HEADERS) $(wildcard tests/bench/*.h) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) $(BENCH_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
bench: build/bench_lexical
	./build/bench_lexical --synthetic 50000
	./build/bench_lexical --synthetic 500000
	$(if $(BENCH_PATHS),./build/bench_lexical --paths $(BENCH_PATHS) --limit 500000)
build/bench_vector: src/core/common.c src/core/sort.c src/index/vector.c src/index/rank.c $(VECTOR_BENCH_SOURCE) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) src/core/common.c src/core/sort.c src/index/vector.c src/index/rank.c $(VECTOR_BENCH_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
bench-vector: build/bench_vector
	./build/bench_vector 50000
	./build/bench_vector 500000
build/torchlightd-release: $(SOURCES) src/bin/torchlightd.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/bench_fixture: src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
bench-daemon: build/torchlightd-release build/bench_fixture
	python3 tests/bench/bench_daemon.py ./build/torchlightd-release ./build/bench_fixture --sizes 50000 500000
clean:
	$(RM) -r build
-include $(OBJECTS:.o=.d) build/src/bin/torchlight.d build/src/bin/torchlightd.d

# Staged installs are reviewable with DESTDIR; prefix defaults to per-user tools.
PREFIX ?= $(HOME)/.local
DESTDIR ?=
.PHONY: install test-ui test-ui-isolated bench-ui
XVFB ?= Xvfb
XDOTOOL ?= xdotool
install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/applications $(DESTDIR)$(PREFIX)/lib/systemd/user
	install -m 755 build/torchlight build/torchlightd build/torchlight-gtk $(DESTDIR)$(PREFIX)/bin/
	install -m 644 packaging/org.torchlight.Launcher.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m 644 packaging/torchlightd.service $(DESTDIR)$(PREFIX)/lib/systemd/user/
test-ui: all build/test_popup_probe.so
	python3 tests/test_popup.py --xdotool $(XDOTOOL)
test-ui-isolated: all build/test_popup_probe.so build/test_popup_view
	python3 tests/run_popup_checks.py --xvfb $(XVFB) --xdotool $(XDOTOOL) --matrix
bench-ui: all build/test_popup_probe.so build/torchlightd-release build/bench_fixture
	python3 tests/run_popup_checks.py --xvfb $(XVFB) --xdotool $(XDOTOOL) --bench

# Isolated shared engine for the labeled mixed-catalog evaluator.
build/quality_engine.so: $(SOURCES) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG -fPIC -shared $(WARNINGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
