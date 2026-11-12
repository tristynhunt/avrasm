# avrasm - top-level Makefile
#
#   make               build build/libavrasm.a, build/avrasm and build/avrasm-lsp
#   make test          build and run every unit test in tests/unit/
#   make test-arena    build and run just tests/unit/test_arena.c
#   make SAN=1 test    same, with AddressSanitizer + UBSan (catches leaks and overruns)
#   make lspcheck      start build/avrasm-lsp and check its answers, as an editor would
#   make CC="zig cc"   any C11 compiler works
#   make install       copy build/avrasm and build/avrasm-lsp to $(PREFIX)/bin (PREFIX=/usr/local.
#                      DESTDIR is put in front, for packaging). make uninstall takes them away
#   make dist          build both programs for every system the VS Code extension runs on with Zig into
#                      editors/vscode/bin/<system>/. make bundle puts just this systems there
#   make clean
#
# Linux: gcc or clang. macOS: the Xcode command line tools (xcode-select --install). Windows: MSYS2s
# UCRT64 shell with gcc and make (pacman -S mingw-w64-ucrt-x86_64-gcc make), the programs get .exe.
# SAN=1 needs a compiler with AddressSanitizer: gcc or clang on Linux, clang on macOS, not MinGW.

CC     ?= cc
AR     ?= ar
BUILD  ?= build
SAN    ?= 0
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Windows programs end in .exe (give EXE=.exe when building for Windows from another system), and
# MinGWs compiler is gcc: there may be no cc.

ifeq ($(OS),Windows_NT)
EXE ?= .exe
ifeq ($(origin CC),default)
CC := gcc
endif
endif

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Iinclude -MMD -MP
ifeq ($(SAN),1)
CFLAGS  += -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LIB_SRC  := $(filter-out src/main.c src/lsp_main.c,$(wildcard src/*.c))
LIB_OBJ  := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))
LIB      := $(BUILD)/libavrasm.a
CLI      := $(BUILD)/avrasm$(EXE)
LSP      := $(BUILD)/avrasm-lsp$(EXE)

TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(patsubst tests/unit/%.c,$(BUILD)/tests/%$(EXE),$(TEST_SRC))

.PHONY: all test clean lspcheck install uninstall dist bundle

all: $(LIB) $(CLI) $(LSP)

$(LIB): $(LIB_OBJ)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLI): $(BUILD)/obj/main.o $(LIB)
	$(CC) $^ $(LDFLAGS) -o $@

$(LSP): $(BUILD)/obj/lsp_main.o $(LIB)
	$(CC) $^ $(LDFLAGS) -o $@

# One executable per tests/unit/test_*.c, linked against the library.
$(BUILD)/tests/%$(EXE): tests/unit/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itests/unit $< $(LIB) $(LDFLAGS) -o $@

# Run all tests, keep going after a failure, fail at the end if any failed.
test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do \
	    echo "== $$t"; $$t || fail=1; \
	done; exit $$fail

# make test-arena  ->  build and run build/tests/test_arena
test-%: $(BUILD)/tests/test_%$(EXE)
	$<

# Talk to the language server through stdin and stdout, as an editor does (tests/tools/lspcheck.py).
lspcheck: all
	python3 tests/tools/lspcheck.py $(LSP)

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(CLI) $(LSP) "$(DESTDIR)$(BINDIR)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/avrasm$(EXE)" "$(DESTDIR)$(BINDIR)/avrasm-lsp$(EXE)"

DIST_DIR ?= editors/vscode/bin
DIST     := linux-x64:x86_64-linux-musl linux-arm64:aarch64-linux-musl darwin-x64:x86_64-macos \
            darwin-arm64:aarch64-macos win32-x64:x86_64-windows-gnu win32-arm64:aarch64-windows-gnu

dist:
	@for d in $(DIST); do \
	  name=$${d%%:*}; target=$${d#*:}; exe=; \
	  case $$name in win32-*) exe=.exe;; esac; \
	  echo "== $$name ($$target)"; \
	  $(MAKE) --no-print-directory BUILD=$(BUILD)/dist/$$name CC="zig cc -target $$target" AR="zig ar" \
	    EXE=$$exe SAN=0 LDFLAGS=-s all || exit 1; \
	  mkdir -p $(DIST_DIR)/$$name; \
	  cp $(BUILD)/dist/$$name/avrasm$$exe $(BUILD)/dist/$$name/avrasm-lsp$$exe $(DIST_DIR)/$$name/ || exit 1; \
	done

# only this systems programs from the ordinary build
HOST_OS   := $(shell uname -s 2>/dev/null)
HOST_ARCH := $(shell uname -m 2>/dev/null)
NODE_OS   := $(if $(filter Linux,$(HOST_OS)),linux,$(if $(filter Darwin,$(HOST_OS)),darwin,win32))
NODE_ARCH := $(if $(filter x86_64 amd64,$(HOST_ARCH)),x64,$(if $(filter aarch64 arm64,$(HOST_ARCH)),arm64,$(HOST_ARCH)))

bundle: all
	mkdir -p $(DIST_DIR)/$(NODE_OS)-$(NODE_ARCH)
	cp $(CLI) $(LSP) $(DIST_DIR)/$(NODE_OS)-$(NODE_ARCH)/

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d $(BUILD)/tests/*.d)