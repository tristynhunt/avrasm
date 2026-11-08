#include "avrasm/instr.h"
#include "avrasm/symtab.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

typedef struct Fx {
  Arena arena;
  SourceMap map;
  DiagList diags;
  Symtab syms;
  ExprEnv env;
} Fx;

static void fxInit(Fx *f)
{
  arenaInit(&f->arena, 0);
  sourceMapInit(&f->map, &f->arena);
  diagListInit(&f->diags, &f->arena);
  symtabInit(&f->syms, &f->arena, 0);
  ExprEnv env = { &f->map, &f->diags, symtabLookup, &f->syms, 0, 0 };
  f->env = env;
}

static void fxDefine(Fx *f, const char *name, int64_t value)
{
  Span none = { FILE_NONE, 0, 0 };
  symtabAdd(&f->syms, name, strlen(name), SYM_EQU, value, none, NULL);
}

static void fxSymbols(Fx *f)
{
  fxDefine(f, "start", 0);
  fxDefine(f, "far", 2000);
  fxDefine(f, "huge", 5000);
  fxDefine(f, "K", 0x55);
}

static int enc(Fx *f, const char *src, int64_t pc, uint16_t out[2])
{
  FileId id = sourceMapAdd(&f->map, "t.asm", src, strlen(src));
  Lexer lx;
  lexerInit(&lx, &f->map, id, &f->diags);
  TokenLine line = lexLine(&lx, &f->arena);

  const SourceFile *file = sourceMapGet(&f->map, id);
  const Token *head = &line.tokens[0];
  const InstrDesc *d = instrFind(file->text + head->span.start, head->span.end - head->span.start);
  if (d == NULL) return -1;

  Operand ops[INSTR_MAX_OPERANDS];
  if (!instrParseOperands(d, &line, 1, &f->arena, &f->diags, ops)) return 0;
  return instrEncode(d, ops, pc, &f->env, out);
}

static void good(const char *src, int64_t pc, unsigned want)
{
  Fx f;
  fxInit(&f);
  fxSymbols(&f);
  uint16_t w[2] = { 0xDEAD, 0xBEEF };
  int n = enc(&f, src, pc, w);
  if (n != 1 || w[0] != want || f.diags.count != 0) {
    fprintf(stderr, "good(\"%s\" at %lld): words %d, got 0x%04X, want 0x%04X, %u diagnostics\n", src,
            (long long)pc, n, w[0], want, f.diags.count);
    check_failures_++;
  }
  CHECK_INT(w[1], 0xBEEF);
  arenaFree(&f.arena);
}

static void bad(const char *src, int64_t pc, unsigned errors, uint32_t at)
{
  Fx f;
  fxInit(&f);
  fxSymbols(&f);
  uint16_t w[2] = { 0xDEAD, 0xBEEF };
  int n = enc(&f, src, pc, w);
  if (n != 0 || f.diags.errorCount != errors || f.diags.head == NULL || f.diags.head->span.start != at) {
    fprintf(stderr, "bad(\"%s\"): words %d, %u errors (want %u), first at %u (want %u)\n", src, n,
            f.diags.errorCount, errors, f.diags.head ? f.diags.head->span.start : 9999, at);
    check_failures_++;
  }
  CHECK_INT(w[0], 0xDEAD);
  arenaFree(&f.arena);
}

int main(void)
{
  {
    CHECK(instrFind("ldi", 3) != NULL);
    CHECK(instrFind("LDI", 3) != NULL);
    CHECK(instrFind("LdI", 3) != NULL);
    CHECK(instrFind("rjmp", 4) == instrFind("RJMP", 4));
    CHECK(instrFind("nop", 3) != NULL);
    CHECK(instrFind("out", 3) != NULL);
    CHECK(instrFind("ldx", 3) == NULL);
    CHECK(instrFind("ld", 2) == NULL);
    CHECK(instrFind("ldi2", 4) == NULL);
    CHECK(instrFind("ldi", 2) == NULL);
    CHECK(instrFind("ldi", 0) == NULL);
    CHECK(instrFind("", 0) == NULL);
    CHECK(instrFind("ldi\0x", 5) == NULL);
    CHECK_INT(instrFind("ldi", 3)->form, FORM_RD_K);
    CHECK_INT(instrFind("ldi", 3)->operandCount, 2);
    CHECK_INT(instrFind("nop", 3)->operandCount, 0);
    CHECK_INT(instrFind("rjmp", 4)->operandCount, 1);
    CHECK_STR(instrFind("OUT", 3)->name, "out");
  }

  good("nop\n", 0, 0x0000);
  good("NOP", 7, 0x0000);

  good("ldi r16, 0x55\n", 0, 0xE505);
  good("ldi r16, 0\n", 0, 0xE000);
  good("ldi r31, 0xFF\n", 0, 0xEFFF);
  good("ldi r17, 0x10\n", 0, 0xE110);
  good("ldi r20, 0xAB\n", 0, 0xEA4B);
  good("ldi r24, 1\n", 0, 0xE081);
  good("LDI R16, 0x55\n", 0, 0xE505);
  good("ldi r16, K\n", 0, 0xE505);
  good("ldi r16, K + 1\n", 0, 0xE506);
  good("ldi r16, -1\n", 0, 0xEF0F);
  good("ldi r16, -128\n", 0, 0xE800);
  good("ldi r16, 255\n", 0, 0xEF0F);
  good("ldi r16, 'A'\n", 0, 0xE401);
  bad("ldi r16, 256\n", 0, 1, 9);
  bad("ldi r16, -129\n", 0, 1, 9);
  bad("ldi r15, 1\n", 0, 1, 4);
  bad("ldi r0, 1\n", 0, 1, 4);
  bad("ldi 16, 1\n", 0, 1, 4);
  bad("ldi r32, 1\n", 0, 1, 4);
  bad("ldi r16\n", 0, 1, 7);
  bad("ldi\n", 0, 1, 3);
  bad("ldi r16, 1, 2\n", 0, 1, 12);
  bad("ldi r16,\n", 0, 1, 8);
  bad("ldi r16, nosuch\n", 0, 1, 9);
  bad("ldi r15, 256\n", 0, 2, 4);
  bad("ldi r16, 1 2\n", 0, 1, 11);

  good("out 0x3f, r16\n", 0, 0xBF0F);
  good("out 0, r0\n", 0, 0xB800);
  good("out 0x3f, r31\n", 0, 0xBFFF);
  good("out 0x10, r5\n", 0, 0xBA50);
  good("out 0x1f, r1\n", 0, 0xBA1F);
  good("out 63, r0\n", 0, 0xBE0F);
  good("out K - 0x50, r2\n", 0, 0xB825);
  bad("out 64, r0\n", 0, 1, 4);
  bad("out -1, r0\n", 0, 1, 4);
  bad("out 0x3f, r32\n", 0, 1, 10);
  bad("out 0x3f, 5\n", 0, 1, 10);
  bad("out r0, r1\n", 0, 1, 4);
  bad("out 64, r32\n", 0, 2, 4);
  bad("out 0x3f\n", 0, 1, 8);

  good("rjmp start\n", 2, 0xCFFD);
  good("rjmp start\n", 0, 0xCFFF);
  good("rjmp 1\n", 0, 0xC000);
  good("rjmp 2\n", 0, 0xC001);
  good("rjmp 2048\n", 0, 0xC7FF);
  good("rjmp 0\n", 2047, 0xC800);
  good("rjmp far\n", 0, 0xC7CF);
  good("rjmp far\n", 4047, 0xC800);
  good("RJMP START\n", 2, 0xCFFD);
  bad("rjmp 2049\n", 0, 1, 5);
  bad("rjmp 0\n", 2048, 1, 5);
  bad("rjmp huge\n", 0, 1, 5);
  bad("rjmp far\n", 4048, 1, 5);
  bad("rjmp -2147483647-1\n", 0, 1, 5);
  bad("rjmp 2147483647\n", 0, 1, 5);
  bad("rjmp\n", 0, 1, 4);
  bad("rjmp 1, 2\n", 0, 1, 8);
  bad("rjmp nosuch\n", 0, 1, 5);

  {
    Fx f;
    uint16_t w[2];
    fxInit(&f);
    fxSymbols(&f);
    enc(&f, "ldi r15, 1\n", 0, w);
    CHECK_STR(f.diags.head->message, "r15 is not allowed here, use r16 to r31");
    enc(&f, "ldi r16\n", 0, w);
    CHECK_STR(f.diags.tail->message, "ldi takes 2 operands, found 1");
    enc(&f, "rjmp\n", 0, w);
    CHECK_STR(f.diags.tail->message, "rjmp takes 1 operand, found 0");
    enc(&f, "nop 5\n", 0, w);
    CHECK_STR(f.diags.tail->message, "too many operands");
    enc(&f, "out 64, r0\n", 0, w);
    CHECK_STR(f.diags.tail->message, "I/O address 64 is out of range (0 to 63)");
    enc(&f, "rjmp 2049\n", 0, w);
    CHECK_STR(f.diags.tail->message, "relative jump out of range: 2048 words (allowed -2048 to 2047)");
    enc(&f, "out 1, 2\n", 0, w);
    CHECK_STR(f.diags.tail->message, "expected a register, r0 to r31");
    CHECK_INT(f.diags.errorCount, 7);
    arenaFree(&f.arena);
  }

  bad("nop r0\n", 0, 1, 4);
  bad("nop 1\n", 0, 1, 4);

  {
    Fx f;
    uint16_t w[2] = { 0xDEAD, 0xBEEF };
    fxInit(&f);
    f.env.allowUndefined = 1;
    CHECK_INT(enc(&f, "rjmp nosuch\n", 0, w), 0);
    CHECK_INT(enc(&f, "ldi r16, nosuch\n", 0, w), 0);
    CHECK_INT(enc(&f, "out nosuch, r0\n", 0, w), 0);
    CHECK_INT(f.diags.errorCount, 3);
    CHECK(strstr(f.diags.head->message, "not known") != NULL);
    CHECK_INT(w[0], 0xDEAD);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    uint16_t w[2];
    fxInit(&f);
    CHECK_INT(enc(&f, "frob r1\n", 0, w), -1);
    CHECK_INT(f.diags.count, 0);
    arenaFree(&f.arena);
  }

  return TEST_RESULT();
}