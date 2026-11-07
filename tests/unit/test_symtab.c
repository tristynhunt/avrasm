#include "avrasm/symtab.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

static Span at(uint32_t start, uint32_t end)
{
  Span s = { 0, start, end };
  return s;
}

int main(void)
{
  Arena arena;
  arenaInit(&arena, 0);

  {
    Symtab t;
    symtabInit(&t, &arena, 1);
    CHECK(symtabFind(&t, "loop", 4) == NULL);
    Symbol *a = symtabAdd(&t, "loop", 4, SYM_LABEL, 42, at(0, 4), NULL);
    REQUIRE(a != NULL);
    CHECK_STR(a->name, "loop");
    CHECK_INT(a->len, 4);
    CHECK_INT(a->value, 42);
    CHECK_INT(a->kind, SYM_LABEL);
    CHECK_INT(a->defined.end, 4);
    const Symbol *f = symtabFind(&t, "loop", 4);
    CHECK(f == a);
    CHECK_INT(t.count, 1);
    Symbol *b = symtabAdd(&t, "SIZE", 4, SYM_EQU, -7, at(10, 14), NULL);
    REQUIRE(b != NULL);
    CHECK_INT(symtabFind(&t, "SIZE", 4)->value, -7);
    CHECK_INT(symtabFind(&t, "loop", 4)->value, 42);
    CHECK_INT(t.count, 2);
  }

  {
    Symtab t;
    symtabInit(&t, &arena, 1);
    const char *text = "FOOBAR FOO";
    symtabAdd(&t, text, 3, SYM_EQU, 1, at(0, 3), NULL);
    CHECK(symtabFind(&t, "FOO", 3) != NULL);
    CHECK(symtabFind(&t, text, 6) == NULL);
    CHECK(symtabFind(&t, text + 7, 3) != NULL);
    CHECK(symtabFind(&t, "FO", 2) == NULL);
    CHECK(symtabFind(&t, "FOOO", 4) == NULL);
    CHECK(symtabFind(&t, "", 0) == NULL);
    CHECK_STR(symtabFind(&t, "FOO", 3)->name, "FOO");
  }

  {
    Symtab t;
    symtabInit(&t, &arena, 1);
    Symbol *a = symtabAdd(&t, "x", 1, SYM_LABEL, 1, at(0, 1), NULL);
    const Symbol *existing = NULL;
    Symbol *b = symtabAdd(&t, "x", 1, SYM_EQU, 2, at(5, 6), &existing);
    CHECK(b == NULL);
    CHECK(existing == a);
    CHECK_INT(existing->value, 1);
    CHECK_INT(existing->defined.start, 0);
    CHECK_INT(t.count, 1);
    CHECK(symtabAdd(&t, "x", 1, SYM_EQU, 3, at(7, 8), NULL) == NULL);
  }

  {
    Symtab t;
    symtabInit(&t, &arena, 0);
    symtabAdd(&t, "Loop", 4, SYM_LABEL, 5, at(0, 4), NULL);
    CHECK(symtabFind(&t, "loop", 4) != NULL);
    CHECK(symtabFind(&t, "LOOP", 4) != NULL);
    CHECK_STR(symtabFind(&t, "lOOp", 4)->name, "Loop");
    const Symbol *existing = NULL;
    CHECK(symtabAdd(&t, "LOOP", 4, SYM_LABEL, 6, at(9, 13), &existing) == NULL);
    CHECK_STR(existing->name, "Loop");
    CHECK(symtabFind(&t, "Loo", 3) == NULL);

    Symtab s;
    symtabInit(&s, &arena, 1);
    symtabAdd(&s, "Loop", 4, SYM_LABEL, 5, at(0, 4), NULL);
    CHECK(symtabFind(&s, "loop", 4) == NULL);
    CHECK(symtabFind(&s, "Loop", 4) != NULL);
    CHECK(symtabAdd(&s, "loop", 4, SYM_LABEL, 6, at(9, 13), NULL) != NULL);
    CHECK_INT(s.count, 2);
    symtabAdd(&t, "a@", 2, SYM_EQU, 1, at(0, 2), NULL);
    CHECK(symtabFind(&t, "a`", 2) == NULL);
    symtabAdd(&t, "\xc3\xa9", 2, SYM_EQU, 2, at(0, 2), NULL);
    CHECK(symtabFind(&t, "\xc3\xa9", 2) != NULL);
  }

  {
    enum { N = 20000 };
    Symtab t;
    symtabInit(&t, &arena, 0);
    char name[32];
    int ok = 1;
    for (int i = 0; i < N; i++) {
      int n = snprintf(name, sizeof name, "sym_%d", i);
      if (symtabAdd(&t, name, (size_t)n, SYM_EQU, i * 3, at((uint32_t)i, (uint32_t)i + 1), NULL) == NULL) ok = 0;
    }
    CHECK(ok);
    CHECK_INT(t.count, N);
    CHECK(t.bucketCount >= 1024);
    CHECK_INT(t.bucketCount & (t.bucketCount - 1), 0);
    ok = 1;
    for (int i = 0; i < N; i++) {
      int n = snprintf(name, sizeof name, "SYM_%d", i);
      const Symbol *s = symtabFind(&t, name, (size_t)n);
      if (s == NULL || s->value != i * 3 || s->defined.start != (uint32_t)i) ok = 0;
    }
    CHECK(ok);
    CHECK(symtabFind(&t, "sym_20000", 9) == NULL);
    CHECK(symtabFind(&t, "sym_", 4) == NULL);
  }

  {
    Symtab t;
    symtabInit(&t, &arena, 0);
    symtabAdd(&t, "start", 5, SYM_LABEL, 100, at(0, 5), NULL);
    int64_t v = -1;
    CHECK(symtabLookup(&t, "START", 5, &v));
    CHECK_INT(v, 100);
    v = -1;
    CHECK(!symtabLookup(&t, "nope", 4, &v));
    CHECK_INT(v, -1);
  }

  arenaFree(&arena);
  return TEST_RESULT();
}