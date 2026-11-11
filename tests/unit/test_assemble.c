#include "avrasm/assemble.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

typedef struct Run {
  AvrCtx ctx;
  Image image;
  int ok;
  const char *text;
} Run;

static void run(Run *r, const char *src)
{
  ctxInit(&r->ctx, avrDiskVfs());
  FileId id = sourceMapAdd(&r->ctx.sources, "t.asm", src, strlen(src));
  imageInit(&r->image, &r->ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  r->ok = assemble(&r->ctx, id, &r->image);
  r->text = imageToText(&r->image, &r->ctx.arena);
}

static void good(const char *src, const char *want)
{
  Run r;
  run(&r, src);
  if (!r.ok || r.ctx.diags.count != 0 || strcmp(r.text, want) != 0) {
    fprintf(stderr, "good: ok=%d, %u diagnostics, image \"%s\" (want \"%s\") for:\n%s\n", r.ok,
            r.ctx.diags.count, r.text, want, src);
    for (Diag *d = r.ctx.diags.head; d; d = d->next) fprintf(stderr, "  %s\n", d->message);
    check_failures_++;
  }
  ctxFree(&r.ctx);
}

static void bad(const char *src, unsigned errors, uint32_t at)
{
  Run r;
  run(&r, src);
  if (r.ok || r.ctx.diags.errorCount != errors || r.ctx.diags.head == NULL || r.ctx.diags.head->span.start != at) {
    fprintf(stderr, "bad: ok=%d, %u errors (want %u), first at %u (want %u) for:\n%s\n", r.ok,
            r.ctx.diags.errorCount, errors, r.ctx.diags.head ? r.ctx.diags.head->span.start : 9999, at, src);
    for (Diag *d = r.ctx.diags.head; d; d = d->next) fprintf(stderr, "  %s\n", d->message);
    check_failures_++;
  }
  ctxFree(&r.ctx);
}

int main(void)
{
  good("  .cseg\n  .org 0\nstart:\n  ldi r16, 0x55\n  out 0x3f, r16\n  rjmp start\n",
       "000000: 05 e5 0f bf fd cf\n");

  good("", "");
  good("\n\n   \n", "");
  good("; just a comment\n\n   ; and another\n", "");
  good("  .cseg\n", "");

  good("nop", "000000: 00 00\n");
  good("nop\nnop", "000000: 00 00 00 00\n");
  good("here: rjmp here", "000000: ff cf\n");

  good("a:\n  nop\n  rjmp a\n", "000000: 00 00 fe cf\n");
  good("a: nop\n   rjmp a\n", "000000: 00 00 fe cf\n");
  good("  rjmp fwd\n  nop\nfwd: nop\n", "000000: 01 c0 00 00 00 00\n");
  good("loop:\nrjmp loop\n", "000000: ff cf\n");
  good("A:\n  rjmp a\n  rjmp A\n", "000000: ff cf fe cf\n");
  good("a:\nb:\n  rjmp b\n", "000000: ff cf\n");

  good("ldi r16, 0x55 ; load\nout 0x3f, r16 ; store\n", "000000: 05 e5 0f bf\n");
  good("nop\r\nnop\r\n", "000000: 00 00 00 00\n");
  good("; header\nstart: ; the entry\n  nop\n", "000000: 00 00\n");

  good(".org 0x10\nnop\n", "000020: 00 00\n");
  good("nop\n.org 4\nnop\n", "000000: 00 00\n000008: 00 00\n");
  good(".org 2\nnop\n.org 0\nnop\n", "000000: 00 00\n000004: 00 00\n");
  good(".org 0\nnop\n.org 1\nnop\n", "000000: 00 00 00 00\n");
  good("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n",
       "000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00\n000010: 00 00\n");
  good(".org 4\nl: rjmp l\n", "000008: ff cf\n");
  good(".equ base = 8\n.org base * 2\nnop\n", "000020: 00 00\n");
  good("nop\n.org pc + 2\nnop\n", "000000: 00 00\n000006: 00 00\n");
  good(".org 0x20000\n", "");
  bad(".org 0x20001\n", 1, 5);
  bad(".org -1\n", 1, 5);
  bad(".org nosuch\n", 1, 5);
  bad(".org 0x20000\nnop\n", 1, 13);
  bad(".org\n", 1, 4);
  bad(".org 1, 2\n", 1, 8);

  good(".equ K = 0x55\nldi r16, K\n", "000000: 05 e5\n");
  good(".equ K = 0x50 + 5\nldi r16, k\n", "000000: 05 e5\n");
  good(".equ A = 3\n.equ B = A * 2\nldi r16, B\n", "000000: 06 e0\n");
  good(".EQU K=5\nLDI R16, K\n", "000000: 05 e0\n");
  good("nop\nnop\nldi r16, pc\n", "000000: 00 00 00 00 02 e0\n");
  good(".equ here = pc\nnop\n.equ there = pc\nldi r16, there\n", "000000: 00 00 01 e0\n");
  good("a: nop\n.equ b = a + 1\nrjmp b\n", "000000: 00 00 ff cf\n");
  bad(".equ A = B\n.equ B = 1\n", 1, 9);
  bad(".equ A = 1\n.equ A = 2\n", 1, 16);
  bad("a: nop\n.equ a = 3\n", 1, 12);
  bad(".equ\n", 1, 4);
  bad(".equ A\n", 1, 5);
  bad(".equ A =\n", 1, 5);
  bad(".equ A 5\n", 1, 5);
  bad(".equ A 5 6\n", 1, 5);
  bad(".equ 5 = 6\n", 1, 5);
  bad(".equ A = 1 2\n", 1, 11);

  good("Start:\nLDI R16, 0x55\nOUT 0x3F, R16\nRJMP START\n", "000000: 05 e5 0f bf fd cf\n");
  good(".CSEG\n.ORG 0\nnop\n", "000000: 00 00\n");

  bad("frob r1\n", 1, 0);
  bad("5 nop\n", 1, 0);
  bad("0b2 nop\n", 1, 0);
  bad("a: b: nop\n", 1, 3);
  bad("ldi r16\n", 1, 7);
  bad("ldi r16, 256\n", 1, 9);
  bad("ldi r16, 0b2\n", 1, 9);
  bad(".cseg 5\n", 1, 6);
  bad(".dd\n", 1, 0);
  bad(".dq\n", 1, 0);
  bad(".or 1\n", 1, 0);
  bad(".cse\n", 1, 0);
  bad(".orgg 1\n", 1, 0);
  bad(".equ2 A = 1\n", 1, 0);
  bad("frob\nldi\nrjmp nosuch\n", 3, 0);
  bad("a: nop\na: nop\n", 1, 7);
  bad("rjmp 4000\n", 1, 5);
  {
    Run r;
    run(&r, "frob 1\nfwd: nop\nrjmp fwd\n");
    CHECK(!r.ok);
    CHECK_INT(r.ctx.diags.errorCount, 1);
    CHECK_STR(r.text, "000000: 00 00 fe cf\n");
    CHECK_STR(r.ctx.diags.head->message, "unknown instruction 'frob'");
    ctxFree(&r.ctx);
  }
  {
    Run r;
    run(&r, "rjmp\nhere: nop\nldi r16, here\n");
    CHECK(!r.ok);
    CHECK_INT(r.ctx.diags.errorCount, 1);
    CHECK_INT(r.ctx.diags.head->span.start, 4);
    CHECK_STR(r.text, "000002: 00 00 01 e0\n");
    ctxFree(&r.ctx);
  }

  {
    Run r;
    run(&r, "a: nop\na: nop\n");
    CHECK_INT(r.ctx.diags.count, 2);
    CHECK_INT(r.ctx.diags.head->severity, DIAG_ERROR);
    CHECK_STR(r.ctx.diags.head->message, "symbol 'a' is already defined");
    CHECK_INT(r.ctx.diags.head->span.start, 7);
    CHECK_INT(r.ctx.diags.head->next->severity, DIAG_INFO);
    CHECK_INT(r.ctx.diags.head->next->span.start, 0);
    ctxFree(&r.ctx);
  }

  {
    Run r;
    run(&r, ".org 0\nldi r16, 1\n.org 0\nldi r16, 2\n");
    CHECK(!r.ok);
    CHECK_INT(r.ctx.diags.count, 1);
    CHECK_INT(r.ctx.diags.head->severity, DIAG_ERROR);
    CHECK_INT(r.ctx.diags.head->span.start, 25);
    ctxFree(&r.ctx);
  }

  {
    enum { N = 5000 };
    static char src[N * 16 + 128];
    size_t used = 0;
    for (int i = 0; i < N; i++) used += (size_t)sprintf(src + used, "l%d: nop\n", i);
    used += (size_t)sprintf(src + used, ".equ last = l%d\nldi r16, last & 0xff\n", N - 1);
    Run r;
    run(&r, src);
    CHECK(r.ok);
    CHECK_INT(r.ctx.diags.count, 0);
    CHECK(strstr(r.text, "002710: 07 e8\n") != NULL);
    ctxFree(&r.ctx);
  }

  {
    const char *src = ".macro m\n  ldi r16, @0\n.endm\nm 300\n";
    bad(src, 1, (uint32_t)(strstr(src, "300") - src));
    good(".macro m\n  ldi r16, @0\n.endm\nm 0x55\n", "000000: 05 e5\n");
  }

  return TEST_RESULT();
}