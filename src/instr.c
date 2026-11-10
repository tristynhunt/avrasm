#include "avrasm/instr.h"
#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/lexer.h"
#include "avrasm/operand.h"
#include "avrasm/source.h"
#include "avrasm/tokline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// sorted by name to allow binary search over ops.
// 
// ld and ldd (st and sts have 0x0200) have opcode 0 as the bits depend on the pointer used.
static const InstrDesc table[] = {
  { "adc",    FORM_RD_RR,   0x1C00, 1, 2 },
  { "add",    FORM_RD_RR,   0x0C00, 1, 2 },
  { "adiw",   FORM_RDW_K,   0x9600, 1, 2 },
  { "and",    FORM_RD_RR,   0x2000, 1, 2 },
  { "andi",   FORM_RD_K,    0x7000, 1, 2 },
  { "asr",    FORM_RD,      0x9405, 1, 1 },
  { "bclr",   FORM_S,       0x9488, 1, 1 },
  { "bld",    FORM_R_B,     0xF800, 1, 2 },
  { "brbc",   FORM_BR_S,    0xF400, 1, 2 },
  { "brbs",   FORM_BR_S,    0xF000, 1, 2 },
  { "brcc",   FORM_BR,      0xF400, 1, 1 },
  { "brcs",   FORM_BR,      0xF000, 1, 1 },
  { "break",  FORM_NONE,    0x9598, 1, 0 },
  { "breq",   FORM_BR,      0xF001, 1, 1 },
  { "brge",   FORM_BR,      0xF404, 1, 1 },
  { "brhc",   FORM_BR,      0xF405, 1, 1 },
  { "brhs",   FORM_BR,      0xF005, 1, 1 },
  { "brid",   FORM_BR,      0xF407, 1, 1 },
  { "brie",   FORM_BR,      0xF007, 1, 1 },
  { "brlo",   FORM_BR,      0xF000, 1, 1 },
  { "brlt",   FORM_BR,      0xF004, 1, 1 },
  { "brmi",   FORM_BR,      0xF002, 1, 1 },
  { "brne",   FORM_BR,      0xF401, 1, 1 },
  { "brpl",   FORM_BR,      0xF402, 1, 1 },
  { "brsh",   FORM_BR,      0xF400, 1, 1 },
  { "brtc",   FORM_BR,      0xF406, 1, 1 },
  { "brts",   FORM_BR,      0xF006, 1, 1 },
  { "brvc",   FORM_BR,      0xF403, 1, 1 },
  { "brvs",   FORM_BR,      0xF003, 1, 1 },
  { "bset",   FORM_S,       0x9408, 1, 1 },
  { "bst",    FORM_R_B,     0xFA00, 1, 2 },
  { "call",   FORM_ABS22,   0x940E, 2, 1 },
  { "cbi",    FORM_A_B,     0x9800, 1, 2 },
  { "cbr",    FORM_RD_NK,   0x7000, 1, 2 },
  { "clc",    FORM_NONE,    0x9488, 1, 0 },
  { "clh",    FORM_NONE,    0x94D8, 1, 0 },
  { "cli",    FORM_NONE,    0x94F8, 1, 0 },
  { "cln",    FORM_NONE,    0x94A8, 1, 0 },
  { "clr",    FORM_RD_SAME, 0x2400, 1, 1 },
  { "cls",    FORM_NONE,    0x94C8, 1, 0 },
  { "clt",    FORM_NONE,    0x94E8, 1, 0 },
  { "clv",    FORM_NONE,    0x94B8, 1, 0 },
  { "clz",    FORM_NONE,    0x9498, 1, 0 },
  { "com",    FORM_RD,      0x9400, 1, 1 },
  { "cp",     FORM_RD_RR,   0x1400, 1, 2 },
  { "cpc",    FORM_RD_RR,   0x0400, 1, 2 },
  { "cpi",    FORM_RD_K,    0x3000, 1, 2 },
  { "cpse",   FORM_RD_RR,   0x1000, 1, 2 },
  { "dec",    FORM_RD,      0x940A, 1, 1 },
  { "eicall", FORM_NONE,    0x9519, 1, 0 },
  { "eijmp",  FORM_NONE,    0x9419, 1, 0 },
  { "elpm",   FORM_LPM,     0x95D8, 1, 2 },
  { "eor",    FORM_RD_RR,   0x2400, 1, 2 },
  { "fmul",   FORM_MUL8,    0x0308, 1, 2 },
  { "fmuls",  FORM_MUL8,    0x0380, 1, 2 },
  { "fmulsu", FORM_MUL8,    0x0388, 1, 2 },
  { "icall",  FORM_NONE,    0x9509, 1, 0 },
  { "ijmp",   FORM_NONE,    0x9409, 1, 0 },
  { "in",     FORM_RD_A,    0xB000, 1, 2 },
  { "inc",    FORM_RD,      0x9403, 1, 1 },
  { "jmp",    FORM_ABS22,   0x940C, 2, 1 },
  { "ld",     FORM_LD,      0x0000, 1, 2 },
  { "ldd",    FORM_LDD,     0x0000, 1, 2 },
  { "ldi",    FORM_RD_K,    0xE000, 1, 2 },
  { "lds",    FORM_LDS,     0x9000, 2, 2 },
  { "lpm",    FORM_LPM,     0x95C8, 1, 2 },
  { "lsl",    FORM_RD_SAME, 0x0C00, 1, 1 },
  { "lsr",    FORM_RD,      0x9406, 1, 1 },
  { "mov",    FORM_RD_RR,   0x2C00, 1, 2 },
  { "movw",   FORM_MOVW,    0x0100, 1, 2 },
  { "mul",    FORM_RD_RR,   0x9C00, 1, 2 },
  { "muls",   FORM_MULS,    0x0200, 1, 2 },
  { "mulsu",  FORM_MUL8,    0x0300, 1, 2 },
  { "neg",    FORM_RD,      0x9401, 1, 1 },
  { "nop",    FORM_NONE,    0x0000, 1, 0 },
  { "or",     FORM_RD_RR,   0x2800, 1, 2 },
  { "ori",    FORM_RD_K,    0x6000, 1, 2 },
  { "out",    FORM_A_RR,    0xB800, 1, 2 },
  { "pop",    FORM_RD,      0x900F, 1, 1 },
  { "push",   FORM_RD,      0x920F, 1, 1 },
  { "rcall",  FORM_REL12,   0xD000, 1, 1 },
  { "ret",    FORM_NONE,    0x9508, 1, 0 },
  { "reti",   FORM_NONE,    0x9518, 1, 0 },
  { "rjmp",   FORM_REL12,   0xC000, 1, 1 },
  { "rol",    FORM_RD_SAME, 0x1C00, 1, 1 },
  { "ror",    FORM_RD,      0x9407, 1, 1 },
  { "sbc",    FORM_RD_RR,   0x0800, 1, 2 },
  { "sbci",   FORM_RD_K,    0x4000, 1, 2 },
  { "sbi",    FORM_A_B,     0x9A00, 1, 2 },
  { "sbic",   FORM_A_B,     0x9900, 1, 2 },
  { "sbis",   FORM_A_B,     0x9B00, 1, 2 },
  { "sbiw",   FORM_RDW_K,   0x9700, 1, 2 },
  { "sbr",    FORM_RD_K,    0x6000, 1, 2 },
  { "sbrc",   FORM_R_B,     0xFC00, 1, 2 },
  { "sbrs",   FORM_R_B,     0xFE00, 1, 2 },
  { "sec",    FORM_NONE,    0x9408, 1, 0 },
  { "seh",    FORM_NONE,    0x9458, 1, 0 },
  { "sei",    FORM_NONE,    0x9478, 1, 0 },
  { "sen",    FORM_NONE,    0x9428, 1, 0 },
  { "ser",    FORM_RD16,    0xEF0F, 1, 1 },
  { "ses",    FORM_NONE,    0x9448, 1, 0 },
  { "set",    FORM_NONE,    0x9468, 1, 0 },
  { "sev",    FORM_NONE,    0x9438, 1, 0 },
  { "sez",    FORM_NONE,    0x9418, 1, 0 },
  { "sleep",  FORM_NONE,    0x9588, 1, 0 },
  { "spm",    FORM_SPM,     0x95E8, 1, 1 },
  { "st",     FORM_ST,      0x0200, 1, 2 },
  { "std",    FORM_STD,     0x0200, 1, 2 },
  { "sts",    FORM_STS,     0x9200, 2, 2 },
  { "sub",    FORM_RD_RR,   0x1800, 1, 2 },
  { "subi",   FORM_RD_K,    0x5000, 1, 2 },
  { "swap",   FORM_RD,      0x9402, 1, 1 },
  { "tst",    FORM_RD_SAME, 0x2000, 1, 1 },
  { "wdr",    FORM_NONE,    0x95A8, 1, 0 },
};

#define TABLE_COUNT (sizeof table / sizeof table[0])

static char lowerAscii(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// orders 'name' (any case) against 'lower'. return <0, 0 or >0
static int compareName(const char *name, size_t len, const char *lower)
{
  size_t i = 0;
  while (i < len && lower[i] != '\0') {
    int diff = (unsigned char)lowerAscii(name[i]) - (unsigned char)lower[i];
    if (diff != 0) return diff;
    i++;
  }
  if (i < len) return 1;
  return lower[i] != '\0' ? -1 : 0;
}

const InstrDesc *instrFind(const char *name, size_t len)
{
  size_t lo = 0, hi = TABLE_COUNT;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = compareName(name, len, table[mid].name);
    if (c == 0) return &table[mid];
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return NULL;
}

int instrParseOperands(const InstrDesc *d, const TokenLine *line, uint32_t from, Arena *arena,
  DiagList *diags, Operand out[INSTR_MAX_OPERANDS])
{
  memset(out, 0, INSTR_MAX_OPERANDS * sizeof *out);
  if (d->form != FORM_LPM && d->form != FORM_SPM) {
    return expectOperands(line, from, arena, diags, d->name, d->operandCount, out);
  }
  // lpm, elpm and spm have no operands or all of them
  int n = splitOperands(line, from, arena, diags, out, d->operandCount);
  if (n == 0 || n == (int)d->operandCount) return 1;
  if (n > 0) {
    const Token *end = &line->tokens[line->count - 1];
    diagEmit(diags, DIAG_ERROR, end->span, "%s takes 0 or %d operands, found %d", d->name, d->operandCount, n);
  }
  return 0;
}

// a register operand no lower than r<lo>. reports why not and returns 0
static int needRegister(const Operand *op, const ExprEnv *env, int lo, int hi, int *reg)
{
  if (!operandRegister(op, env->sources, reg)) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "expected a register, r%d to r%d", lo, hi);
    return 0;
  }
  if (*reg < lo || *reg > hi) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "r%d is not allowed here, use r%d to r%d", *reg, lo, hi);
    return 0;
  }
  return 1;
}

// a pair of registers written as "r25:r24".
// stores the lower register, which must also be even.
static int needPair(const Operand *op, const ExprEnv *env, int *low)
{
  const Token *t = op->tokens.tokens;
  if (op->tokens.count == 4 && t[1].kind == TOK_COLON) {
    Token hiToks[2] = { t[0], t[3] }, loToks[2] = { t[2], t[3] };
    Operand hiOp = *op, loOp = *op;
    hiOp.tokens.tokens = hiToks;
    hiOp.tokens.count = 2;
    loOp.tokens.tokens = loToks;
    loOp.tokens.count = 2;
    int hi, lo;
    if (!operandRegister(&hiOp, env->sources, &hi) || !operandRegister(&loOp, env->sources, &lo)) {
      diagEmit(env->diags, DIAG_ERROR, op->span, "expected a register pair like r25:r24");
      return 0;
    }
    // r24:r25 is same as r25:r24
    if (hi < lo) { int t2 = hi; hi = lo; lo = t2; }
    if (hi != lo + 1 || (lo & 1)) {
      diagEmit(env->diags, DIAG_ERROR, op->span, "a register pair is an even register and the next one: r25:r24");
      return 0;
    }
    *low = lo;
    return 1;
  }

  return needRegister(op, env, 0, 31, low);
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

// the distance in words from the instruction after pc to the target.
// the variable cannot overflow and checked against lo..hi
static int needDistance(const Operand *op, const ExprEnv *env, int64_t pc, 
  int64_t lo, int64_t hi, const char *what, int64_t *k)
{
  ExprValue v;
  if (!evalOperand(op, env, &v)) return 0;
  if (!v.known) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "jump target is not known yet");
    return 0;
  }
  
  *k = (int64_t)((uint64_t)v.value - (uint64_t)(pc + 1));
  if (*k < lo || *k > hi) {
    diagEmit(env->diags, DIAG_ERROR, op->span, "%s out of range: %lld words (allowed %lld to %lld)", what,
      (long long)*k, (long long)lo, (long long)hi);
    return 0;
  }
  return 1;
}

typedef enum PtrMode
{
  PTR_X, PTR_X_INC, PTR_X_DEC, PTR_Y, PTR_Y_INC, PTR_Y_DEC, PTR_Z, PTR_Z_INC, PTR_Z_DEC,
  PTR_YQ, PTR_ZQ // Y+q and Z+q, q in *disp
} PtrMode;

// bits of ld for each pointer mode, and st add 0x0200. Y and Z without
// a displacement are ldd with q = 0
static const uint16_t ptrBase[9] = {
  0x900C, 0x900D, 0x900E, 0x8008, 0x9009, 0x900A, 0x8000, 0x9001, 0x9002
};

// returns 0, 1 or 2 if the token is the single letters x, y or z.
// returns -1 otherwise
static int ptrLetter(const ExprEnv *env, const Token *t)
{
  const SourceFile *f = sourceMapGet(env->sources, t->span.file);
  if (f == NULL || t->kind != TOK_IDENT || t->span.end - t->span.start != 1) return -1;
  char c = lowerAscii(f->text[t->span.start]);
  return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : -1;
}

static int parsePointer(const Operand *op, const ExprEnv *env, PtrMode *mode, int64_t *disp)
{
  const Token *t = op->tokens.tokens;
  uint32_t n = op->tokens.count - 1;
  int letter = -1;
  if (n == 2 && t[0].kind == TOK_MINUS) {
    // IDENT PLUS:    X+
    letter = ptrLetter(env, &t[1]);
    if (letter >= 0) { *mode = (PtrMode)(letter * 3 + 2); return 1; }
  } else if (n >= 1) {
    letter = ptrLetter(env, &t[0]);
    if (letter >= 0 && n == 1) { *mode = (PtrMode)(letter * 3); return 1; }
    if (letter >= 0 && n == 2 && t[1].kind == TOK_PLUS) { *mode = (PtrMode)(letter * 3 + 1); return 1; }
    if (letter >= 1 && n >= 3 && t[1].kind == TOK_PLUS) {
      TokenCursor cur;
      ExprValue v;
      cursorInit(&cur, &op->tokens);
      (void)cursorNext(&cur);
      (void)cursorNext(&cur);
      if (!exprEval(&cur, env, &v)) return 0;
      if (!cursorAtEnd(&cur)) {
        diagEmit(env->diags, DIAG_ERROR, cursorPeek(&cur)->span, "unexpected token after the displacement");
        return 0;
      }
      if (!v.known || v.value < 0 || v.value > 63) {
        diagEmit(env->diags, DIAG_ERROR, op->span, "displacement %lld is out of range (0 to 63)", (long long)v.value);
        return 0;
      }
      *mode = letter == 1 ? PTR_YQ : PTR_ZQ;
      *disp = v.value;
      return 1;
    }
  }
  diagEmit(env->diags, DIAG_ERROR, op->span, "expected a pointer: X, Y, Z, X+, -X, Y+q ...");
  return 0;
}

// q is 6-bits and goes to bits 13, 11:10, 2:0
// e.g: 10q0 qq0d dddd 1qqq
static uint16_t dispBits(int64_t q)
{
  unsigned u = (unsigned)q;
  return (uint16_t)(((u & 0x20) << 8) | ((u & 0x18) << 7) | (u & 0x07));
}

// ensure that a displaced pointer cannot reference itself in an operation.
// example: ld r30, Z+ would load into then immedietly destroy r30 by incrementing.
static int clobbersPointer(const Operand *regOp, const ExprEnv *env, int reg, int letter)
{
  int low = 26 + 2 * letter;
  if (reg != low && reg != low + 1) return 0;
  diagEmit(env->diags, DIAG_ERROR, regOp->span,
    "r%d is part of the pointer %c: the result is undefined", reg, "XYZ"[letter]);
  return 1;
}

int instrEncode(const InstrDesc *d, const Operand *ops, int64_t pc, const ExprEnv *env, uint16_t out[2])
{
  int rd = 0, rr = 0;
  int64_t v = 0, w = 0;
  PtrMode mode = PTR_X;
  
  switch (d->form) {
    case FORM_NONE:
      // 0000 0000 0000 0000
      out[0] = d->opcode;
      return 1;
    
    case FORM_RD_RR: {
      int okD = needRegister(&ops[0], env, 0, 31, &rd);
      int okR = needRegister(&ops[1], env, 0, 31, &rr);
      if (!okD || !okR) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)(rr & 0x10) << 5) | ((unsigned)rd << 4) | ((unsigned)rr & 0x0F));
      return 1;
    }

    case FORM_RD: 
      if (!needRegister(&ops[0], env, 0, 31, &rd)) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)rd << 4));
      return 1;

    case FORM_RD_SAME:
      if (!needRegister(&ops[0], env, 0, 31, &rd)) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)(rd & 0x10) << 5) | ((unsigned)rd << 4) | ((unsigned)rd & 0x0F));
      return 1;

    case FORM_RD16:
      if (!needRegister(&ops[0], env, 16, 31, &rd)) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)(rd - 16) << 4));
      return 1;

    case FORM_RD_K:
    case FORM_RD_NK: {
      int okReg = needRegister(&ops[0], env, 16, 31, &rd);
      int okVal = needValue(&ops[1], env, -128, 255, "immediate", &v);
      if (!okReg || !okVal) return 0;
      unsigned k = (unsigned)v & 0xFF;
      if (d->form == FORM_RD_NK) k = ~k & 0xFF;
      // 1110 KKKK dddd KKKK
      out[0] = (uint16_t)(d->opcode | ((k & 0xF0) << 4) | ((unsigned)(rd - 16) << 4) | (k & 0x0F));
      return 1;
    }
    
    case FORM_RDW_K: {
      int okReg = needPair(&ops[0], env, &rd);
      if (okReg && (rd < 24 || (rd & 1))) {
        diagEmit(env->diags, DIAG_ERROR, ops[0].span, "%s works on r24, r26, r28 or r30 (r25:r24 ...)", d->name);
        okReg = 0;
      }
      int okValue = needValue(&ops[1], env, 0, 63, "immediate", &v);
      if (!okReg || !okValue) return 0;
      unsigned k = (unsigned)v;
      out[0] = (uint16_t)(d->opcode | ((k & 0x30) << 2) | ((unsigned)((rd - 24) / 2) << 4) | (k & 0x0F));
      return 1;
    }

    case FORM_MOVW: {
      int okD = needPair(&ops[0], env, &rd);
      int okR = needPair(&ops[1], env, &rr);
      if (okD && (rd & 1)) {
        diagEmit(env->diags, DIAG_ERROR, ops[0].span, "movw works on even registers: r%d is odd", rd);
        okD = 0;
      }
      if (okR && (rr & 1)) {
        diagEmit(env->diags, DIAG_ERROR, ops[1].span, "movw works on even registers: r%d is odd", rr);
        okR = 0;
      }
      if (!okD || !okR) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)(rd / 2) << 4) | ((unsigned)(rr / 2)));
      return 1;
    }

    case FORM_MULS:
    case FORM_MUL8: {
      int hi = d->form == FORM_MULS ? 31 : 23;
      int okD = needRegister(&ops[0], env, 16, hi, &rd);
      int okR = needRegister(&ops[1], env, 16, hi, &rr);
      if (!okD || !okR) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)(rd - 16) << 4) | (unsigned)(rr - 16));
      return 1;
    }

    case FORM_RD_A: {
      int okReg = needRegister(&ops[0], env, 0, 31, &rd);
      int okAddr = needValue(&ops[1], env, 0, 63, "I/O address", &v);
      if (!okReg || !okAddr) return 0;
      unsigned ad = (unsigned)v;
      // 1011 1AAr rrrr AAAA
      out[0] = (uint16_t)(d->opcode | ((ad & 0x30) << 5) | ((unsigned)rd << 4) | (ad & 0x0F));
      return 1;
    }

    case FORM_A_RR: {
      int okAddr = needValue(&ops[0], env, 0, 63, "I/O address", &v);
      int okReg = needRegister(&ops[1], env, 0, 31, &rr);
      if (!okReg || !okAddr) return 0;
      unsigned ad = (unsigned)v;
      // 1011 1AAr rrrr AAAA
      out[0] = (uint16_t)(d->opcode | ((ad & 0x30) << 5) | ((unsigned)rr << 4) | (ad & 0x0F));
      return 1;
    }

    case FORM_A_B: {
      int okAddr = needValue(&ops[0], env, 0, 31, "I/O address", &v);
      int okBit = needValue(&ops[1], env, 0, 7, "bit number", &w);
      if (!okAddr || !okBit) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)v << 3) | (unsigned)w);
      return 1;
    }
    
    case FORM_R_B: {
      int okReg = needRegister(&ops[0], env, 0, 31, &rd);
      int okBit = needValue(&ops[1], env, 0, 7, "bit number", &w);
      if (!okReg || !okBit) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)rd << 4) | (unsigned)w);
      return 1;
    }
    
    case FORM_S:
      if (!needValue(&ops[0], env, 0, 7, "flag number", &v)) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)v << 4));
      return 1;
    
    case FORM_BR_S: {
      int okFlag = needValue(&ops[0], env, 0, 7, "flag number", &v);
      int okDist = needDistance(&ops[1], env, pc, -64, 63, "branch", &w);
      if (!okFlag || !okDist) return 0;
      out[0] = (uint16_t)(d->opcode | (((unsigned)w & 0x7F) << 3) | (unsigned)v);
      return 1;
    }

    case FORM_BR:
      if (!needDistance(&ops[0], env, pc, -64, 63, "branch", &w)) return 0;
      out[0] = (uint16_t)(d->opcode | (((unsigned)w & 0x7F) << 3));
      return 1;

    case FORM_REL12:
      if (!needDistance(&ops[0], env, pc, -2048, 2047, "relative jump", &w)) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)w & 0x0FFF));
      return 1;

    case FORM_ABS22:
      if (!needValue(&ops[0], env, 0, 0x3FFFFF, "jump target", &v)) return 0;
      out[0] = (uint16_t)(d->opcode | (((unsigned)v >> 17) << 4) | (((unsigned)v >> 16) & 1));
      out[1] = (uint16_t)((unsigned)v & 0xFFFF);
      return 2;
    
    case FORM_LDS: {
      int okReg = needRegister(&ops[0], env, 0, 31, &rd);
      int okAddr = needValue(&ops[1], env, 0, 0xFFFF, "data address", &v);
      if (!okReg || !okAddr) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)rd << 4));
      out[1] = (uint16_t)v;
      return 2;
    }

    case FORM_STS: {
      int okAddr = needValue(&ops[0], env, 0, 0xFFFF, "data address", &v);
      int okReg = needRegister(&ops[1], env, 0, 31, &rr);
      if (!okReg || !okAddr) return 0;
      out[0] = (uint16_t)(d->opcode | ((unsigned)rr << 4));
      out[1] = (uint16_t)v;
      return 2;
    }

    case FORM_LD:
    case FORM_ST:
    case FORM_LDD:
    case FORM_STD: {
      int store = d->form == FORM_ST || d->form == FORM_STD;
      int displaced = d->form == FORM_LDD || d->form == FORM_STD;
      const Operand *regOp = store ? &ops[1] : &ops[0];
      const Operand *ptrOp = store ? &ops[0] : &ops[1];
      int okReg, okPtr;
      if (store) {
        okPtr = parsePointer(ptrOp, env, &mode, &v);
        okReg = needRegister(regOp, env, 0, 31, &rd);
      } else {
        okReg = needRegister(regOp, env, 0, 31, &rd);
        okPtr = parsePointer(ptrOp, env, &mode, &v);
      }
      if (okPtr && displaced != (mode >= PTR_YQ)) {
        if (displaced) diagEmit(env->diags, DIAG_ERROR, ptrOp->span, "%s needs a displacement: Y+q or Z+q", d->name);
        else diagEmit(env->diags, DIAG_ERROR, ptrOp->span, "%s takes no displacement, use %sd", d->name, d->name);
        okPtr = 0;
      }
      if (okReg && okPtr && mode < PTR_YQ && mode % 3 != 0) okReg = !clobbersPointer(regOp, env, rd, mode / 3);
      if (!okPtr || !okReg) return 0;
      uint16_t bits = mode < PTR_YQ ? ptrBase[mode] : (uint16_t)((mode == PTR_YQ ? 0x8008 : 0x8000) | dispBits(v));
      out[0] = (uint16_t)(bits | d->opcode | ((unsigned)rd << 4));
      return 1;
    }

    case FORM_LPM: {
      if (ops[0].tokens.tokens == NULL) {
        out[0] = d->opcode;
        return 1;
      }
      int okReg = needRegister(&ops[0], env, 0, 31, &rd);
      int okPtr = parsePointer(&ops[1], env, &mode, &v);
      if (okPtr && mode != PTR_Z && mode != PTR_Z_INC) {
        diagEmit(env->diags, DIAG_ERROR, ops[1].span, "%s takes Z or Z+", d->name);
        okPtr = 0;
      }
      if (okReg && okPtr && mode == PTR_Z_INC) okReg = !clobbersPointer(&ops[0], env, rd, 2);
      if (!okReg || !okPtr) return 0;
      unsigned base = d->opcode == 0x95D8 ? 0x9006 : 0x9004; // elpm : lpm
      out[0] = (uint16_t)(base | (mode == PTR_Z_INC ? 1u : 0u) | ((unsigned)rd << 4));
      return 1;
    }
    
    case FORM_SPM: {
      if (ops[0].tokens.tokens == NULL) {
        out[0] = d->opcode;
        return 1;
      }
      if (!parsePointer(&ops[0], env, &mode, &v)) return 0;
      if (mode != PTR_Z_INC) {
        diagEmit(env->diags, DIAG_ERROR, ops[0].span, "spm takes Z+ or nothing");
        return 0;
      }
      out[0] = (uint16_t)(d->opcode | 0x0010);
      return 1;
    }
  }
  
  return 0;
}