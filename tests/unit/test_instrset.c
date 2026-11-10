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
  Span none = { FILE_NONE, 0, 0 };
  symtabAdd(&f->syms, "K", 1, SYM_EQU, 0x55, none, NULL);
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

static void good(const char *src, int64_t pc, int words, unsigned w0, unsigned w1)
{
  Fx f;
  fxInit(&f);
  uint16_t w[2] = { 0xDEAD, 0xBEEF };
  int n = enc(&f, src, pc, w);
  if (n != words || w[0] != w0 || (words == 2 && w[1] != w1) || f.diags.count != 0) {
    fprintf(stderr, "good(\"%s\" at %lld): %d words 0x%04X 0x%04X, want %d words 0x%04X 0x%04X, %u diagnostics\n", src,
            (long long)pc, n, w[0], w[1], words, w0, w1, f.diags.count);
    check_failures_++;
  }
  if (words == 1) CHECK_INT(w[1], 0xBEEF);
  arenaFree(&f.arena);
}

static void bad(const char *src, int64_t pc, unsigned errors, uint32_t at)
{
  Fx f;
  fxInit(&f);
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

typedef struct Row {
  int pc;
  const char *src;
  int words;
  unsigned w0, w1;
} Row;

static const Row rows[] = {
  { 0, "add r1, r2", 1, 0x0C12, 0 },
  { 0, "add r0, r0", 1, 0x0C00, 0 },
  { 0, "add r31, r31", 1, 0x0FFF, 0 },
  { 0, "add r17, r5", 1, 0x0D15, 0 },
  { 0, "adc r16, r31", 1, 0x1F0F, 0 },
  { 0, "sub r3, r18", 1, 0x1A32, 0 },
  { 0, "sbc r30, r1", 1, 0x09E1, 0 },
  { 0, "and r10, r20", 1, 0x22A4, 0 },
  { 0, "or r10, r20", 1, 0x2AA4, 0 },
  { 0, "eor r10, r20", 1, 0x26A4, 0 },
  { 0, "cp r9, r27", 1, 0x169B, 0 },
  { 0, "cpc r9, r27", 1, 0x069B, 0 },
  { 0, "cpse r2, r3", 1, 0x1023, 0 },
  { 0, "mov r31, r0", 1, 0x2DF0, 0 },
  { 0, "mul r16, r17", 1, 0x9F01, 0 },
  { 0, "com r5", 1, 0x9450, 0 },
  { 0, "neg r6", 1, 0x9461, 0 },
  { 0, "swap r7", 1, 0x9472, 0 },
  { 0, "inc r8", 1, 0x9483, 0 },
  { 0, "dec r9", 1, 0x949A, 0 },
  { 0, "asr r10", 1, 0x94A5, 0 },
  { 0, "lsr r11", 1, 0x94B6, 0 },
  { 0, "ror r31", 1, 0x95F7, 0 },
  { 0, "push r12", 1, 0x92CF, 0 },
  { 0, "pop r13", 1, 0x90DF, 0 },
  { 0, "clr r5", 1, 0x2455, 0 },
  { 0, "clr r31", 1, 0x27FF, 0 },
  { 0, "tst r5", 1, 0x2055, 0 },
  { 0, "tst r17", 1, 0x2311, 0 },
  { 0, "lsl r17", 1, 0x0F11, 0 },
  { 0, "rol r17", 1, 0x1F11, 0 },
  { 0, "ser r16", 1, 0xEF0F, 0 },
  { 0, "ser r31", 1, 0xEFFF, 0 },
  { 0, "ldi r16, 0x55", 1, 0xE505, 0 },
  { 0, "subi r17, 1", 1, 0x5011, 0 },
  { 0, "sbci r18, 0xFF", 1, 0x4F2F, 0 },
  { 0, "andi r19, 0x0F", 1, 0x703F, 0 },
  { 0, "ori r20, 0xF0", 1, 0x6F40, 0 },
  { 0, "cpi r21, 100", 1, 0x3654, 0 },
  { 0, "sbr r22, 0x81", 1, 0x6861, 0 },
  { 0, "cbr r23, 0x81", 1, 0x777E, 0 },
  { 0, "cbr r16, 0", 1, 0x7F0F, 0 },
  { 0, "cbr r16, 255", 1, 0x7000, 0 },
  { 0, "adiw r24, 1", 1, 0x9601, 0 },
  { 0, "adiw r26, 63", 1, 0x96DF, 0 },
  { 0, "adiw r28, 0x21", 1, 0x96A1, 0 },
  { 0, "adiw r30, 2", 1, 0x9632, 0 },
  { 0, "adiw r25:r24, 1", 1, 0x9601, 0 },
  { 0, "adiw r24:r25, 1", 1, 0x9601, 0 },
  { 0, "sbiw r24, 1", 1, 0x9701, 0 },
  { 0, "sbiw r31:r30, 63", 1, 0x97FF, 0 },
  { 0, "movw r24, r0", 1, 0x01C0, 0 },
  { 0, "movw r25:r24, r1:r0", 1, 0x01C0, 0 },
  { 0, "movw r24:r25, r0:r1", 1, 0x01C0, 0 },
  { 0, "movw r30, r28", 1, 0x01FE, 0 },
  { 0, "movw r31:r30, r29:r28", 1, 0x01FE, 0 },
  { 0, "muls r16, r31", 1, 0x020F, 0 },
  { 0, "mulsu r16, r23", 1, 0x0307, 0 },
  { 0, "fmul r17, r22", 1, 0x031E, 0 },
  { 0, "fmuls r18, r21", 1, 0x03A5, 0 },
  { 0, "fmulsu r23, r16", 1, 0x03F8, 0 },
  { 0, "in r0, 0", 1, 0xB000, 0 },
  { 0, "in r5, 0x3F", 1, 0xB65F, 0 },
  { 0, "in r31, 63", 1, 0xB7FF, 0 },
  { 0, "out 0x3f, r16", 1, 0xBF0F, 0 },
  { 0, "out 0x10, r5", 1, 0xBA50, 0 },
  { 0, "sbi 0, 0", 1, 0x9A00, 0 },
  { 0, "sbi 31, 7", 1, 0x9AFF, 0 },
  { 0, "cbi 0x05, 3", 1, 0x982B, 0 },
  { 0, "sbic 0x1f, 2", 1, 0x99FA, 0 },
  { 0, "sbis 0x10, 6", 1, 0x9B86, 0 },
  { 0, "sbrc r0, 0", 1, 0xFC00, 0 },
  { 0, "sbrs r31, 7", 1, 0xFFF7, 0 },
  { 0, "sbrc r17, 3", 1, 0xFD13, 0 },
  { 0, "bld r1, 1", 1, 0xF811, 0 },
  { 0, "bst r31, 7", 1, 0xFBF7, 0 },
  { 0, "bset 0", 1, 0x9408, 0 },
  { 0, "bset 7", 1, 0x9478, 0 },
  { 0, "bclr 3", 1, 0x94B8, 0 },
  { 0, "bclr 7", 1, 0x94F8, 0 },
  { 100, "brbs 0, 101", 1, 0xF000, 0 },
  { 100, "brbs 7, 164", 1, 0xF1FF, 0 },
  { 100, "brbs 3, 37", 1, 0xF203, 0 },
  { 100, "brbc 0, 101", 1, 0xF400, 0 },
  { 100, "brbc 7, 164", 1, 0xF5FF, 0 },
  { 100, "brbc 5, 37", 1, 0xF605, 0 },
  { 100, "brcs 164", 1, 0xF1F8, 0 },
  { 100, "brlo 37", 1, 0xF200, 0 },
  { 100, "brcc 101", 1, 0xF400, 0 },
  { 100, "brsh 102", 1, 0xF408, 0 },
  { 100, "breq 164", 1, 0xF1F9, 0 },
  { 100, "brne 37", 1, 0xF601, 0 },
  { 100, "brmi 150", 1, 0xF18A, 0 },
  { 100, "brpl 150", 1, 0xF58A, 0 },
  { 100, "brvs 150", 1, 0xF18B, 0 },
  { 100, "brvc 150", 1, 0xF58B, 0 },
  { 100, "brlt 150", 1, 0xF18C, 0 },
  { 100, "brge 150", 1, 0xF58C, 0 },
  { 100, "brhs 150", 1, 0xF18D, 0 },
  { 100, "brhc 150", 1, 0xF58D, 0 },
  { 100, "brts 150", 1, 0xF18E, 0 },
  { 100, "brtc 150", 1, 0xF58E, 0 },
  { 100, "brie 150", 1, 0xF18F, 0 },
  { 100, "brid 150", 1, 0xF58F, 0 },
  { 100, "rjmp 101", 1, 0xC000, 0 },
  { 100, "rjmp 100", 1, 0xCFFF, 0 },
  { 100, "rjmp 2148", 1, 0xC7FF, 0 },
  { 100, "rjmp 0", 1, 0xCF9B, 0 },
  { 100, "rcall 101", 1, 0xD000, 0 },
  { 100, "rcall 2148", 1, 0xD7FF, 0 },
  { 100, "rcall 0", 1, 0xDF9B, 0 },
  { 0, "jmp 0", 2, 0x940C, 0x0000 },
  { 0, "jmp 0x100", 2, 0x940C, 0x0100 },
  { 0, "jmp 0x3FFF", 2, 0x940C, 0x3FFF },
  { 0, "jmp 0x12345", 2, 0x940D, 0x2345 },
  { 0, "jmp 0x3FFFFF", 2, 0x95FD, 0xFFFF },
  { 0, "call 0", 2, 0x940E, 0x0000 },
  { 0, "call 0x100", 2, 0x940E, 0x0100 },
  { 0, "call 0x2ABCD", 2, 0x941E, 0xABCD },
  { 0, "lds r0, 0", 2, 0x9000, 0x0000 },
  { 0, "lds r31, 0xFFFF", 2, 0x91F0, 0xFFFF },
  { 0, "lds r5, 0x200", 2, 0x9050, 0x0200 },
  { 0, "sts 0x200, r5", 2, 0x9250, 0x0200 },
  { 0, "sts 0xFFFF, r31", 2, 0x93F0, 0xFFFF },
  { 0, "sts 0, r0", 2, 0x9200, 0x0000 },
  { 0, "ld r1, X", 1, 0x901C, 0 },
  { 0, "ld r1, X+", 1, 0x901D, 0 },
  { 0, "ld r1, -X", 1, 0x901E, 0 },
  { 0, "ld r1, Y", 1, 0x8018, 0 },
  { 0, "ld r1, Y+", 1, 0x9019, 0 },
  { 0, "ld r1, -Y", 1, 0x901A, 0 },
  { 0, "ld r1, Z", 1, 0x8010, 0 },
  { 0, "ld r29, Z+", 1, 0x91D1, 0 },
  { 0, "ld r1, -Z", 1, 0x9012, 0 },
  { 0, "st X, r1", 1, 0x921C, 0 },
  { 0, "st X+, r1", 1, 0x921D, 0 },
  { 0, "st -X, r1", 1, 0x921E, 0 },
  { 0, "st Y, r1", 1, 0x8218, 0 },
  { 0, "st Y+, r1", 1, 0x9219, 0 },
  { 0, "st -Y, r1", 1, 0x921A, 0 },
  { 0, "st Z, r1", 1, 0x8210, 0 },
  { 0, "st Z+, r29", 1, 0x93D1, 0 },
  { 0, "st -Z, r1", 1, 0x9212, 0 },
  { 0, "ldd r1, Y+0", 1, 0x8018, 0 },
  { 0, "ldd r1, Y+63", 1, 0xAC1F, 0 },
  { 0, "ldd r1, Z+1", 1, 0x8011, 0 },
  { 0, "ldd r31, Z+63", 1, 0xADF7, 0 },
  { 0, "ldd r5, Y+33", 1, 0xA059, 0 },
  { 0, "std Y+0, r1", 1, 0x8218, 0 },
  { 0, "std Y+63, r31", 1, 0xAFFF, 0 },
  { 0, "std Z+2, r1", 1, 0x8212, 0 },
  { 0, "std Z+40, r7", 1, 0xA670, 0 },
  { 0, "lpm", 1, 0x95C8, 0 },
  { 0, "lpm r1, Z", 1, 0x9014, 0 },
  { 0, "lpm r29, Z+", 1, 0x91D5, 0 },
  { 0, "lpm r0, Z", 1, 0x9004, 0 },
  { 0, "elpm", 1, 0x95D8, 0 },
  { 0, "elpm r1, Z", 1, 0x9016, 0 },
  { 0, "elpm r2, Z+", 1, 0x9027, 0 },
  { 0, "spm", 1, 0x95E8, 0 },
  { 0, "spm Z+", 1, 0x95F8, 0 },
  { 0, "ld r26, X", 1, 0x91AC, 0 },
  { 0, "ld r30, Z", 1, 0x81E0, 0 },
  { 0, "ldd r30, Z+1", 1, 0x81E1, 0 },
  { 0, "lpm r30, Z", 1, 0x91E4, 0 },
  { 0, "ldd r28, Y+1", 1, 0x81C9, 0 },
  { 0, "break", 1, 0x9598, 0 },
  { 0, "sleep", 1, 0x9588, 0 },
  { 0, "wdr", 1, 0x95A8, 0 },
  { 0, "ret", 1, 0x9508, 0 },
  { 0, "reti", 1, 0x9518, 0 },
  { 0, "ijmp", 1, 0x9409, 0 },
  { 0, "icall", 1, 0x9509, 0 },
  { 0, "eijmp", 1, 0x9419, 0 },
  { 0, "eicall", 1, 0x9519, 0 },
  { 0, "sec", 1, 0x9408, 0 },
  { 0, "clc", 1, 0x9488, 0 },
  { 0, "sen", 1, 0x9428, 0 },
  { 0, "cln", 1, 0x94A8, 0 },
  { 0, "sez", 1, 0x9418, 0 },
  { 0, "clz", 1, 0x9498, 0 },
  { 0, "sei", 1, 0x9478, 0 },
  { 0, "cli", 1, 0x94F8, 0 },
  { 0, "ses", 1, 0x9448, 0 },
  { 0, "cls", 1, 0x94C8, 0 },
  { 0, "sev", 1, 0x9438, 0 },
  { 0, "clv", 1, 0x94B8, 0 },
  { 0, "set", 1, 0x9468, 0 },
  { 0, "clt", 1, 0x94E8, 0 },
  { 0, "seh", 1, 0x9458, 0 },
  { 0, "clh", 1, 0x94D8, 0 },
};

int main(void)
{
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    good(rows[i].src, rows[i].pc, rows[i].words, rows[i].w0, rows[i].w1);
  }

  good("ADD R1, R2", 0, 1, 0x0C12, 0);
  good("Ld r1, x+", 0, 1, 0x901D, 0);
  good("ST -z, R2", 0, 1, 0x9222, 0);
  good("LPM R1, z+", 0, 1, 0x9015, 0);
  good("MOVW R25:R24, R1:R0", 0, 1, 0x01C0, 0);
  good("Jmp 0x100", 0, 2, 0x940C, 0x0100);

  good("ldi r16, low(K + 0x100)", 0, 1, 0xE505, 0);
  good("adiw r24, K - 0x50", 0, 1, 0x9605, 0);
  good("ldd r1, Y+K - 0x50", 0, 1, 0x801D, 0);
  good("sbi 0x10, 1+1", 0, 1, 0x9A82, 0);
  good("lds r5, K*4", 0, 2, 0x9050, 0x0154);
  good("brne K", 0x50, 1, 0xF421, 0);

  CHECK_INT(instrFind("jmp", 3)->words, 2);
  CHECK_INT(instrFind("CALL", 4)->words, 2);
  CHECK_INT(instrFind("lds", 3)->words, 2);
  CHECK_INT(instrFind("sts", 3)->words, 2);
  CHECK_INT(instrFind("rjmp", 4)->words, 1);
  CHECK_INT(instrFind("ld", 2)->words, 1);

  CHECK(instrFind("ldx", 3) == NULL);
  CHECK(instrFind("l", 1) == NULL);
  CHECK(instrFind("lddd", 4) == NULL);
  CHECK(instrFind("sub", 2) == NULL);
  CHECK(instrFind("zzz", 3) == NULL);
  CHECK(instrFind("", 0) == NULL);
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    const char *s = rows[i].src;
    size_t len = strcspn(s, " ");
    CHECK(instrFind(s, len) != NULL);
  }

  bad("add r32, r1", 0, 1, 4);
  bad("add r1, 5", 0, 1, 8);
  bad("add r32, r33", 0, 2, 4);
  bad("add r1", 0, 1, 6);
  bad("com r1, r2", 0, 1, 8);
  bad("clr 5", 0, 1, 4);
  bad("ser r15", 0, 1, 4);
  bad("cbr r15, 1", 0, 1, 4);
  bad("cbr r16, 256", 0, 1, 9);
  bad("adiw r16, 1", 0, 1, 5);
  bad("adiw r25, 1", 0, 1, 5);
  bad("adiw r24, 64", 0, 1, 10);
  bad("adiw r24:r23, 1", 0, 1, 5);
  bad("sbiw r23:r22, 1", 0, 1, 5);
  bad("movw r1, r3", 0, 2, 5);
  bad("movw r24, r3", 0, 1, 10);
  bad("movw r25:r24, r2:r1", 0, 1, 14);
  bad("muls r15, r16", 0, 1, 5);
  bad("mulsu r24, r16", 0, 1, 6);
  bad("fmul r16, r24", 0, 1, 10);
  bad("in r1, 64", 0, 1, 7);
  bad("sbi 32, 0", 0, 1, 4);
  bad("sbi 0, 8", 0, 1, 7);
  bad("sbrc r1, 8", 0, 1, 9);
  bad("bset 8", 0, 1, 5);
  bad("bclr -1", 0, 1, 5);
  bad("brbs 8, 101", 100, 1, 5);
  bad("brbs 0, 200", 100, 1, 8);
  bad("breq 165", 100, 1, 5);
  bad("breq 36", 100, 1, 5);
  bad("rcall 2149", 100, 1, 6);
  bad("jmp -1", 0, 1, 4);
  bad("jmp 0x400000", 0, 1, 4);
  bad("call 0x400000", 0, 1, 5);
  bad("lds r1, 0x10000", 0, 1, 8);
  bad("sts 0x10000, r1", 0, 1, 4);
  bad("lds r32, 0", 0, 1, 4);
  bad("ld r1, W", 0, 1, 7);
  bad("ld r1, X+1", 0, 1, 7);
  bad("ld r1, Y+1", 0, 1, 7);
  bad("ldd r1, Y", 0, 1, 8);
  bad("ldd r1, X+1", 0, 1, 8);
  bad("ldd r1, -Y", 0, 1, 8);
  bad("ldd r1, Y+64", 0, 1, 8);
  bad("st X, r32", 0, 1, 6);
  bad("st r1, X", 0, 2, 3);
  bad("ld r26, X+", 0, 1, 3);
  bad("ld r27, -X", 0, 1, 3);
  bad("ld r28, Y+", 0, 1, 3);
  bad("ld r30, Z+", 0, 1, 3);
  bad("st X+, r26", 0, 1, 7);
  bad("st -Y, r28", 0, 1, 7);
  bad("lpm r30, Z+", 0, 1, 4);
  bad("elpm r31, Z+", 0, 1, 5);
  bad("ld r29, Y+", 0, 1, 3);
  bad("ld r31, -Z", 0, 1, 3);
  bad("st Z+, r30", 0, 1, 7);
  bad("adiw r26:r24, 1", 0, 1, 5);
  bad("adiw r25:r25, 1", 0, 1, 5);
  bad("movw r26:r24, r0", 0, 1, 5);
  bad("spm X+", 0, 1, 4);
  bad("lpm r1", 0, 1, 6);
  bad("lpm r1, X", 0, 1, 8);
  bad("lpm r1, -Z", 0, 1, 8);
  bad("elpm r1, Y", 0, 1, 9);
  bad("lpm 5, Z", 0, 1, 4);
  bad("ret r1", 0, 1, 4);

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