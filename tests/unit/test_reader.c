#include "avrasm/reader.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entry {
  const char *path;
  const char *text;
} Entry;

static const Entry files[] = {
    {"main.asm", NULL},
    {"x.inc", "inc1\ninc2\n"},
    {"inc/y.inc", "from_y\n.exit\nnever\n"},
    {"dirs/z.inc", "from_z\n"},
    {NULL, NULL},
};

static char *readFile(void *user, const char *path, size_t *len)
{
  (void)user;
  for (const Entry *e = files + 1; e->path != NULL; e++) {
    if (strcmp(e->path, path) != 0) continue;
    *len = strlen(e->text);
    char *copy = malloc(*len + 1);
    memcpy(copy, e->text, *len + 1);
    return copy;
  }
  return NULL;
}

static const char *tokText(AvrCtx *c, const Token *t, size_t *len)
{
  *len = t->span.end - t->span.start;
  return sourceMapGet(&c->sources, t->span.file)->text + t->span.start;
}

static AvrCtx *theCtx;

static int hookEval(void *user, TokenCursor *cur, int64_t *value)
{
  (void)user;
  size_t len;
  const Token *t = cursorNext(cur);
  const char *s = tokText(theCtx, t, &len);
  *value = t->kind == TOK_NUMBER ? strtol(s, NULL, 0) : 0;
  return 1;
}

static int hookIsDefined(void *user, const char *name, size_t len)
{
  (void)user;
  (void)len;
  return name[0] == 'd';
}

static int hookNameOk(void *user, const Token *name)
{
  (void)user;
  (void)name;
  return 1;
}

static const char *lines(const char *main, const char *const *defs, uint32_t defCount)
{
  static char out[1024];
  AvrCtx ctx;
  theCtx = &ctx;
  ctxInit(&ctx, (AvrVfs){readFile, NULL});
  FileId id = sourceMapAdd(&ctx.sources, "main.asm", main, strlen(main));
  ReaderHooks hooks = {hookEval, hookIsDefined, hookNameOk, NULL};
  const char *dirs[] = {"dirs"};
  Reader *r = readerCreate(&ctx, &hooks, id, dirs, 1);
  for (uint32_t i = 0; i < defCount; i++) readerDefine(r, defs[i]);
  out[0] = '\0';
  size_t used = 0;
  ReaderLine rl;
  while (readerNext(r, &rl)) {
    if (rl.line.tokens[0].kind == TOK_NEWLINE || rl.line.tokens[0].kind == TOK_EOF) continue;
    for (uint32_t i = 0; rl.line.tokens[i].kind != TOK_NEWLINE && rl.line.tokens[i].kind != TOK_EOF; i++) {
      size_t n;
      const char *txt = tokText(&ctx, &rl.line.tokens[i], &n);
      used += (size_t)snprintf(out + used, sizeof out - used, "%s%.*s", used && out[used - 1] != '|' && i == 0 ? " | " : i ? " " : "", (int)n, txt);
    }
    if (rl.scope != 0) used += (size_t)snprintf(out + used, sizeof out - used, "@%u", rl.scope);
  }
  if (ctx.diags.errorCount != 0) {
    snprintf(out + used, sizeof out - used, "%sERR %u", used ? " | " : "", ctx.diags.errorCount);
  }
  ctxFree(&ctx);
  return out;
}

int main(void)
{
  CHECK_STR(lines("nop\n; c\n\nldi r16, 1\n", NULL, 0), "nop | ldi r16 , 1");

  CHECK_STR(lines(".if 1\na\n.else\nb\n.endif\n", NULL, 0), "a");
  CHECK_STR(lines(".if 0\na\n.elif 1\nb\n.else\nc\n.endif\n", NULL, 0), "b");
  CHECK_STR(lines(".if 0\n.if 1\na\n.endif\n.endif\nz\n", NULL, 0), "z");
  CHECK_STR(lines(".ifdef dx\na\n.endif\n.ifndef dx\nb\n.endif\n", NULL, 0), "a");
  CHECK_STR(lines(".if 1\na\n", NULL, 0), "a | ERR 1");
  CHECK_STR(lines(".endif\n", NULL, 0), "ERR 1");
  CHECK_STR(lines(".if 1\n.else\n.else\n.endif\n", NULL, 0), "ERR 1");

  CHECK_STR(lines("a\n.include \"x.inc\"\nb\n", NULL, 0), "a | inc1 | inc2 | b");
  CHECK_STR(lines(".include \"z.inc\"\n", NULL, 0), "from_z");
  CHECK_STR(lines(".include <z.inc>\n", NULL, 0), "from_z");
  CHECK_STR(lines(".include <x.inc>\n", NULL, 0), "ERR 1");
  CHECK_STR(lines(".include \"nofile.inc\"\n", NULL, 0), "ERR 1");
  CHECK_STR(lines(".include \"inc/y.inc\"\nafter\n", NULL, 0), "from_y | after");

  CHECK_STR(lines("#define N 5\nldi r16, N\n", NULL, 0), "ldi r16 , 5");
  CHECK_STR(lines("#define ADD(a, b) a + b\n.db ADD(1, 2)\n", NULL, 0), ".db 1 + 2");
  CHECK_STR(lines("#ifdef FLAG\na\n#else\nb\n#endif\n", NULL, 0), "b");
  CHECK_STR(lines("#ifdef FLAG\na\n#else\nb\n#endif\n", (const char *const[]){"FLAG"}, 1), "a");
  CHECK_STR(lines("#if V > 2\na\n#endif\n", (const char *const[]){"V=3"}, 1), "a");
  CHECK_STR(lines("#error stop\nafter\n", NULL, 0), "after | ERR 1");

  CHECK_STR(lines(".macro m\nldi r16, @0\nrjmp l\n.endm\nm 7\nm 8\n", NULL, 0),
            "ldi r16 , 7@1 | rjmp l@1 | ldi r16 , 8@2 | rjmp l@2");
  CHECK_STR(lines(".macro m\nnop\n.endm\nM\n", NULL, 0), "nop@1");
  CHECK_STR(lines("m\n", NULL, 0), "m");
  CHECK_STR(lines(".macro m\n.macro n\n.endm\n.endm\n", NULL, 0), "ERR 2");
  CHECK_STR(lines(".macro m\nnop\n", NULL, 0), "ERR 1");
  CHECK_STR(lines(".macro m\nnop\n.endm\n.macro m\nnop\n.endm\n", NULL, 0), "ERR 1");

  CHECK_STR(lines(".macro m\nnop\n.endm\nlbl: m\n", NULL, 0), "lbl : | nop@1");

  CHECK_STR(lines(".message \"hi\"\n.warning \"w\"\nnop\n", NULL, 0), "nop");
  CHECK_STR(lines(".error \"stop\"\nnop\n", NULL, 0), "nop | ERR 1");

  return TEST_RESULT();
}