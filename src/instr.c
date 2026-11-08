#include "avrasm/instr.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/operand.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const InstrDesc table[] = {
  { "ldi",  FORM_RD_K,  0xE000, 1, 2 },
  { "nop",  FORM_NONE,  0x0000, 1, 0 },
  { "out",  FORM_A_RR,  0xB800, 1, 2 },
  { "rjmp", FORM_REL12, 0xC000, 1, 1 }
};

static char lowerAscii(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

const InstrDesc *instrFind(const char *name, size_t len)
{
  for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
    const char *instr = table[i].name;
    if (len != strlen(instr)) continue;
    size_t j = 0;
    while (j < len && lowerAscii(name[j]) == instr[j]) j++;
    if (j == len) return &table[i];
  }
  return NULL;
}

int instrParseOperands(const InstrDesc *d, const TokenLine *line, uint32_t from, Arena *arena,
  DiagList *diags, Operand out[INSTR_MAX_OPERANDS])
{
  return expectOperands(line, from, arena, diags, d->name, d->operandCount, out);
}

// a register operand no lower than r<lo>. reports why not and returns 0
static int needRegister(const Operand *op, const ExprEnv *env, int lo, int *reg)
{
  if (!operandRegister(op, env->sources, reg)) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "expected a register, r%d to r31", lo);
    return 0;
  }
  if (*reg < lo) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "r%d is not allowed here, use r%d to r31", *reg, lo);
    return 0;
  }
  return 1;
}

// an expression operand whose value must lie in lo..hi
static int needValue(const Operand *op, const ExprEnv *env, int64_t lo, int64_t hi, 
  const char *what, int64_t *value)
{
  ExprValue out;
  // already reported
  if (!evalOperand(op, env, &out)) return 0;

  if (!out.known) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "%s is not known yet", what);
    return 0;
  }

  if (out.value < lo || out.value > hi) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "%s %lld is out of range (%lld to %lld)",
      what, (long long)out.value, (long long)lo, (long long)hi);
    return 0;
  }
  *value = out.value;
  return 1;
}


int instrEncode(const InstrDesc *d, const Operand *ops, int64_t pc, const ExprEnv *env, uint16_t out[2])
{
  int reg = 0;
  int64_t value = 0;
  
  switch (d->form) {
    case FORM_NONE:
      // 0000 0000 0000 0000
      out[0] = d->opcode;
      return 1;
    case FORM_RD_K: {
      int okReg = needRegister(&ops[0], env, 16, &reg);
      int okVal = needValue(&ops[1], env, -128, 255, "immediate", &value);
      if (!okReg || !okVal) return 0;
      unsigned k = (unsigned)value & 0xFF;
      unsigned d4 = (unsigned)(reg - 16);
      // 1110 KKKK dddd KKKK
      out[0] = (uint16_t)(d->opcode | ((k & 0xF0) << 4) | (d4 << 4) | (k & 0x0F));
      return 1;
    }
    case FORM_A_RR: {
      int okVal = needValue(&ops[0], env, 0, 63, "I/O address", &value);
      int okReg = needRegister(&ops[1], env, 0, &reg);
      if (!okReg || !okVal) return 0;
      unsigned ad = (unsigned)value;
      unsigned rr = (unsigned)(reg);
      // 1011 1AAr rrrr AAAA
      out[0] = (uint16_t)(d->opcode | ((ad & 0x30) << 5) | (rr << 4) | (ad & 0x0F));
      return 1;
    }
    case FORM_REL12: {
      ExprValue v;
      if (!evalOperand(&ops[0], env, &v)) return 0;
      if (!v.known) {
        diagEmit(env->diags, DIAG_ERROR, ops[0].span, "jump target is not known yet");
        return 0;
      }
      // the distance in words from the next instruction in unsigned arithmetic
      int64_t k = (int64_t)((uint64_t)v.value - (uint64_t)(pc + 1));
      if (k < -2048 || k > 2047) {
        diagEmit(env->diags, DIAG_ERROR, ops[0].span, 
          "relative jump out of range: %lld words (allowed -2048 to 2047)", (long long)k);
        return 0;
      }
      // 1100 kkkk kkkk kkkk
      out[0] = (uint16_t)(d->opcode | ((unsigned)k & 0x0FFF));
      return 1;
    }
  }
  return 0;
}