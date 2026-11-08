#ifndef AVRASM_INSTR_H
#define AVRASM_INSTR_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/diag.h"
#include "avrasm/expr.h"
#include "avrasm/operand.h"

#define INSTR_MAX_OPERANDS 2

// how an instruction's operands become bits. one form
// covers every instruction that is encoded the same way.
typedef enum InstrForm
{
  FORM_NONE,  // nop          no operands
  FORM_RD_K,  // ldi Rd, K    Rd in r16..r31, K an 8-bit
  FORM_A_RR,  // out A, Rr    A an I/O address 0..63, Rr any register
  FORM_REL12  // rjmp k       target label or word address, 12-bit relative
} InstrForm;

typedef struct InstrDesc
{
  const char *name;   // lower case
  InstrForm form;
  uint16_t opcode;    // the bits with no operand changes
  uint8_t words;      // size in 16-bit words
  uint8_t operandCount;
} InstrDesc;

// the instruction called name[0..len) in any case. NULL if no mnemonic exists.
const InstrDesc *instrFind(const char *name, size_t len);

// parses the operands following a mnemonic. ensures that it has exactly d->operandCount
// operands. returns 1 and fills out, or 0 after one error.
int instrParseOperands(const InstrDesc *d, const TokenLine *line, uint32_t from, Arena *arena,
  DiagList *diags, Operand out[INSTR_MAX_OPERANDS]);

// encodes one instruction that sits at word address pc into out[0..words).
// returns the number of words written or 0 after reporting one error per bad operand.
// needs every value to be known.
int instrEncode(const InstrDesc *d, const Operand *ops, int64_t pc, 
  const ExprEnv *env, uint16_t out[2]);

#endif

/*

FORM_NONE       0000 0000 0000 0000
FORM_RD_K       1110 KKKK dddd KKKK       d = Rd - 16 (only r16..r31 exist here), K = low 8 bits
FORM_A_RR       1011 1AAr rrrr AAAA       A = I/O address 0..63, split 2 + 4 bits around the register
FORM_REL12      1100 kkkk kkkk kkkk       k = target - (pc + 1) in WORDS, 12 bits, two's complement

*/