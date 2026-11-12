#ifndef AVRASM_LINT_H
#define AVRASM_LINT_H

#include "avrasm/diag.h"
#include "avrasm/image.h"
#include "avrasm/outline.h"

// static analysis. warnings for code that assembles but is probably wrong. it reads the
// program as the processor would run it such as the instructions in the flash image,
// where the outline says they are, followed from the reset address and the interrupt vectors.
//
//   - a skip (sbrs, sbrc, sbis, sbic, cpse) right before a macro call of more than one instruction
//     but only the first one is skipped
//   - lds or sts with an address below 0x20, which is a register and not the I/O register it was
//     probably meant to be (sts PORTB, r16 writes r5)
//   - execution that runs into .db/.dw data or past the end of the program or jumps or calls to
//     where the program has no code
//   - code that nothing runs (only when the program has no ijmp/icall whose targets are unknown)
//   - a routine whose push and pop do not balance at its ret, and a loop that pushes more than it
//     pops each time round
//   - an interrupt handler that ends in ret instead of reti, or changes the status flags before it
//     has saved SREG
//   - the reset code running into a ret or reti that nothing called
//
// interrupt vectors are only known when the program has a device file (INT_VECTORS_SIZE) and starts
// with a jump at address 0, as every program that uses interrupts does

// adds the warnings to diags. call it only after the program assembled without errors.
// 'outline' and 'flash' are what assembleOpt filled.
void lintProgram(DiagList *diags, const Outline *outline, const Image *flash);

#endif