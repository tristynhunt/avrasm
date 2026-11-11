#include "avrasm/assemble.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

typedef struct Run {
  AvrCtx ctx;
  Image flash;
  Image eeprom;
  int ok;
  const char *flashText;
  const char *eepromText;
} Run;

static void run(Run *r, const char *src)
{
  ctxInit(&r->ctx, avrDiskVfs());
  FileId id = sourceMapAdd(&r->ctx.sources, "t.asm", src, strlen(src));
  imageInit(&r->flash, &r->ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  imageInit(&r->eeprom, &r->ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  r->ok = assembleAll(&r->ctx, id, &r->flash, &r->eeprom);
  r->flashText = imageToText(&r->flash, &r->ctx.arena);
  r->eepromText = imageToText(&r->eeprom, &r->ctx.arena);
}

typedef struct Good {
  const char *src;
  const char *flash;
  const char *eeprom;
  unsigned warnings;
} Good;

static const Good goods[] = {
  { ".db 1, 2, 3, 4\n",
    "000000: 01 02 03 04\n", "", 0 },
  { ".db 1, 2, 3\n",
    "000000: 01 02 03 00\n", "", 1 },
  { ".db \"AB\"\n",
    "000000: 41 42\n", "", 0 },
  { ".db \"A\\n\"\n",
    "000000: 41 5c 6e 00\n", "", 2 },
  { ".db \"ab\", 0\n",
    "000000: 61 62 00 00\n", "", 1 },
  { ".db 'a', 'b'\n",
    "000000: 61 62\n", "", 0 },
  { ".db -1, 255\n",
    "000000: ff ff\n", "", 0 },
  { ".dw 0x1234, 0xABCD\n",
    "000000: 34 12 cd ab\n", "", 0 },
  { "nop\nfoo: .dw foo, foo + 1\n",
    "000000: 00 00 01 00 02 00\n", "", 0 },
  { ".dw -1, 65535\n",
    "000000: ff ff ff ff\n", "", 0 },
  { ".db 1\n.db 2\n",
    "000000: 01 00 02 00\n", "", 2 },
  { ".db 1, 2, 3\nnop\n",
    "000000: 01 02 03 00 00 00\n", "", 1 },
  { ".org 2\n.db 1, 2\n",
    "000004: 01 02\n", "", 0 },
  { ".equ k = 7\n.db k, k * 2\n",
    "000000: 07 0e\n", "", 0 },
  { ".db 1, 2\n.cseg\n.db 3, 4\n",
    "000000: 01 02 03 04\n", "", 0 },
  { ".eseg\n.db 1, 2, 3\n.dw 0x1234\n",
    "", "000000: 01 02 03 34 12\n", 0 },
  { ".eseg\n.org 0x10\n.db 9\n",
    "", "000010: 09\n", 0 },
  { ".cseg\nnop\n.eseg\n.db 5\n.cseg\nnop\n",
    "000000: 00 00 00 00\n", "000000: 05\n", 0 },
  { ".eseg\n.db 1\n.cseg\nnop\n.eseg\n.db 2\n",
    "000000: 00 00\n", "000000: 01 02\n", 0 },
  { ".eseg\nlab: .db 1, 2\nlab2: .db 3\n.cseg\nldi r16, lab2\n",
    "000000: 02 e0\n", "000000: 01 02 03\n", 0 },
  { ".dseg\n.org 0x100\nvar: .byte 2\nvar2: .byte 1\n.cseg\nlds r16, var2\n",
    "000000: 00 91 02 01\n", "", 0 },
  { ".dseg\n.byte 5\n.cseg\nstart: rjmp start\n",
    "000000: ff cf\n", "", 0 },
  { ".dseg\nv: .byte 1\n.cseg\nlds r16, v\n",
    "000000: 00 91 60 00\n", "", 0 },
  { ".dseg\n.byte 3\nv: .byte 1\n.cseg\nlds r16, v\n",
    "000000: 00 91 63 00\n", "", 0 },
  { ".eseg\n.byte 1\n",
    "", "", 0 },
  { ".eseg\n.byte 3\nv: .db 1\n.cseg\nldi r16, v\n",
    "000000: 03 e0\n", "000003: 01\n", 0 },
  { ".dseg\n.byte 0\nv: .byte 1\n.cseg\nlds r16, v\n",
    "000000: 00 91 60 00\n", "", 0 },
  { ".cseg\nnop\n.dseg\n.org 0x100\n.byte 4\n.cseg\nfoo: nop\n.dseg\nbar: .byte 1\n.cseg\nldi r16, foo\nldi r17, bar & 0xff\n",
    "000000: 00 00 00 00 01 e0 14 e0\n", "", 0 },
  { ".set k = 1\nldi r16, k\n.set k = 2\nldi r17, k\n",
    "000000: 01 e0 12 e0\n", "", 0 },
  { ".set a = 4\n.org a\nnop\n.set a = 8\n.org a\nnop\n",
    "000008: 00 00\n000010: 00 00\n", "", 0 },
  { ".set k = 1\n.set k = k + 1\nldi r16, k\n",
    "000000: 02 e0\n", "", 0 },
  { ".def temp = r16\nldi temp, 1\n",
    "000000: 01 e0\n", "", 0 },
  { ".def lo = r24\n.def hi = r25\nadiw hi:lo, 1\n",
    "000000: 01 96\n", "", 0 },
  { ".def Temp = r16\nldi TEMP, 1\n.undef temp\n.def temp = r17\nldi temp, 1\n",
    "000000: 01 e0 11 e0\n", "", 0 },
  { ".def a = r16\n.def b = r16\nldi a, 1\nldi b, 2\n",
    "000000: 01 e0 02 e0\n", "", 1 },
  { ".db \"a\\n\", 0\n",
    "000000: 61 5c 6e 00\n", "", 1 },
  { "org: nop\n.equ min = 1\nldi r16, min\n",
    "000000: 00 00 01 e0\n", "", 0 },
  { ".def a = r16\n.def A = r16\nldi a, 1\n",
    "000000: 01 e0\n", "", 0 },
  { ".def a = r16\n.undef a\n.def a = r17\nldi a, 1\n",
    "000000: 11 e0\n", "", 0 },
  { ".def zl = r30\nmov zl, r1\n",
    "000000: e1 2d\n", "", 0 },
};

typedef struct Bad {
  const char *src;
  unsigned errors;
  uint32_t at;
} Bad;

static const Bad bads[] = {
  { ".dseg\nnop\n", 1, 6 },
  { ".eseg\nnop\n", 1, 6 },
  { ".byte 1\n", 1, 0 },
  { ".dseg\n.byte -1\n", 1, 12 },
  { ".db\n", 1, 0 },
  { ".db 256, 0\n", 1, 4 },
  { ".db 0, -129\n", 1, 7 },
  { ".dw 65536\n", 1, 4 },
  { ".dw -32769\n", 1, 4 },
  { ".db \"\"\n", 1, 4 },
  { ".db \"\", 1\n", 1, 4 },
  { ".dseg\n.db 1\n", 1, 6 },
  { ".dseg\n.dw 1\n", 1, 6 },
  { ".def a = r16\n.def a = r17\n", 1, 18 },
  { ".dw \"ab\"\n", 1, 4 },
  { ".db nothing, 0\n", 1, 4 },
  { ".set k = 1\n.equ k = 2\n", 1, 16 },
  { ".equ k = 1\n.set k = 2\n", 1, 16 },
  { "foo: nop\n.set foo = 2\n", 1, 14 },
  { ".def a = r32\n", 1, 9 },
  { ".def x = 5\n", 1, 9 },
  { ".undef a\n", 1, 7 },
  { ".def a = r16\n.def b = a\n", 1, 22 },
  { "x: nop\n", 1, 0 },
  { ".equ y = 1\n", 1, 5 },
  { ".set z = 1\n", 1, 5 },
  { "r5: nop\n", 1, 0 },
  { ".def x = r16\n", 1, 5 },
  { "nop: nop\n", 1, 0 },
  { "ldi r16, k\n.set k = 1\n", 1, 0 },
  { "ldi r16, k\n.set k = 1\n.set k = 2\n", 1, 0 },
  { "BRNE: nop\n", 1, 0 },
  { "des: nop\n", 1, 0 },
  { "low: nop\n", 1, 0 },
  { "pc: nop\n", 1, 0 },
  { ".equ high = 1\n", 1, 5 },
  { ".set Byte1 = 1\n", 1, 5 },
  { ".def ldi = r16\n", 1, 5 },
  { ".equ a = 1\n.def a = r16\n", 1, 16 },
  { "a: nop\n.def a = r16\n", 1, 12 },
  { ".def a = r16\n.equ a = 1\n", 1, 18 },
  { ".org 0\nnop\n.org 0\nnop\n", 1, 18 },
  { ".db 1, 2\n.org 0\n.db 3, 4\n", 1, 16 },
  { ".def a = r16\n.undef a\nldi a, 1\n", 1, 26 },
  { ".def a = r16\nldi r17, a\n", 1, 22 },
};

int main(void)
{
  for (size_t i = 0; i < sizeof goods / sizeof goods[0]; i++) {
    Run r;
    run(&r, goods[i].src);
    if (!r.ok || r.ctx.diags.count != goods[i].warnings || strcmp(r.flashText, goods[i].flash) != 0 ||
        strcmp(r.eepromText, goods[i].eeprom) != 0) {
      fprintf(stderr, "good #%zu: ok=%d, %u diagnostics (want %u), flash \"%s\" (want \"%s\"), eeprom \"%s\" (want \"%s\") for:\n%s\n",
              i, r.ok, r.ctx.diags.count, goods[i].warnings, r.flashText, goods[i].flash, r.eepromText, goods[i].eeprom, goods[i].src);
      for (Diag *d = r.ctx.diags.head; d; d = d->next) fprintf(stderr, "  %s\n", d->message);
      check_failures_++;
    }
    ctxFree(&r.ctx);
  }

  for (size_t i = 0; i < sizeof bads / sizeof bads[0]; i++) {
    Run r;
    run(&r, bads[i].src);
    if (r.ok || r.ctx.diags.errorCount != bads[i].errors || r.ctx.diags.head == NULL ||
        r.ctx.diags.head->span.start != bads[i].at) {
      fprintf(stderr, "bad #%zu: ok=%d, %u errors (want %u), first at %u (want %u) for:\n%s\n", i, r.ok,
              r.ctx.diags.errorCount, bads[i].errors, r.ctx.diags.head ? r.ctx.diags.head->span.start : 9999,
              bads[i].at, bads[i].src);
      for (Diag *d = r.ctx.diags.head; d; d = d->next) fprintf(stderr, "  %s\n", d->message);
      check_failures_++;
    }
    ctxFree(&r.ctx);
  }

  {
    Run r;
    run(&r, "ldi temp, 1\n.def temp = r16\n");
    CHECK(!r.ok);
    ctxFree(&r.ctx);
  }

  {
    Run r;
    run(&r, ".dseg\n.byte 4\n");
    CHECK(r.ok);
    CHECK(strcmp(r.flashText, "") == 0);
    CHECK(strcmp(r.eepromText, "") == 0);
    ctxFree(&r.ctx);
  }

  return TEST_RESULT();
}