#ifndef AVRASM_OPERAND_H
#define AVRASM_OPERAND_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

// one operand of an instruction or directive. its tokens are copied
// out of line into the arena and followed by a NEWLINE token (the 
// empty span where the operand ends), so the cursor and expression eval
// treat it the same as a line.
typedef struct Operand
{
  TokenLine tokens;
  Span span;  // from the start of the first token to the end of the last
} Operand;

// the operand made of line->tokens[from..to) which must hold at least one token.
Operand makeOperand(const TokenLine *line, uint32_t from, uint32_t to, Arena *arena);

// splits line->token[from..last) where 'last' is the NEWLINE or EOF token and 
// is not part of any operand. splits at commas that are not inside parens into out[0..max).
// 
// returns the number of operands, is 0 when nothing follows 'from'.
// returns -1 after reporting an error when an operand is empty or when there are more than max.
int splitOperands(const TokenLine *line, uint32_t from, Arena *arena, 
  DiagList *diags, Operand *out, uint32_t max);

// splitOperands, then insits on exactly 'count' operands. 'name' is the instruction or
// directive for the message. returns 1 and fills out [0..count) or returns 0 on error
int expectOperands(const TokenLine *line, uint32_t from, Arena *arena,
  DiagList *diags, const char *name, uint32_t count, Operand *out);

// evaluates the whole operand as one expression. every token of it
// must belong to the expression. returns 1 and sets *out or returns 0 after
// and error sets *out to { 0, 0 }
int evalOperand(const Operand *op, const ExprEnv *env, ExprValue *out);

// checks if name[0..len) is the name of a core register, r0 to r31 in any case.
// if true, stores that register number in *reg
int parseRegisterName(const char *name, size_t len, int *reg);

// checks if the operand is a single identifier that names a register.
// if true, stores that register number in *reg
int operandRegister(const Operand *op, const SourceMap *sources, int *reg);

#endif