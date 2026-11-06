#ifndef AVRASM_EXPR_H
#define AVRASM_EXPR_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/diag.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

typedef struct ExprEnv
{
  const SourceMap *sources;
  DiagList *diags;
  // symbol lookup function. 'name' points into the source text and is 'len' bytes.
  // return 1 and store the value in *value if it exists, 0 if it doesnt and null is stored.
  int (*lookup)(void *user, const char *name, size_t len, int64_t *value);
  void *user;
  int allowUndefined;
  // the current program counter value
  int64_t pc;
} ExprEnv;

typedef struct ExprValue
{
  int64_t value;
  // 0 when the value depends on an undefined symbol
  int known;
} ExprValue;

// evaluates one expression starting at the cursor. avrasm2.exe calculates with signed 64-bit
// integers and wraps on overflow then cuts the final result to a signed 32-bit number.
//
// it reads as many tokens which belong to the expression and stops at the first token
// that cannot continue it (a comma, a closing parenthesis or the end of the line),
// leaving the cursor on that token. it doesnt check what that token is, so the caller
// decides whether a leftover token is an error. if the leftover token is TOK_ERROR,
// the lexer has already reported it and is not reported again.
//
// returns 1 on success and stores the result in *out, returns 0 on failure after
// emitting exactly one error (or none if the failure is a TOK_ERROR token which the
// lexer already reported), *out is {0, 0} and the cursor is somewhere inside the expression.
int exprEval(TokenCursor *cur, const ExprEnv *env, ExprValue *out);

#endif