#include "avrasm/operand.h"
#include "avrasm/arena.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/lexer.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

Operand makeOperand(const TokenLine *line, uint32_t from, uint32_t to, Arena *arena)
{
  uint32_t count = to - from;
  Token *copy = arenaAlloc(arena, (count + 1) * sizeof *copy, _Alignof(Token));
  memcpy(copy, &line->tokens[from], count * sizeof *copy);

  Span first = line->tokens[from].span;
  Span last = line->tokens[to - 1].span;
  Token end = { TOK_NEWLINE, { first.file, last.end, last.end }, 0, 0.0 };
  copy[count] = end;

  Operand op;
  op.tokens.tokens = copy;
  op.tokens.count = count + 1;
  op.span.file = first.file;
  op.span.start = first.start;
  op.span.end = last.end;
  return op;
}

int splitOperands(const TokenLine *line, uint32_t from, Arena *arena, 
  DiagList *diags, Operand *out, uint32_t max)
{
  uint32_t last = line->count - 1; // the NEWLINE or EOF
  if (from >= last) return 0;

  uint32_t n = 0;
  uint32_t depth = 0;
  uint32_t start = from;
  for (uint32_t i = from; i <= last; i++) {
    const Token *t = &line->tokens[i];
    int atEnd = i == last;

    // increase / decrease depth
    if (!atEnd) {
      if (t->kind == TOK_LPAREN) depth++;
      else if (t->kind == TOK_RPAREN && depth > 0) depth--;
    }

    // continue when within an operand
    if (!atEnd && !(t->kind == TOK_COMMA && depth == 0)) continue;

    // tokens [start, i) is empty ("ldi ,r16")
    if (i == start) { //   not allowed ^^^
      diagEmit(diags, DIAG_ERROR, t->span, "expected an operand");
      return -1;
    }
    
    if (n == max) {
      Span span = line->tokens[start].span;
      diagEmit(diags, DIAG_ERROR, span, "too many operands");
      return -1;
    }

    out[n++] = makeOperand(line, start, i, arena);
    start = i + 1;
  }
  return (int)n;
}

int expectOperands(const TokenLine *line, uint32_t from, Arena *arena,
  DiagList *diags, const char *name, uint32_t count, Operand *out)
{
  int n = splitOperands(line, from, arena, diags, out, count);
  if (n < 0) return 0;
  if ((uint32_t)n != count) {
    const Token *end = &line->tokens[line->count - 1];
    diagEmit(diags, DIAG_ERROR, end->span, "%s takes %u operand%s, found %d",
      name, count, count > 1 ? "s" : "", n);
    return 0;
  }
  return 1;
}

int evalOperand(const Operand *op, const ExprEnv *env, ExprValue *out)
{
  TokenCursor c;
  cursorInit(&c, &op->tokens);
  if(!exprEval(&c, env, out)) return 0;
  if (!cursorAtEnd(&c)) {
    const Token *t = cursorPeek(&c);
    // left over TOK_ERROR has already been reported
    if (t->kind != TOK_ERROR) diagEmit(env->diags, DIAG_ERROR, t->span, "unexpected token after the expression");
    out->known = 0;
    out->value = 0;
    return 0;
  }
  return 1;
}

int parseRegisterName(const char *name, size_t len, int *reg)
{
  if (len != 2 && len != 3) return 0;
  
  const char *p = name;
  if (*p != 'r' && *p != 'R') return 0;
  p++;

  int r = 0;
  if (*p < '0' || *p > '9') return 0;
  r = *p++ - '0';
  if (len == 3) {
    if (*p < '0' || *p > '9' || r == 0) return 0;
    r = r * 10 + (*p - '0');
  }
  if (r > 31) return 0;
  *reg = r;
  return 1; 
}

int operandRegister(const Operand *op, const SourceMap *sources, int *reg)
{
  if(op->tokens.count != 2 || op->tokens.tokens[0].kind != TOK_IDENT) return 0;
  const Token *t = &op->tokens.tokens[0];
  const SourceFile *f = sourceMapGet(sources, t->span.file);
  if (f == NULL) return 0;
  return parseRegisterName(f->text + t->span.start, t->span.end - t->span.start, reg);
}

int operandRegisterEnv(const Operand *op, const ExprEnv *env, int *reg)
{
  if (operandRegister(op, env->sources, reg)) return 1;
  if (env->lookupRegister == NULL || op->tokens.count != 2 || op->tokens.tokens[0].kind != TOK_IDENT) return 0;
  const Token *t = &op->tokens.tokens[0];
  const SourceFile *f = sourceMapGet(env->sources, t->span.file);
  if (f == NULL) return 0;
  return env->lookupRegister(env->user, f->text + t->span.start, t->span.end - t->span.start, reg);
}