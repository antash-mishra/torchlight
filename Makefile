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
          src/index/trigram.c src/index/typo.c src/index/dirtree.c src/index/place.c src/index/lexical.c \
          src/index/embed.c src/index/potion.c src/index/vector.c src/index/rank.c src/index/usage.c \
          src/index/desktop.c src/index/lexical_query.c src/index/catalog.c src/fs/crawl.c src/fs/watch.c src/fs/mounts.c src/storage/store.c \
          src/ipc/ipc.c src/ipc/async.c src/ipc/client.c src/service/delta.c src/service/ledger.c src/service/personal.c src/service/writer.c src/service/semantic.c src/service/daemon.c
BIN_SOURCES = src/bin/torchlight.c src/bin/torchlightd.c
OBJECTS = $(SOURCES:%.c=build/%.o)
HEADERS = $(wildcard include/torchlight/*.h) $(wildcard src/*/*.h)
TEST_SOURCES = $(wildcard tests/unit/test_*.c)
BENCH_SOURCES = tests/bench/bench_lexical.c tests/bench/corpus.c tests/bench/queries.c \
                tests/bench/stats.c tests/bench/personal.c
VECTOR_BENCH_SOURCE = tests/bench/bench_vector.c
VECTOR_EVAL_SOURCE = tests/bench/eval_vector.c
FIXTURE_SOURCE = tests/bench/fixture/export.c
DESKTOP_FIXTURE_SOURCE = tests/fixtures/desktop_replace.c
SEMANTIC_FIXTURE_SOURCE = tests/fixtures/semantic_stall.c
ALLOC_SOURCE = tests/alloc/query.c
# Developer-only LD_PRELOAD CPU sampler (tests/bench/profiler), never shipped.
SAMPLER_SOURCE = tests/bench/profiler/sampler.c
# Optional NUL-separated real path list for `make bench` (see scripts/make_corpus.sh).
BENCH_PATHS ?=
SAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
.PHONY: all headless test lint lint-headless format bench bench-vector eval-vector bench-daemon profile-daemon bench-scenarios clean
# Only GTK recipes expand these, so headless targets never require GTK, X11 or Wayland.
GTK_CPPFLAGS = $(subst -I,-isystem ,$(shell $(PKG_CONFIG) --cflags 'gtk4 >= 4.14' x11 wayland-client)) \
               -isystem $(WAYLAND_GENERATED)
GTK_LDLIBS = $(shell $(PKG_CONFIG) --libs 'gtk4 >= 4.14' x11 wayland-client)
# Client code for the compositors' task bar protocols (ADR 0052), generated from the files
# vendored in third_party/wayland-protocols. COSMIC's files mark old messages with an
# attribute that Ubuntu 24.04's wayland-scanner predates; it warns but generates the same
# code, so its output is shown only when it fails.
WAYLAND_SCANNER ?= $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_GENERATED = build/ui/wayland
WAYLAND_PROTOCOLS = wlr-foreign-toplevel-management-unstable-v1 ext-foreign-toplevel-list-v1 \
                    ext-workspace-v1 cosmic-workspace-unstable-v1 cosmic-toplevel-info-unstable-v1 \
                    cosmic-toplevel-management-unstable-v1
WAYLAND_CODE = $(WAYLAND_PROTOCOLS:%=$(WAYLAND_GENERATED)/%-protocol.c)
WAYLAND_HEADERS = $(WAYLAND_PROTOCOLS:%=$(WAYLAND_GENERATED)/%-client.h)
$(WAYLAND_GENERATED)/%-protocol.c: third_party/wayland-protocols/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) private-code $< $@ 2>$@.log || { cat $@.log; exit 1; }
$(WAYLAND_GENERATED)/%-client.h: third_party/wayland-protocols/%.xml
	@mkdir -p $(@D)
	$(WAYLAND_SCANNER) client-header $< $@ 2>$@.log || { cat $@.log; exit 1; }
# The presentation's widgets, which the native view test builds on its own.
VIEW_SOURCES = ui/gtk/view.c ui/gtk/path_label.c ui/gtk/selection_track.c ui/gtk/header.c ui/gtk/index_strip.c \
               ui/gtk/preview.c
# The open-window sources, one per kind of session (ADR 0052), and how one is chosen.
WINDOW_SOURCES = ui/gtk/windows.c ui/gtk/windows_x11.c ui/gtk/windows_shell.c ui/gtk/windows_kwin.c \
                 ui/gtk/windows_track.c ui/gtk/windows_wayland.c ui/gtk/windows_session.c
UI_SOURCES = ui/gtk/model.c $(WINDOW_SOURCES) ui/gtk/actions.c ui/gtk/launcher.c $(VIEW_SOURCES) \
             ui/gtk/wayland_globals.c src/bin/torchlight-gtk.c
POPUP_FIXTURE_SOURCE = tests/fixtures/popup_probe.c
WINDOW_FIXTURE_SOURCE = tests/fixtures/window_app.c
# Lists and switches windows through the session's window source, for the desktop tests.
WINDOW_SOURCES_FIXTURE = tests/fixtures/window_sources.c
all: headless build/torchlight-gtk
# Daemon and CLI only, for servers and systems without GTK 4.14 (ADR 0037).
headless: build/torchlight build/torchlightd
$(FRIZBEE_LIB): $(FRIZBEE_INPUTS)
	$(CARGO) build --release --offline --locked --manifest-path $(FRIZBEE_MANIFEST) --target-dir build/frizbee
	@touch $@
UI_HEADERS = $(wildcard ui/gtk/*.h)
# Each bundle compiles to one string literal and ISO C guarantees only 4095 bytes, so
# the styles are split across bundles.
UI_RESOURCES = build/ui/gtk/resources.c build/ui/gtk/palettes.c
build/ui/gtk/resources.c: ui/gtk/resources.xml ui/gtk/popup.css
	@mkdir -p $(@D)
	glib-compile-resources --sourcedir=ui/gtk --generate-source --target=$@ --c-name=torchlight_ui $<
build/ui/gtk/palettes.c: ui/gtk/palettes.xml ui/gtk/palette-light.css ui/gtk/palette-dark.css
	@mkdir -p $(@D)
	glib-compile-resources --sourcedir=ui/gtk --generate-source --target=$@ --c-name=torchlight_palettes $<
build/torchlight-gtk: $(OBJECTS) $(UI_SOURCES) $(UI_HEADERS) $(UI_RESOURCES) $(WAYLAND_CODE) $(WAYLAND_HEADERS) $(HEADERS) $(FRIZBEE_LIB)
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(CFLAGS) $(WARNINGS) $(UI_SOURCES) $(UI_RESOURCES) $(WAYLAND_CODE) $(OBJECTS) $(LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
build/test_popup_view: $(VIEW_SOURCES) tests/gtk/test_view.c $(UI_HEADERS) $(UI_RESOURCES) $(FRIZBEE_LIB)
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(CFLAGS) $(WARNINGS) $(SAN_FLAGS) $(VIEW_SOURCES) tests/gtk/test_view.c $(UI_RESOURCES) $(LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
build/test_popup_probe.so: $(POPUP_FIXTURE_SOURCE)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) -fPIC -shared $< $(GTK_LDLIBS) -ldl -o $@
build/test_window_app: $(WINDOW_FIXTURE_SOURCE)
	@mkdir -p build
	$(CC) $(GTK_CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $< $(shell $(PKG_CONFIG) --libs x11) -o $@
build/test_window_sources: $(WINDOW_SOURCES_FIXTURE) $(WINDOW_SOURCES) $(UI_HEADERS) $(HEADERS) $(WAYLAND_CODE) $(WAYLAND_HEADERS)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $< $(WINDOW_SOURCES) $(WAYLAND_CODE) src/core/json.c src/core/common.c $(LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
build/torchlight: $(OBJECTS) build/src/bin/torchlight.o $(FRIZBEE_LIB)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/torchlightd: $(OBJECTS) build/src/bin/torchlightd.o $(FRIZBEE_LIB)
	$(CC) $(CFLAGS) $(WARNINGS) $^ $(LDFLAGS) $(LDLIBS) -o $@
build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@
# The popup's GTK-free parts, unit-tested with the engine.
UNIT_UI_SOURCES = ui/gtk/model.c ui/gtk/windows.c ui/gtk/windows_shell.c ui/gtk/windows_kwin.c \
                  ui/gtk/windows_track.c ui/gtk/actions.c
UNIT_UI_HEADERS = ui/gtk/actions.h ui/gtk/windows_shell.h ui/gtk/windows_kwin.h ui/gtk/windows_track.h
build/tests: $(SOURCES) $(UNIT_UI_SOURCES) $(TEST_SOURCES) $(HEADERS) $(UNIT_UI_HEADERS) tests/unit/test.h $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g $(WARNINGS) $(SAN_FLAGS) $(SOURCES) $(UNIT_UI_SOURCES) $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
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
	python3 tests/test_headless.py $(MAKE)
lint-headless:
	@command -v $(CLANG_TIDY) >/dev/null || { echo 'clang-tidy is required'; exit 1; }
	@command -v $(CPPCHECK) >/dev/null || { echo 'cppcheck is required'; exit 1; }
	$(CLANG_TIDY) $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(VECTOR_BENCH_SOURCE) $(VECTOR_EVAL_SOURCE) $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE) $(SAMPLER_SOURCE) --warnings-as-errors='*' -- $(CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -D_GNU_SOURCE -Iinclude $(SOURCES) $(BIN_SOURCES) $(TEST_SOURCES) $(BENCH_SOURCES) $(VECTOR_BENCH_SOURCE) $(VECTOR_EVAL_SOURCE) $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE) $(SAMPLER_SOURCE)
lint: lint-headless $(WAYLAND_HEADERS)
	$(CLANG_TIDY) $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) $(WINDOW_FIXTURE_SOURCE) $(WINDOW_SOURCES_FIXTURE) tests/gtk/test_view.c --warnings-as-errors='*' -- $(CPPFLAGS) $(GTK_CPPFLAGS) -std=c17 $(WARNINGS)
	$(CPPCHECK) --enable=warning,performance,portability --error-exitcode=1 --std=c17 --suppress=missingIncludeSystem -D_GNU_SOURCE --library=gtk -Iinclude $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) $(WINDOW_FIXTURE_SOURCE) $(WINDOW_SOURCES_FIXTURE) tests/gtk/test_view.c
format:
	clang-format -i tests/gtk/test_view.c $(UI_SOURCES) $(POPUP_FIXTURE_SOURCE) $(WINDOW_FIXTURE_SOURCE) $(WINDOW_SOURCES_FIXTURE) $(UI_HEADERS) $(SOURCES) $(BIN_SOURCES) $(HEADERS) tests/unit/*.h $(TEST_SOURCES) tests/bench/*.c tests/bench/*.h $(FIXTURE_SOURCE) $(DESKTOP_FIXTURE_SOURCE) $(SEMANTIC_FIXTURE_SOURCE) $(ALLOC_SOURCE) $(SAMPLER_SOURCE)
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
	./build/bench_vector 50000 --int8
	./build/bench_vector 500000 --int8
	./build/bench_vector 50000 --prefix
	./build/bench_vector 500000 --prefix
# Shortlist recall on trained embeddings; needs an exported model, e.g.
# make eval-vector MODEL=build/models/potion-256.tlm (see docs/m4-model-evaluation.md).
MODEL ?= build/models/potion-256.tlm
build/eval_vector: $(SOURCES) tests/bench/corpus.c tests/bench/queries.c $(VECTOR_EVAL_SOURCE) $(HEADERS) $(wildcard tests/bench/*.h) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) tests/bench/corpus.c tests/bench/queries.c $(VECTOR_EVAL_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
build/semantic_queries.txt: tests/quality/semantic.json
	@mkdir -p build
	python3 -c 'import json,sys; print("\n".join(q["text"] for q in json.load(open(sys.argv[1]))["queries"]))' $< > $@
eval-vector: build/eval_vector build/semantic_queries.txt
	./build/eval_vector $(MODEL) 50000 build/semantic_queries.txt
	./build/eval_vector $(MODEL) 500000 build/semantic_queries.txt
build/torchlightd-release: $(SOURCES) src/bin/torchlightd.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/bench_fixture: src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG $(WARNINGS) src/core/common.c src/core/vec.c src/core/json.c tests/bench/corpus.c tests/bench/queries.c $(FIXTURE_SOURCE) $(LDFLAGS) $(LDLIBS) -o $@
bench-daemon: build/torchlightd-release build/bench_fixture
	python3 tests/bench/bench_daemon.py ./build/torchlightd-release ./build/bench_fixture --sizes 50000 500000
# M7 indexing scenarios (docs/m7-plan.md): an -O3 daemon with frame pointers
# plus the LD_PRELOAD sampler, for per-thread profiles where perf is locked.
PROFILE_CFLAGS = -std=c17 -O3 -DNDEBUG -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
build/profile/torchlightd: $(SOURCES) src/bin/torchlightd.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PROFILE_CFLAGS) $(WARNINGS) $(SOURCES) src/bin/torchlightd.c $(LDFLAGS) $(LDLIBS) -o $@
build/profile/sampler.so: $(SAMPLER_SOURCE)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c17 -O2 -g $(WARNINGS) -fPIC -shared $< -ldl -o $@
profile-daemon: build/profile/torchlightd build/profile/sampler.so
# SCENARIO_SIZE sizes the synthetic corpus; BENCH_PATHS mirrors a real path
# list instead. Results go to tests/bench/results/<date>-m7-$(SCENARIO_LABEL).jsonl.
SCENARIO_SIZE ?= 500000
SCENARIO_LABEL ?= scenarios
SCENARIO_PHASES ?=
bench-scenarios: profile-daemon build/bench_fixture
	python3 tests/bench/bench_scenarios.py --daemon build/profile/torchlightd \
		--sampler build/profile/sampler.so --label $(SCENARIO_LABEL) \
		$(if $(BENCH_PATHS),--paths $(BENCH_PATHS),--fixture build/bench_fixture --size $(SCENARIO_SIZE)) \
		$(SCENARIO_PHASES)
clean:
	$(RM) -r build
-include $(OBJECTS:.o=.d) build/src/bin/torchlight.d build/src/bin/torchlightd.d

# Torchlight's GNOME Shell and Cinnamon extensions, which list windows on those desktops'
# Wayland sessions (ADR 0051, ADR 0054); both use the same UUID.
GNOME_EXTENSION = torchlight@antash-mishra.github.io
# Staged installs are reviewable with DESTDIR; prefix defaults to per-user tools.
PREFIX ?= $(HOME)/.local
DESTDIR ?=
.PHONY: install install-headless test-ui test-ui-isolated test-ui-wayland test-ui-gnome test-ui-desktops bench-ui
XVFB ?= Xvfb
XDOTOOL ?= xdotool
install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/applications $(DESTDIR)$(PREFIX)/lib/systemd/user
	install -m 755 build/torchlight build/torchlightd build/torchlight-gtk $(DESTDIR)$(PREFIX)/bin/
	install -m 644 packaging/org.torchlight.Launcher.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m 644 packaging/torchlightd.service $(DESTDIR)$(PREFIX)/lib/systemd/user/
	install -d $(DESTDIR)$(PREFIX)/share/fonts/torchlight
	install -m 644 third_party/fonts/schibsted-grotesk/SchibstedGrotesk-VF.ttf third_party/fonts/bedstead/bedstead.otf $(DESTDIR)$(PREFIX)/share/fonts/torchlight/
	install -d $(DESTDIR)$(PREFIX)/share/gnome-shell/extensions/$(GNOME_EXTENSION)
	install -m 644 ui/gnome-shell/$(GNOME_EXTENSION)/metadata.json ui/gnome-shell/$(GNOME_EXTENSION)/extension.js $(DESTDIR)$(PREFIX)/share/gnome-shell/extensions/$(GNOME_EXTENSION)/
	install -d $(DESTDIR)$(PREFIX)/share/cinnamon/extensions/$(GNOME_EXTENSION)
	install -m 644 ui/cinnamon/$(GNOME_EXTENSION)/metadata.json ui/cinnamon/$(GNOME_EXTENSION)/extension.js $(DESTDIR)$(PREFIX)/share/cinnamon/extensions/$(GNOME_EXTENSION)/
# Same unit name, but started with the user manager instead of a graphical session.
install-headless: headless
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/systemd/user
	install -m 755 build/torchlight build/torchlightd $(DESTDIR)$(PREFIX)/bin/
	install -m 644 packaging/torchlightd-headless.service $(DESTDIR)$(PREFIX)/lib/systemd/user/torchlightd.service
# Optimized, hardened binaries for packages (docs/release.md); `make` stays a debug build.
# Ubuntu's hardening flags, without link-time optimization.
RELEASE_CFLAGS = -std=c17 -O2 -g -pthread -fno-omit-frame-pointer -fstack-protector-strong \
                 -fstack-clash-protection -fcf-protection -D_FORTIFY_SOURCE=3 -DNDEBUG
RELEASE_LDFLAGS = -Wl,-z,relro,-z,now
.PHONY: release deb
release: build/release/torchlight build/release/torchlightd build/release/torchlight-gtk
build/release/torchlight build/release/torchlightd: build/release/%: $(SOURCES) src/bin/%.c $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $(WARNINGS) $(SOURCES) src/bin/$*.c $(LDFLAGS) $(RELEASE_LDFLAGS) $(LDLIBS) -o $@
build/release/torchlight-gtk: $(SOURCES) $(UI_SOURCES) $(UI_HEADERS) $(UI_RESOURCES) $(WAYLAND_CODE) $(WAYLAND_HEADERS) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(GTK_CPPFLAGS) $(RELEASE_CFLAGS) $(WARNINGS) $(UI_SOURCES) $(UI_RESOURCES) $(WAYLAND_CODE) $(SOURCES) $(LDFLAGS) $(RELEASE_LDFLAGS) $(GTK_LDLIBS) $(LDLIBS) -o $@
# build/deb/torchlight_VERSION_ARCH.deb; needs dpkg-dev.
deb: release
	packaging/deb/build_deb.sh build/deb
# Publishing a signed APT repository, checked with apt as an ordinary user.
.PHONY: test-packaging
test-packaging:
	python3 tests/test_packaging.py
test-ui: all build/test_popup_probe.so build/test_window_app
	python3 tests/test_popup.py --xdotool $(XDOTOOL)
test-ui-isolated: all build/test_popup_probe.so build/test_popup_view build/test_window_app build/test_window_sources
	python3 tests/run_popup_checks.py --xvfb $(XVFB) --xdotool $(XDOTOOL) --matrix
	# A wide screen gives the popup room for its preview panel (ADR 0048).
	python3 tests/run_popup_checks.py --xvfb $(XVFB) --xdotool $(XDOTOOL) --screen 1280x800x24
	python3 tests/test_popup_wayland.py
	python3 tests/test_windows_gnome_shell.py
	python3 tests/test_windows_desktops.py
# Native Wayland on a headless Weston (ADR 0049); skips when Weston is not installed.
test-ui-wayland: all
	python3 tests/test_popup_wayland.py
# Open windows on GNOME's Wayland session, on a headless GNOME Shell (ADR 0051); skips
# when GNOME Shell is not installed.
test-ui-gnome: all
	python3 tests/test_windows_gnome_shell.py
# Open windows on Sway, labwc, COSMIC, KWin and Cinnamon (ADR 0052 to 0054); skips each
# desktop that is not installed (see the test's header for unpacked ones).
test-ui-desktops: all build/test_window_sources
	python3 tests/test_windows_desktops.py
bench-ui: all build/test_popup_probe.so build/torchlightd-release build/bench_fixture
	python3 tests/run_popup_checks.py --xvfb $(XVFB) --xdotool $(XDOTOOL) --bench

# Isolated shared engine for the labeled mixed-catalog evaluator.
build/quality_engine.so: $(SOURCES) $(HEADERS) $(FRIZBEE_LIB)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c17 -O3 -DNDEBUG -fPIC -shared $(WARNINGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
