#include "avrasm/assemble.h"
#include "avrasm/lint.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
  const char *path;
  const char *text;
} fixtures[] = {
  { "dev.inc", ".equ INT_VECTORS_SIZE = 8\n.equ FLASHEND = 0x3FFF\n.equ SREG = 0x3F\n.equ PORTB = 0x05\n" },
  { "tiny.inc", ".device ATtiny13\n#pragma AVRPART ADMIN PART_NAME ATtiny13\n#pragma AVRPART CORE CORE_VERSION V2\n"
               "#pragma AVRPART MEMORY PROG_FLASH 1024\n#pragma AVRPART MEMORY EEPROM 64\n"
               "#pragma AVRPART MEMORY INT_SRAM SIZE 64\n#pragma AVRPART MEMORY INT_SRAM START_ADDR 0x60\n"
               ".equ FLASHEND = 0x01FF\n" },
};

static char *readFixture(void *user, const char *path, size_t *len)
{
  (void)user;
  for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
    if (strcmp(path, fixtures[i].path) != 0) continue;
    *len = strlen(fixtures[i].text);
    char *copy = malloc(*len + 1);
    memcpy(copy, fixtures[i].text, *len + 1);
    return copy;
  }
  return NULL;
}

#define GOOD_IRQ \
  ".include \"dev.inc\"\n" \
  ".org 0\n  rjmp reset\n.org 2\n  rjmp handler\n.org INT_VECTORS_SIZE\n" \
  "reset:\n  sei\nmain:\n  rcall work\n  rjmp main\n" \
  "work:\n  push r16\n  ldi r16, 1\n  pop r16\n  ret\n" \
  "handler:\n  push r16\n  in r16, SREG\n  push r16\n  inc r17\n  pop r16\n  out SREG, r16\n  pop r16\n  reti\n" \
  "table: .db 1, 2\n"

static const struct {
  const char *src;
  const char *want;
} rows[] = {
  { "loop: rjmp loop\n", "" },
  { GOOD_IRQ, "" },
  { ".macro one\n  nop\n.endm\nloop: sbrs r16, 0\n  one\n  rjmp loop\n", "" },
  { ".macro m\n  sbrs r16, 0\n  inc r17\n  inc r18\n.endm\nloop: m\n  rjmp loop\n", "" },
  { "main: ldi r30, low(t)\n  ldi r31, high(t)\n  ijmp\nt: rjmp a\na: rjmp main\n", "" },
  { ".include \"dev.inc\"\nmain: ldi r16, 1\n  inc r16\n  rjmp main\n", "" },
  { ".include \"dev.inc\"\n  rjmp main\nmain: ldi r16, 1\n  inc r16\n  rjmp main\n", "" },
  { "main: rcall f\n  rjmp main\nf: push r16\n  breq done\n  inc r16\ndone: pop r16\n  ret\n", "" },
  { "main: sbrs r16, 0\n  jmp main\n  rjmp main\n", "" },
  { ".dseg\nv: .byte 1\n.cseg\nmain: lds r16, v\n  sts 0x20, r16\n  rjmp main\n", "" },

  { ".macro two\n  inc r17\n  inc r18\n.endm\nloop: sbrs r16, 0\n  two\n  rjmp loop\n", "6:the sbrs before this macro call skips only its first instruction, the other 1 still runs" },
  { ".macro three\n  nop\n  nop\n  nop\n.endm\nloop: cpse r1, r2\n  three\n  rjmp loop\n", "7:the cpse before this macro call skips only its first instruction, the other 2 still run" },
  { ".include \"dev.inc\"\nmain: sts PORTB, r16\n  rjmp main\n", "2:sts with address 0x05 writes register r5" },
  { "main: lds r16, 0x10\n  rjmp main\n", "1:lds with address 0x10 reads register r16" },
  { "nop\nldi r16, 1\n", "2:execution runs on past this instruction into memory with no code" },
  { "nop\n.db 1, 2\n", "1:execution runs on past this instruction into the data at word address 0x0001" },
  { "rjmp 100\n", "1:this goes to word address 0x0064, where the program has no code" },
  { "main: rcall 100\n  rjmp main\n", "1:this goes to word address 0x0064" },
  { "main: breq t\n  rjmp main\nt: .dw 0\n", "1:this goes to the data at word address 0x0002" },
  { "main: rjmp main\n  nop\n  nop\n  .db 1, 2\n  nop\n", "2:nothing runs this code|5:nothing runs this code" },
  { ".include \"tiny.inc\"\nrjmp 0x1F0\n", "2:this goes to word address 0x01F0" },
  { "main: rcall f\n  rjmp main\nf: push r16\n  ret\n", "4:ret with 1 byte pushed and not popped" },
  { "main: rcall f\n  rjmp main\nf: pop r16\n  pop r17\n  ret\n", "5:ret after popping 2 bytes more than were pushed" },
  { "main: rcall f\n  rjmp main\nf: push r16\n  breq done\n  pop r16\ndone: ret\n",
    "6:ret with 1 byte pushed" },
  { "main: push r16\n  rjmp main\n", "1:the stack holds 1 byte more here on one path than on another" },
  { "main: nop\nf: ret\n", "2:the reset code runs into this ret, which nothing called" },
  { ".include \"dev.inc\"\n.org 0\n  rjmp main\n.org 2\n  rjmp h\n.org 8\nmain: rjmp main\nh: inc r17\n  reti\n",
    "8:this interrupt handler changes the status flags before it saves SREG" },
  { ".include \"dev.inc\"\n.org 0\n  rjmp main\n.org 2\n  rjmp h\n.org 8\nmain: rjmp main\nh: nop\n  ret\n",
    "9:an interrupt handler that ends with ret leaves interrupts off: use reti" },
  { ".include \"dev.inc\"\n.org 0\n  rjmp main\n.org 2\n  reti\n.org 8\nmain: rjmp main\n", "" },
  { ".macro bad\n  sts 3, r16\n.endm\nmain: bad\n  rjmp main\n", "2:sts with address 0x03 writes register r3" },
};

static uint32_t lineOf(const AvrCtx *ctx, Span span)
{
  return sourceLineCol(sourceMapGet(&ctx->sources, span.file), span.start).line + 1;
}

static void checkRow(size_t n)
{
  AvrCtx ctx;
  ctxInit(&ctx, (AvrVfs){ readFixture, NULL });
  FileId id = sourceMapAdd(&ctx.sources, "t.asm", rows[n].src, strlen(rows[n].src));
  Image flash, eeprom;
  imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  Outline o;
  outlineInit(&o, &ctx.arena);
  AsmOptions opt = { NULL, 0, NULL, 0, &o };
  int result = assembleOpt(&ctx, id, &flash, &eeprom, &opt);
  if (result != 1 || ctx.diags.count != 0) {
    fprintf(stderr, "row %zu: does not assemble cleanly (result %d, %u diagnostics):\n%s\n", n, result,
            ctx.diags.count, rows[n].src);
    check_failures_++;
    ctxFree(&ctx);
    return;
  }
  lintProgram(&ctx.diags, &o, &flash);

  int wanted = 0, ok = 1;
  char want[512];
  snprintf(want, sizeof want, "%s", rows[n].want);
  for (char *part = strtok(want, "|"); part != NULL; part = strtok(NULL, "|")) {
    wanted++;
    uint32_t line = (uint32_t)strtoul(part, NULL, 10);
    const char *words = strchr(part, ':') + 1;
    int found = 0;
    for (const Diag *d = ctx.diags.head; d != NULL; d = d->next) {
      found |= d->severity == DIAG_WARNING && lineOf(&ctx, d->span) == line && strstr(d->message, words) != NULL;
    }
    ok &= found;
  }
  int warnings = 0;
  for (const Diag *d = ctx.diags.head; d != NULL; d = d->next) warnings += d->severity == DIAG_WARNING;
  if (!ok || warnings != wanted) {
    fprintf(stderr, "row %zu: want \"%s\", got:\n", n, rows[n].want);
    for (const Diag *d = ctx.diags.head; d != NULL; d = d->next) {
      fprintf(stderr, "  %u: %s\n", lineOf(&ctx, d->span), d->message);
    }
    fprintf(stderr, "for:\n%s\n", rows[n].src);
    check_failures_++;
  }
  ctxFree(&ctx);
}

int main(void)
{
  for (size_t n = 0; n < sizeof rows / sizeof rows[0]; n++) checkRow(n);
  return TEST_RESULT();
}