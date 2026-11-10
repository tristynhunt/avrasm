#include "avrasm/operand.h"
#include "avrasm/symtab.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

typedef struct Fx {
  Arena arena;
  SourceMap map;
  DiagList diags;
  Symtab syms;
  Lexer lx;
  TokenLine line;
} Fx;

static void fxInit(Fx *f, const char *src)
{
  arenaInit(&f->arena, 0);
  sourceMapInit(&f->map, &f->arena);
  diagListInit(&f->diags, &f->arena);
  symtabInit(&f->syms, &f->arena, 0);
  FileId id = sourceMapAdd(&f->map, "t.asm", src, strlen(src));
  lexerInit(&f->lx, &f->map, id, &f->diags);
  f->line = lexLine(&f->lx, &f->arena);
}

static ExprEnv envOf(Fx *f)
{
  ExprEnv env = { &f->map, &f->diags, symtabLookup, &f->syms, 0, 0, NULL };
  return env;
}

static const char *spans(Fx *f, const Operand *ops, int n)
{
  static char out[256];
  out[0] = '\0';
  const SourceFile *file = sourceMapGet(&f->map, ops[0].span.file);
  for (int i = 0; i < n; i++) {
    size_t used = strlen(out);
    snprintf(out + used, sizeof out - used, "%s%.*s", i ? " | " : "", (int)(ops[i].span.end - ops[i].span.start),
             file->text + ops[i].span.start);
  }
  return out;
}

static int fakeAliases(void *user, const char *name, size_t len, int *reg)
{
  (void)user;
  if (len != 4 || strncmp(name, "temp", 4) != 0) return 0;
  *reg = 20;
  return 1;
}

int main(void)
{
  Operand ops[8];

  {
    Fx f;
    fxInit(&f, "ldi r16, 0x55 ; comment\n");
    int n = splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4);
    CHECK_INT(n, 2);
    CHECK_STR(spans(&f, ops, 2), "r16 | 0x55");
    CHECK_INT(ops[0].tokens.count, 2);
    CHECK_INT(ops[0].tokens.tokens[1].kind, TOK_NEWLINE);
    CHECK_INT(ops[0].tokens.tokens[1].span.start, 7);
    CHECK_INT(ops[0].tokens.tokens[1].span.end, 7);
    CHECK_INT(ops[1].tokens.count, 2);
    CHECK_INT(f.diags.errorCount, 0);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "x low(a), (b, c), d\n");
    int n = splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4);
    CHECK_INT(n, 3);
    CHECK_STR(spans(&f, ops, 3), "low(a) | (b, c) | d");
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "nop\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 2), 0);
    CHECK_INT(f.diags.errorCount, 0);
    arenaFree(&f.arena);
    fxInit(&f, "nop");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 2), 0);
    arenaFree(&f.arena);
    fxInit(&f, "rjmp loop\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 1), 1);
    CHECK_STR(spans(&f, ops, 1), "loop");
    arenaFree(&f.arena);
    fxInit(&f, "a 1, 2, 3, 4\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), 4);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "nop r1\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 0), -1);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_INT(f.diags.head->span.start, 4);
    CHECK_INT(f.diags.head->span.end, 6);
    arenaFree(&f.arena);

    fxInit(&f, "ldi r16, 1, 2\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 2), -1);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_INT(f.diags.head->span.start, 12);
    arenaFree(&f.arena);

    fxInit(&f, "ldi r16,\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), -1);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_INT(f.diags.head->span.start, 8);
    arenaFree(&f.arena);

    fxInit(&f, "ldi ,r16\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), -1);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_INT(f.diags.head->span.start, 4);
    CHECK_INT(f.diags.head->span.end, 5);
    arenaFree(&f.arena);

    fxInit(&f, "ldi r16,,3\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), -1);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_INT(f.diags.head->span.start, 8);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "x 1), 2\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), 2);
    CHECK_STR(spans(&f, ops, 2), "1) | 2");
    arenaFree(&f.arena);
    fxInit(&f, "x (1, 2\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), 1);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "x a, b\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 4), 2);
    memset(f.line.tokens, 0, f.line.count * sizeof(Token));
    CHECK_INT(ops[0].tokens.tokens[0].kind, TOK_IDENT);
    CHECK_INT(ops[1].tokens.tokens[0].span.start, 5);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, ".equ A = 1 + 2\n");
    Operand e = makeOperand(&f.line, 3, 6, &f.arena);
    CHECK_INT(e.tokens.count, 4);
    CHECK_INT(e.span.start, 9);
    CHECK_INT(e.span.end, 14);
    ExprEnv env = envOf(&f);
    ExprValue v;
    CHECK(evalOperand(&e, &env, &v));
    CHECK_INT(v.value, 3);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "x 1 + 2, 3 4, (5, ), 0b2, 6\n");
    symtabAdd(&f.syms, "K", 1, SYM_EQU, 40, f.line.tokens[0].span, NULL);
    int n = splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 8);
    CHECK_INT(n, 5);
    ExprEnv env = envOf(&f);
    ExprValue v;
    CHECK(evalOperand(&ops[0], &env, &v));
    CHECK_INT(v.value, 3);
    CHECK_INT(f.diags.errorCount, 1);
    CHECK(!evalOperand(&ops[1], &env, &v));
    CHECK_INT(v.known, 0);
    CHECK_INT(f.diags.errorCount, 2);
    CHECK(!evalOperand(&ops[3], &env, &v));
    CHECK_INT(f.diags.errorCount, 2);
    CHECK(evalOperand(&ops[4], &env, &v));
    CHECK_INT(v.value, 6);
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "x 1 0b2\n");
    CHECK_INT(splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 8), 1);
    CHECK_INT(f.diags.errorCount, 1);
    ExprEnv env = envOf(&f);
    ExprValue v;
    CHECK(!evalOperand(&ops[0], &env, &v));
    CHECK_INT(f.diags.errorCount, 1);
    arenaFree(&f.arena);
  }

  {
    int r = -1;
    CHECK(parseRegisterName("r0", 2, &r));  CHECK_INT(r, 0);
    CHECK(parseRegisterName("r9", 2, &r));  CHECK_INT(r, 9);
    CHECK(parseRegisterName("r16", 3, &r)); CHECK_INT(r, 16);
    CHECK(parseRegisterName("R31", 3, &r)); CHECK_INT(r, 31);
    CHECK(parseRegisterName("R7", 2, &r));  CHECK_INT(r, 7);
    r = -1;
    CHECK(!parseRegisterName("r32", 3, &r));
    CHECK(!parseRegisterName("r99", 3, &r));
    CHECK(!parseRegisterName("r", 1, &r));
    CHECK(!parseRegisterName("r1000", 5, &r));
    CHECK(!parseRegisterName("r1x", 3, &r));
    CHECK(!parseRegisterName("x16", 3, &r));
    CHECK(!parseRegisterName("16", 2, &r));
    CHECK(!parseRegisterName("rx", 2, &r));
    CHECK(!parseRegisterName("r-1", 3, &r));
    CHECK(!parseRegisterName("r05", 3, &r));
    CHECK(!parseRegisterName("", 0, &r));
    CHECK(!parseRegisterName("r016", 4, &r));
    CHECK(!parseRegisterName("r001", 4, &r));
    CHECK_INT(r, -1);
    CHECK(parseRegisterName("r16 and more", 3, &r));
    CHECK_INT(r, 16);
  }
  {
    Fx f;
    fxInit(&f, "x r16, r17+1, r40, 5, R3\n");
    int n = splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 8);
    CHECK_INT(n, 5);
    int r = -1;
    CHECK(operandRegister(&ops[0], &f.map, &r));
    CHECK_INT(r, 16);
    CHECK(!operandRegister(&ops[1], &f.map, &r));
    CHECK(!operandRegister(&ops[2], &f.map, &r));
    CHECK(!operandRegister(&ops[3], &f.map, &r));
    CHECK(operandRegister(&ops[4], &f.map, &r));
    CHECK_INT(r, 3);
    arenaFree(&f.arena);
  }
  {
    Fx f;
    fxInit(&f, "x r16, temp, other, temp+1\n");
    int n = splitOperands(&f.line, 1, &f.arena, &f.diags, ops, 8);
    CHECK_INT(n, 4);
    ExprEnv env = envOf(&f);
    int r = -1;
    CHECK(operandRegisterEnv(&ops[0], &env, &r));
    CHECK_INT(r, 16);
    CHECK(!operandRegisterEnv(&ops[1], &env, &r));
    env.lookupRegister = fakeAliases;
    CHECK(operandRegisterEnv(&ops[1], &env, &r));
    CHECK_INT(r, 20);
    CHECK(!operandRegisterEnv(&ops[2], &env, &r));
    CHECK(!operandRegisterEnv(&ops[3], &env, &r));
    arenaFree(&f.arena);
  }

  {
    Fx f;
    fxInit(&f, "ldi r16, 1\n");
    CHECK(expectOperands(&f.line, 1, &f.arena, &f.diags, "ldi", 2, ops));
    CHECK_INT(f.diags.errorCount, 0);
    CHECK_STR(spans(&f, ops, 2), "r16 | 1");
    arenaFree(&f.arena);

    fxInit(&f, "ldi r16\n");
    CHECK(!expectOperands(&f.line, 1, &f.arena, &f.diags, "ldi", 2, ops));
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_STR(f.diags.head->message, "ldi takes 2 operands, found 1");
    CHECK_INT(f.diags.head->span.start, 7);
    arenaFree(&f.arena);

    fxInit(&f, ".org\n");
    CHECK(!expectOperands(&f.line, 1, &f.arena, &f.diags, ".org", 1, ops));
    CHECK_STR(f.diags.head->message, ".org takes 1 operand, found 0");
    arenaFree(&f.arena);

    fxInit(&f, "nop 5\n");
    CHECK(!expectOperands(&f.line, 1, &f.arena, &f.diags, "nop", 0, ops));
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_STR(f.diags.head->message, "too many operands");
    arenaFree(&f.arena);

    fxInit(&f, "nop\n");
    CHECK(expectOperands(&f.line, 1, &f.arena, &f.diags, "nop", 0, ops));
    arenaFree(&f.arena);

    fxInit(&f, "ldi r16,,1\n");
    CHECK(!expectOperands(&f.line, 1, &f.arena, &f.diags, "ldi", 2, ops));
    CHECK_INT(f.diags.errorCount, 1);
    CHECK_STR(f.diags.head->message, "expected an operand");
    arenaFree(&f.arena);
  }

  return TEST_RESULT();
}