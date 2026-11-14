# GNU Cedit (working name) - build system
#
#   make                  release build (./cedit)
#   make debug            -O0 -g build in build/debug
#   make asan             AddressSanitizer + UBSan build in build/asan
#   make test             unit tests (release flags)
#   make test-asan        unit tests under ASan + UBSan
#   make fuzz             libFuzzer targets (needs clang)
#   make bench            benchmarks
#   make install          PREFIX=/usr/local DESTDIR=
#   make LUA=1            enable Lua scripting (LUA_PC=lua5.4 to pick a version)

VERSION   := 1.0.0
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
MANDIR    ?= $(PREFIX)/share/man
DOCDIR    ?= $(PREFIX)/share/doc/cedit
DESTDIR   ?=

CC        ?= cc
BUILD     ?= release
LUA       ?= 0
LUA_PC    ?= lua

WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
             -Wmissing-prototypes -Wvla -Wpointer-arith -Wcast-qual
CPPFLAGS  += -Iinclude -D_XOPEN_SOURCE=700 -DCEDIT_VERSION='"$(VERSION)"'
CFLAGS    ?=
CFLAGS    += -std=c17 $(WARN)
LDLIBS    :=

ifeq ($(BUILD),release)
  CFLAGS   += -O2 -fstack-protector-strong
  CPPFLAGS += -DNDEBUG -D_FORTIFY_SOURCE=3
else ifeq ($(BUILD),debug)
  CFLAGS   += -O0 -g3
else ifeq ($(BUILD),asan)
  CFLAGS   += -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
              -fno-sanitize-recover=undefined
  LDFLAGS  += -fsanitize=address,undefined
else
  $(error BUILD must be release, debug or asan)
endif

ifeq ($(LUA),1)
  CPPFLAGS += -DCEDIT_LUA $(shell pkg-config --cflags $(LUA_PC))
  LDLIBS   += $(shell pkg-config --libs $(LUA_PC))
  BUILDDIR := build/$(BUILD)-lua
else
  BUILDDIR := build/$(BUILD)
endif

LIB_SRC := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILDDIR)/%.o)
MAIN_OBJ := $(BUILDDIR)/main.o
TEST_SRC := tests/test_main.c
DEPS    := $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)

ifeq ($(BUILD),release)
  TARGET := cedit
else
  TARGET := $(BUILDDIR)/cedit
endif

.PHONY: all debug asan test test-asan fuzz bench install uninstall clean

all: $(TARGET)

$(TARGET): $(LIB_OBJ) $(MAIN_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILDDIR):
	mkdir -p $@

debug:
	$(MAKE) BUILD=debug

asan:
	$(MAKE) BUILD=asan

$(BUILDDIR)/test_main: $(TEST_SRC) $(LIB_OBJ) tests/test.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(TEST_SRC) $(LIB_OBJ) $(LDLIBS) -o $@

test: $(BUILDDIR)/test_main
	$(BUILDDIR)/test_main

test-asan:
	$(MAKE) BUILD=asan test

$(BUILDDIR)/bench: tests/bench.c $(LIB_OBJ)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/bench.c $(LIB_OBJ) $(LDLIBS) -o $@

bench: $(BUILDDIR)/bench
	$(BUILDDIR)/bench

FUZZ_CFLAGS := -std=c17 -g -O1 -fsanitize=fuzzer,address,undefined -Iinclude \
               -D_XOPEN_SOURCE=700
fuzz:
	mkdir -p build/fuzz
	clang $(FUZZ_CFLAGS) tests/fuzz_input.c src/input.c src/utf8.c -o build/fuzz/fuzz_input
	clang $(FUZZ_CFLAGS) tests/fuzz_text.c src/text.c src/undo.c src/buffer.c \
	    src/utf8.c src/util.c src/syntax.c src/file.c -o build/fuzz/fuzz_text
	clang $(FUZZ_CFLAGS) tests/fuzz_json.c src/json.c src/utf8.c src/util.c \
	    -o build/fuzz/fuzz_json
	@echo "run: build/fuzz/fuzz_input -max_total_time=60"
	@echo "     build/fuzz/fuzz_text  -max_total_time=60"
	@echo "     build/fuzz/fuzz_json  -max_total_time=60"

install: cedit
	install -Dm755 cedit $(DESTDIR)$(BINDIR)/cedit
	install -Dm644 docs/cedit.1 $(DESTDIR)$(MANDIR)/man1/cedit.1
	install -Dm644 README.md $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 examples/config $(DESTDIR)$(DOCDIR)/examples/config
	install -Dm644 examples/init.lua $(DESTDIR)$(DOCDIR)/examples/init.lua

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/cedit $(DESTDIR)$(MANDIR)/man1/cedit.1
	rm -rf $(DESTDIR)$(DOCDIR)

clean:
	rm -rf build cedit

-include $(DEPS)
