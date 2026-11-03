# avrasm - top-level Makefile
#
#   make               build build/libavrasm.a (and build/avrasm once src/main.c exists)
#   make test          build and run every unit test in tests/unit/
#   make test-arena    build and run just tests/unit/test_arena.c
#   make SAN=1 test    same, with AddressSanitizer + UBSan (catches leaks and overruns)
#   make CC="zig cc"   any C11 compiler works
#   make clean

CC     ?= cc
AR     ?= ar
BUILD  ?= build
SAN    ?= 0

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Iinclude -MMD -MP
ifeq ($(SAN),1)
CFLAGS  += -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LIB_SRC  := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ  := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))
LIB      := $(BUILD)/libavrasm.a
CLI      := $(BUILD)/avrasm

TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(TEST_SRC))

.PHONY: all test clean

all: $(LIB) $(if $(wildcard src/main.c),$(CLI))

$(LIB): $(LIB_OBJ)
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLI): $(BUILD)/obj/main.o $(LIB)
	$(CC) $^ $(LDFLAGS) -o $@

# One executable per tests/unit/test_*.c, linked against the library.
$(BUILD)/tests/%: tests/unit/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Itests/unit $< $(LIB) $(LDFLAGS) -o $@

# Run all tests, keep going after a failure, fail at the end if any failed.
test: $(TEST_BIN)
	@fail=0; for t in $(TEST_BIN); do \
	    echo "== $$t"; $$t || fail=1; \
	done; exit $$fail

# make test-arena  ->  build and run build/tests/test_arena
test-%: $(BUILD)/tests/test_%
	$<

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d $(BUILD)/tests/*.d)