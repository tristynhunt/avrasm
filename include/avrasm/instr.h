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
  FORM_NONE,     // nop, ret, sei ...      no operands
  FORM_RD_RR,    // add Rd, Rr             two registers, r0..r31
  FORM_RD,       // com Rd                 one register, r0..r31
  FORM_RD_SAME,  // clr Rd                 one register, used as both Rd and Rr (clr, tst, lsl, rol)
  FORM_RD16,     // ser Rd                 one register, r16..r31
  FORM_RD_K,     // ldi Rd, K              Rd in r16..r31, K an 8-bit immediate
  FORM_RD_NK,    // cbr Rd, K              like andi Rd, ~K
  FORM_RDW_K,    // adiw Rd, K             Rd is r24, r26, r28 or r30 (or a pair r25:r24), K 0..63
  FORM_MOVW,     // movw Rd, Rr            two even registers (or two pairs, r25:r24)
  FORM_MULS,     // muls Rd, Rr            both in r16..r31
  FORM_MUL8,     // mulsu, fmul ...        both in r16..r23
  FORM_RD_A,     // in Rd, A               A an I/O address 0..63
  FORM_A_RR,     // out A, Rr
  FORM_A_B,      // sbi A, b               A an I/O address 0..31, b a bit 0..7
  FORM_R_B,      // sbrc Rr, b / bld Rd, b a register and a bit
  FORM_S,        // bset s                 a flag number 0..7
  FORM_BR_S,     // brbs s, k              7-bit relative branch on flag s
  FORM_BR,       // breq k                 7-bit relative branch, the flag is in the opcode
  FORM_REL12,    // rjmp k                 12-bit relative
  FORM_ABS22,    // jmp k                  22-bit absolute word address, two words
  FORM_LDS,      // lds Rd, k              16-bit data address, two words
  FORM_STS,      // sts k, Rr
  FORM_LD,       // ld Rd, X / X+ / -X / Y / Y+ / -Y / Z / Z+ / -Z
  FORM_ST,       // st X, Rr ...
  FORM_LDD,      // ldd Rd, Y+q / Z+q      q 0..63
  FORM_STD,      // std Y+q, Rr
  FORM_LPM,      // lpm | lpm Rd, Z | lpm Rd, Z+   (elpm too)
  FORM_SPM       // spm | spm Z+
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
