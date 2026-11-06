#include "avrasm/expr.h"
#include "check.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Table {
  uint32_t lookups;
} Table;

static int lookup(void *user, const char *name, size_t len, int64_t *value)
{
  static const struct {
    const char *name;
    int64_t value;
  } syms[] = {
    { "FOO", 10 }, { "BAR", 0x100 }, { "NEG", -5 }, { "MAXI", INT64_MAX }, { "MINI", INT64_MIN },
  };
  ((Table *)user)->lookups++;
  for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
    if (strlen(syms[i].name) == len && memcmp(syms[i].name, name, len) == 0) {
      *value = syms[i].value;
      return 1;
    }
  }
  return 0;
}

typedef struct Out {
  char text[64];
  uint32_t errors;
  Span firstError;
  uint32_t lookups;
} Out;

static void runWith(Out *o, const char *src, size_t len, int allowUndefined, int64_t pc)
{
  Arena arena;
  arenaInit(&arena, 0);
  SourceMap map;
  sourceMapInit(&map, &arena);
  DiagList diags;
  diagListInit(&diags, &arena);
  FileId id = sourceMapAdd(&map, "t.asm", src, len);
  Lexer lx;
  lexerInit(&lx, &map, id, &diags);
  TokenLine line = lexLine(&lx, &arena);
  TokenCursor cur;
  cursorInit(&cur, &line);

  Table table = { 0 };
  ExprEnv env = { &map, &diags, lookup, &table, allowUndefined, pc };
  ExprValue v = { 123, 1 };
  int ok = exprEval(&cur, &env, &v);

  if (!ok) {
    snprintf(o->text, sizeof o->text, "error");
    CHECK_INT(v.value, 0);
    CHECK_INT(v.known, 0);
    CHECK(diags.errorCount >= 1);
  } else if (!v.known) {
    snprintf(o->text, sizeof o->text, "?");
  } else {
    snprintf(o->text, sizeof o->text, "%" PRId64, v.value);
  }
  if (ok) CHECK_INT(diags.errorCount, 0);
  if (ok && !cursorAtEnd(&cur)) {
    size_t used = strlen(o->text);
    snprintf(o->text + used, sizeof o->text - used, " @%s", tokenKindName(cursorPeek(&cur)->kind));
  }
  o->errors = diags.errorCount;
  o->firstError = diags.head != NULL ? diags.head->span : (Span){ FILE_NONE, 0, 0 };
  o->lookups = table.lookups;
  arenaFree(&arena);
}

static char *repeat(const char *unit, size_t times, const char *mid, const char *close,
                    size_t closeTimes)
{
  size_t ul = strlen(unit), ml = strlen(mid), cl = strlen(close);
  char *s = malloc(ul * times + ml + cl * closeTimes + 1);
  if (s == NULL) return NULL;
  char *p = s;
  for (size_t i = 0; i < times; i++, p += ul) memcpy(p, unit, ul);
  memcpy(p, mid, ml);
  p += ml;
  for (size_t i = 0; i < closeTimes; i++, p += cl) memcpy(p, close, cl);
  *p = '\0';
  return s;
}

static const char *ev(const char *src)
{
  static Out o;
  runWith(&o, src, strlen(src), 0, 0);
  return o.text;
}

static const char *evUndef(const char *src)
{
  static Out o;
  runWith(&o, src, strlen(src), 1, 0);
  return o.text;
}

#define CHECK_ERR(src, s, e) \
  do { \
    Out o_; \
    runWith(&o_, src, strlen(src), 0, 0); \
    CHECK_STR(o_.text, "error"); \
    CHECK_INT(o_.errors, 1); \
    CHECK_INT(o_.firstError.start, s); \
    CHECK_INT(o_.firstError.end, e); \
  } while (0)

int main(void)
{
  CHECK_STR(ev("42"), "42");
  CHECK_STR(ev("0x10 + $10 + 0b10 + 010"), "42");
  CHECK_STR(ev("'A' + 1"), "66");
  CHECK_STR(ev("'a' - 'A'"), "32");
  CHECK_STR(ev("'\\n'"), "10");
  CHECK_STR(ev("2147483647"), "2147483647");
  CHECK_STR(ev("2147483648"), "-2147483648");
  CHECK_STR(ev("0x80000000"), "-2147483648");
  CHECK_STR(ev("4294967295"), "-1");
  CHECK_STR(ev("0xFFFFFFFFFFFFFFFF"), "-1");
  CHECK_STR(ev("0x8000000000000000"), "0");
  CHECK_STR(ev("9223372036854775807"), "-1");

  CHECK_STR(ev("1 + 2 * 3"), "7");
  CHECK_STR(ev("(1 + 2) * 3"), "9");
  CHECK_STR(ev("10 - 3 - 2"), "5");
  CHECK_STR(ev("100 / 10 / 5"), "2");
  CHECK_STR(ev("2 << 3 + 1"), "32");
  CHECK_STR(ev("1 | 2 ^ 3 & 4"), "3");
  CHECK_STR(ev("1 | 3 ^ 3"), "1");
  CHECK_STR(ev("3 ^ 1 | 1"), "3");
  CHECK_STR(ev("1 ^ 3 & 2"), "3");
  CHECK_STR(ev("6 & 3 | 8"), "10");
  CHECK_STR(ev("2 & 2 == 2"), "0");
  CHECK_STR(ev("1 + 1 == 2"), "1");
  CHECK_STR(ev("1 << 1 < 3"), "1");
  CHECK_STR(ev("5 > 3 && 2 > 1"), "1");
  CHECK_STR(ev("0 || 5"), "1");
  CHECK_STR(ev("5 && 6"), "1");
  CHECK_STR(ev("0 && 0 || 1"), "1");
  CHECK_STR(ev("1 || 0 && 0"), "1");
  CHECK_STR(ev("1 < 2 == 1"), "1");
  CHECK_STR(ev("2 * 3 % 4"), "2");

  CHECK_STR(ev("-3"), "-3");
  CHECK_STR(ev("- -3"), "3");
  CHECK_STR(ev("-2 * 3"), "-6");
  CHECK_STR(ev("-2 * -3"), "6");
  CHECK_STR(ev("~0"), "-1");
  CHECK_STR(ev("~1 + 1"), "-1");
  CHECK_STR(ev("~-1"), "0");
  CHECK_STR(ev("!0"), "1");
  CHECK_STR(ev("!5"), "0");
  CHECK_STR(ev("!!7"), "1");
  CHECK_STR(ev("!1 + 1"), "1");
  CHECK_STR(ev("-(2 + 3)"), "-5");

  CHECK_STR(ev("-7 / 2"), "-3");
  CHECK_STR(ev("7 / -2"), "-3");
  CHECK_STR(ev("-7 % 3"), "-1");
  CHECK_STR(ev("7 % -3"), "1");
  CHECK_STR(ev("1 << 40 >> 30"), "1024");
  CHECK_STR(ev("1 << 40"), "0");
  CHECK_STR(ev("1 << 31"), "-2147483648");
  CHECK_STR(ev("4294967296 >> 1"), "-2147483648");
  CHECK_STR(ev("(2147483647 + 1) / 2"), "1073741824");
  CHECK_STR(ev("(1 << 32) >> 1"), "-2147483648");
  CHECK_STR(ev("2147483648 > 0"), "1");
  CHECK_STR(ev("0xFFFFFFFF > 0"), "1");
  CHECK_STR(ev("0xFFFFFFFF / 2"), "2147483647");
  CHECK_STR(ev("0xFFFFFFFF >> 4"), "268435455");
  CHECK_STR(ev("-2147483648 / -1"), "-2147483648");
  CHECK_STR(ev("256 >> 4"), "16");
  CHECK_STR(ev("1 < 2"), "1");
  CHECK_STR(ev("2 < 1"), "0");
  CHECK_STR(ev("2 <= 2"), "1");
  CHECK_STR(ev("3 >= 4"), "0");
  CHECK_STR(ev("4 >= 4"), "1");
  CHECK_STR(ev("5 >= 4"), "1");
  CHECK_STR(ev("4 > 4"), "0");
  CHECK_STR(ev("0x100 == 0"), "0");
  CHECK_STR(ev("0x100 != 0"), "1");
  CHECK_STR(ev("0x100 == 0x100"), "1");
  CHECK_STR(ev("0x100000000 == 0"), "0");
  CHECK_STR(ev("-1 < 0"), "1");
  CHECK_STR(ev("1 == 1"), "1");
  CHECK_STR(ev("1 != 1"), "0");
  CHECK_STR(ev("MAXI + 1"), "0");
  CHECK_STR(ev("MINI - 1"), "-1");
  CHECK_STR(ev("MAXI * 2"), "-2");
  CHECK_STR(ev("-MINI"), "0");
  CHECK_STR(ev("MINI / -1"), "0");
  CHECK_STR(ev("MINI % -1"), "0");
  CHECK_STR(ev("(MAXI + 1) >> 63"), "-1");

  CHECK_STR(ev("1 ? 2 : 3"), "2");
  CHECK_STR(ev("0 ? 2 : 3"), "3");
  CHECK_STR(ev("1 + 1 ? 10 : 20"), "10");
  CHECK_STR(ev("0 ? 1 : 2 + 3"), "5");
  CHECK_STR(ev("1 ? 2 : 0 ? 3 : 4"), "2");
  CHECK_STR(ev("0 ? 2 : 0 ? 3 : 4"), "4");
  CHECK_STR(ev("0 ? 2 : 1 ? 3 : 4"), "3");
  CHECK_STR(ev("(1 ? 0 : 1) ? 5 : 6"), "6");
  CHECK_STR(ev("1 ? 0 ? 7 : 8 : 9"), "8");

  CHECK_STR(ev("low(0x1234)"), "52");
  CHECK_STR(ev("high(0x1234)"), "18");
  CHECK_STR(ev("byte2(0x1234)"), "18");
  CHECK_STR(ev("byte3(0x12345678)"), "52");
  CHECK_STR(ev("byte4(0x12345678)"), "18");
  CHECK_STR(ev("lwrd(0x12345678)"), "22136");
  CHECK_STR(ev("hwrd(0x12345678)"), "4660");
  CHECK_STR(ev("page(0x123456)"), "18");
  CHECK_STR(ev("page(0x7F0000)"), "63");
  CHECK_STR(ev("exp2(10)"), "1024");
  CHECK_STR(ev("exp2(0)"), "1");
  CHECK_STR(ev("log2(1024)"), "10");
  CHECK_STR(ev("log2(1000)"), "9");
  CHECK_STR(ev("log2(1)"), "0");
  CHECK_STR(ev("abs(-5)"), "5");
  CHECK_STR(ev("abs(5)"), "5");
  CHECK_STR(ev("exp2(31)"), "-2147483648");
  CHECK_STR(ev("exp2(32)"), "1");
  CHECK_STR(ev("exp2(62)"), "1073741824");
  CHECK_STR(ev("log2(0)"), "-1");
  CHECK_STR(ev("log2(-1)"), "31");
  CHECK_STR(ev("abs(-2147483647 - 1)"), "-2147483648");
  CHECK_STR(ev("page(-1)"), "63");
  CHECK_STR(ev("LOW(0x1234)"), "52");
  CHECK_STR(ev("High(0x1234)"), "18");
  CHECK_STR(ev("low (0x1234)"), "52");
  CHECK_STR(ev("low(-1)"), "255");
  CHECK_STR(ev("high(-1)"), "255");
  CHECK_STR(ev("hwrd(-1)"), "65535");
  CHECK_STR(ev("low(high(0x1234))"), "18");
  CHECK_STR(ev("high(1 + 0xFF)"), "1");
  CHECK_STR(ev("low(0x1234) + high(0x1234) * 2"), "88");
  CHECK_STR(ev("-low(1)"), "-1");
  CHECK_STR(ev("low(0x34, 0x12)"), "error");

  CHECK_STR(ev("FOO"), "10");
  CHECK_STR(ev("FOO + 1"), "11");
  CHECK_STR(ev("BAR / FOO"), "25");
  CHECK_STR(ev("FOO * (BAR - FOO)"), "2460");
  CHECK_STR(ev("NEG * NEG"), "25");
  CHECK_STR(ev("low(BAR) + high(BAR)"), "1");
  CHECK_ERR("lowest", 0, 6);
  CHECK_ERR("highway", 0, 7);
  CHECK_ERR("pcx", 0, 3);
  CHECK_ERR("pc2 + 1", 0, 3);
  CHECK_ERR("defined1", 0, 8);
  CHECK_ERR("absolute", 0, 8);
  CHECK_ERR("page_size", 0, 9);
  CHECK_STR(ev("FOOBAR"), "error");
  CHECK_STR(ev("FO"), "error");
  CHECK_STR(ev("foo"), "error");
  {
    Out o;
    runWith(&o, "1 + 2", 5, 0, 0);
    CHECK_INT(o.lookups, 0);
    runWith(&o, "FOO + FOO", 9, 0, 0);
    CHECK_INT(o.lookups, 2);
    CHECK_STR(o.text, "20");
  }

  {
    Out o;
    runWith(&o, "pc + 2", 6, 0, 100);
    CHECK_STR(o.text, "102");
    CHECK_INT(o.lookups, 0);
    runWith(&o, "PC", 2, 0, -3);
    CHECK_STR(o.text, "-3");
    runWith(&o, "pc", 2, 0, 0);
    CHECK_STR(o.text, "0");
  }

  CHECK_STR(ev("defined(FOO)"), "1");
  CHECK_STR(ev("defined(NOPE)"), "0");
  CHECK_STR(ev("defined FOO"), "1");
  CHECK_STR(ev("defined NOPE"), "0");
  CHECK_STR(ev("DEFINED(FOO)"), "1");
  CHECK_STR(ev("!defined(NOPE)"), "1");
  CHECK_STR(ev("defined(FOO) && FOO > 5"), "1");
  CHECK_STR(ev("defined(NOPE) || defined(FOO)"), "1");
  CHECK_STR(evUndef("defined(NOPE)"), "0");
  CHECK_ERR("defined(FOO", 11, 11);
  CHECK_ERR("defined(1)", 8, 9);
  CHECK_ERR("defined()", 8, 9);
  CHECK_ERR("defined", 7, 7);

  CHECK_ERR("NOPE", 0, 4);
  CHECK_ERR("1 + NOPE", 4, 8);
  CHECK_STR(evUndef("NOPE"), "?");
  CHECK_STR(evUndef("NOPE + 1"), "?");
  CHECK_STR(evUndef("1 + NOPE * 2"), "?");
  CHECK_STR(evUndef("-NOPE"), "?");
  CHECK_STR(evUndef("low(NOPE)"), "?");
  CHECK_STR(evUndef("NOPE ? 1 : 2"), "?");
  CHECK_STR(evUndef("1 ? NOPE : 2"), "?");
  CHECK_STR(evUndef("1 ? 2 : NOPE"), "2");
  CHECK_STR(evUndef("0 ? NOPE : 2"), "2");
  CHECK_STR(evUndef("FOO + 1"), "11");
  CHECK_STR(evUndef("1 / NOPE"), "?");
  CHECK_STR(evUndef("NOPE / NOPE"), "?");
  CHECK_STR(evUndef("NOPE % NOPE"), "?");
  {
    Out o;
    runWith(&o, "NOPE / 0", 8, 1, 0);
    CHECK_STR(o.text, "error");
    CHECK_INT(o.errors, 1);
    CHECK_INT(o.firstError.start, 5);
    CHECK_INT(o.firstError.end, 6);
  }

  CHECK_STR(ev("1 + 2, 3"), "3 @COMMA");
  CHECK_STR(ev("(1 + 2) ) 3"), "3 @RPAREN");
  CHECK_STR(ev("1 2"), "1 @NUMBER");
  CHECK_STR(ev("FOO BAR"), "10 @IDENT");
  CHECK_STR(ev("FOO--1"), "10 @MINUSMINUS");
  CHECK_STR(ev("FOO++"), "10 @PLUSPLUS");
  CHECK_STR(ev("FOO - -1"), "11");
  CHECK_STR(ev("FOO -1"), "9");
  CHECK_STR(ev("1 +"), "error");
  CHECK_STR(ev("FOO = 3"), "10 @EQ");
  CHECK_STR(ev("FOO : 3"), "10 @COLON");
  CHECK_STR(ev("1 + 2 ; comment"), "3");
  CHECK_STR(ev("1 + 2\nnop"), "3");
  CHECK_STR(ev("1 + \\\n 2"), "3");
  {
    Arena arena;
    arenaInit(&arena, 0);
    SourceMap map;
    sourceMapInit(&map, &arena);
    DiagList diags;
    diagListInit(&diags, &arena);
    const char *src = ".db 1+1, 2*3\n";
    FileId id = sourceMapAdd(&map, "t.asm", src, strlen(src));
    Lexer lx;
    lexerInit(&lx, &map, id, &diags);
    TokenLine line = lexLine(&lx, &arena);
    TokenCursor cur;
    cursorInit(&cur, &line);
    cursorNext(&cur);
    Table table = { 0 };
    ExprEnv env = { &map, &diags, lookup, &table, 0, 0 };
    ExprValue a, b;
    CHECK(exprEval(&cur, &env, &a));
    CHECK(cursorAccept(&cur, TOK_COMMA));
    CHECK(exprEval(&cur, &env, &b));
    CHECK_INT(a.value, 2);
    CHECK_INT(b.value, 6);
    CHECK(cursorAtEnd(&cur));
    arenaFree(&arena);
  }

  CHECK_ERR("", 0, 0);
  CHECK_ERR("1 +", 3, 3);
  CHECK_ERR("1 +\n", 3, 4);
  CHECK_ERR(")", 0, 1);
  CHECK_ERR("(1", 2, 2);
  CHECK_ERR("(1 + 2", 6, 6);
  CHECK_ERR("((1)", 4, 4);
  CHECK_ERR("()", 1, 2);
  CHECK_ERR("1 +* 2", 3, 4);
  CHECK_ERR("* 2", 0, 1);
  CHECK_ERR("1 ? 2", 5, 5);
  CHECK_ERR("1 ? 2 3", 6, 7);
  CHECK_ERR("1 ? : 3", 4, 5);
  CHECK_ERR("1 / 0", 2, 3);
  CHECK_ERR("1 % 0", 2, 3);
  CHECK_ERR("1 / (2 - 2)", 2, 3);
  CHECK_ERR("FOO / (FOO - 10)", 4, 5);
  CHECK_ERR("+1", 0, 1);
  CHECK_ERR("--1", 0, 2);
  CHECK_ERR("++1", 0, 2);
  CHECK_ERR("1 + --1", 4, 6);
  CHECK_ERR("1.5", 0, 3);
  CHECK_ERR("1 + 2.5", 4, 7);
  CHECK_ERR("1 < 2 < 3", 6, 7);
  CHECK_ERR("1 == 2 == 3", 7, 9);
  CHECK_ERR("1 <= 2 > 0", 7, 8);
  CHECK_ERR("1 != 2 == 3", 7, 9);
  CHECK_ERR("low", 3, 3);
  CHECK_ERR("low 5", 4, 5);
  CHECK_ERR("low(", 4, 4);
  CHECK_ERR("low()", 4, 5);
  CHECK_ERR("low(1", 5, 5);
  CHECK_ERR("low(1, 2)", 5, 6);
  CHECK_ERR("int(2)", 0, 3);
  CHECK_ERR("strlen(\"x\")", 0, 6);
  CHECK_ERR("q15(1)", 0, 3);
  CHECK_ERR("exp2(-1)", 0, 4);
  CHECK_ERR("\"str\"", 0, 5);
  CHECK_ERR("#", 0, 1);

  CHECK_ERR("0b2", 0, 3);
  CHECK_ERR("1 + 0b2", 4, 7);
  CHECK_ERR("(0b2)", 1, 4);
  CHECK_ERR("-0b2", 1, 4);
  CHECK_ERR("low(0b2)", 4, 7);
  CHECK_ERR("1 ? 0b2 : 3", 4, 7);
  CHECK_ERR("'ab'", 0, 4);
  CHECK_ERR("`", 0, 1);

  {
    static const size_t depths[] = { 5000, 100000 };
    for (size_t k = 0; k < sizeof depths / sizeof depths[0]; k++) {
      size_t n = depths[k];
      char *s1 = repeat("(", n, "1", ")", n);
      char *s2 = repeat("~", n, "1", "", 0);
      char *s3 = repeat("- ", n, "1", "", 0);
      char *s4 = repeat("1?", n, "1", ":1", n);
      char *s5 = repeat("1?1:", n, "1", "", 0);
      REQUIRE(s1 && s2 && s3 && s4 && s5);
      Out o;
      const char *all[] = { s1, s2, s3, s4, s5 };
      for (int i = 0; i < 5; i++) {
        runWith(&o, all[i], strlen(all[i]), 0, 0);
        CHECK_STR(o.text, "error");
        CHECK_INT(o.errors, 1);
      }
      free(s1); free(s2); free(s3); free(s4); free(s5);
    }

    enum { N = 100000 };
    char *flat1 = repeat("1+", N, "1", "", 0);
    char *flat2 = repeat("1-", N, "1", "", 0);
    char *flat3 = repeat("(1)+", N, "(1)", "", 0);
    char *flat4 = repeat("-1+", N, "1", "", 0);
    char *flat5 = repeat("low(1)|", N, "1", "", 0);
    REQUIRE(flat1 && flat2 && flat3 && flat4 && flat5);
    Out o;
    runWith(&o, flat1, strlen(flat1), 0, 0);
    CHECK_STR(o.text, "100001");
    runWith(&o, flat2, strlen(flat2), 0, 0);
    CHECK_STR(o.text, "-99999");
    runWith(&o, flat3, strlen(flat3), 0, 0);
    CHECK_STR(o.text, "100001");
    runWith(&o, flat4, strlen(flat4), 0, 0);
    CHECK_STR(o.text, "-99999");
    runWith(&o, flat5, strlen(flat5), 0, 0);
    CHECK_STR(o.text, "1");
    free(flat1); free(flat2); free(flat3); free(flat4); free(flat5);

    CHECK_STR(ev("((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))"), "1");
    CHECK_STR(ev("- - - - - - - - - - - - - - - - - - - - 5"), "5");
    CHECK_STR(ev("1?1?1?1?1:2:3:4:5"), "1");
  }

  return TEST_RESULT();
}