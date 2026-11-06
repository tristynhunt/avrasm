#include "avrasm/tokline.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fx {
  Arena arena;
  SourceMap map;
  DiagList diags;
  Lexer lx;
  const SourceFile *file;
} Fx;

static void fxInit(Fx *f, const char *src, size_t len)
{
  arenaInit(&f->arena, 0);
  sourceMapInit(&f->map, &f->arena);
  diagListInit(&f->diags, &f->arena);
  FileId id = sourceMapAdd(&f->map, "t.asm", src, len);
  f->file = sourceMapGet(&f->map, id);
  lexerInit(&f->lx, &f->map, id, &f->diags);
}

static void fxFree(Fx *f)
{
  arenaFree(&f->arena);
}

static void show(char *out, size_t cap, const Fx *f, const TokenLine *line)
{
  out[0] = '\0';
  for (uint32_t i = 0; i < line->count; i++) {
    const Token *t = &line->tokens[i];
    size_t used = strlen(out);
    const char *sep = i > 0 ? " " : "";
    if (t->kind == TOK_IDENT) {
      snprintf(out + used, cap - used, "%sIDENT(%.*s)", sep, (int)(t->span.end - t->span.start),
               f->file->text + t->span.start);
    } else if (t->kind == TOK_NUMBER) {
      snprintf(out + used, cap - used, "%sNUMBER(%llu)", sep, (unsigned long long)t->value);
    } else {
      snprintf(out + used, cap - used, "%s%s", sep, tokenKindName(t->kind));
    }
  }
}

static const char *oneLine(const char *src)
{
  static char out[512];
  Fx f;
  fxInit(&f, src, strlen(src));
  TokenLine line = lexLine(&f.lx, &f.arena);
  show(out, sizeof out, &f, &line);
  fxFree(&f);
  return out;
}

int main(void)
{
  {
    Fx f;
    fxInit(&f, "ldi r16, 1\nnop\n", 15);
    char s[256];
    TokenLine a = lexLine(&f.lx, &f.arena);
    show(s, sizeof s, &f, &a);
    CHECK_STR(s, "IDENT(ldi) IDENT(r16) COMMA NUMBER(1) NEWLINE");
    CHECK_INT(a.count, 5);
    TokenLine b = lexLine(&f.lx, &f.arena);
    show(s, sizeof s, &f, &b);
    CHECK_STR(s, "IDENT(nop) NEWLINE");
    TokenLine c = lexLine(&f.lx, &f.arena);
    show(s, sizeof s, &f, &c);
    CHECK_STR(s, "EOF");
    CHECK_INT(c.count, 1);
    TokenLine d = lexLine(&f.lx, &f.arena);
    show(s, sizeof s, &f, &d);
    CHECK_STR(s, "EOF");

    show(s, sizeof s, &f, &a);
    CHECK_STR(s, "IDENT(ldi) IDENT(r16) COMMA NUMBER(1) NEWLINE");
    CHECK(a.tokens != b.tokens);
    CHECK(b.tokens != c.tokens);
    fxFree(&f);
  }

  CHECK_STR(oneLine("\n"), "NEWLINE");
  CHECK_STR(oneLine("   ; just a comment\n"), "NEWLINE");
  CHECK_STR(oneLine("nop"), "IDENT(nop) EOF");
  CHECK_STR(oneLine(""), "EOF");
  CHECK_STR(oneLine("nop\r\nnop"), "IDENT(nop) NEWLINE");

  CHECK_STR(oneLine("a, \\\n  b\nc"), "IDENT(a) COMMA IDENT(b) NEWLINE");

  {
    Fx f;
    fxInit(&f, "0b2 nop\n", 8);
    char s[256];
    TokenLine a = lexLine(&f.lx, &f.arena);
    show(s, sizeof s, &f, &a);
    CHECK_STR(s, "ERROR IDENT(nop) NEWLINE");
    CHECK_INT(f.diags.errorCount, 1);
    fxFree(&f);
  }

  {
    enum { N = 10000 };
    char *src = malloc(N * 3 + 2);
    REQUIRE(src != NULL);
    size_t len = 0;
    for (int i = 0; i < N; i++) {
      src[len++] = 'a';
      src[len++] = ',';
      src[len++] = ' ';
    }
    src[len++] = '\n';
    Fx f;
    fxInit(&f, src, len);
    free(src);
    TokenLine a = lexLine(&f.lx, &f.arena);
    CHECK_INT(a.count, 2 * N + 1);
    int ok = 1;
    for (int i = 0; i < N; i++) {
      if (a.tokens[2 * i].kind != TOK_IDENT) ok = 0;
      if (a.tokens[2 * i].span.start != (uint32_t)(3 * i)) ok = 0;
      if (a.tokens[2 * i + 1].kind != TOK_COMMA) ok = 0;
    }
    CHECK(ok);
    CHECK_INT(a.tokens[a.count - 1].kind, TOK_NEWLINE);
    fxFree(&f);
  }

  {
    Fx f;
    fxInit(&f, "ldi r16, 1\n", 11);
    TokenLine line = lexLine(&f.lx, &f.arena);
    TokenCursor c;
    cursorInit(&c, &line);
    CHECK_INT(cursorPeek(&c)->kind, TOK_IDENT);
    CHECK_INT(cursorPeekAt(&c, 0)->kind, TOK_IDENT);
    CHECK_INT(cursorPeekAt(&c, 2)->kind, TOK_COMMA);
    CHECK_INT(cursorPeekAt(&c, 3)->kind, TOK_NUMBER);
    CHECK_INT(cursorPeekAt(&c, 4)->kind, TOK_NEWLINE);
    CHECK_INT(cursorPeekAt(&c, 5)->kind, TOK_NEWLINE);
    CHECK_INT(cursorPeekAt(&c, 4000000000u)->kind, TOK_NEWLINE);
    CHECK(!cursorAtEnd(&c));

    CHECK_INT(cursorNext(&c)->kind, TOK_IDENT);
    CHECK_INT(cursorPeek(&c)->kind, TOK_IDENT);
    CHECK(!cursorAccept(&c, TOK_COMMA));
    CHECK_INT(cursorPeek(&c)->kind, TOK_IDENT);
    CHECK(cursorAccept(&c, TOK_IDENT));
    CHECK(cursorAccept(&c, TOK_COMMA));
    CHECK_INT(cursorPeek(&c)->value, 1);
    CHECK_INT(cursorPeekAt(&c, 1)->kind, TOK_NEWLINE);
    CHECK_INT(cursorPeekAt(&c, 4294967295u)->kind, TOK_NEWLINE);
    cursorNext(&c);
    CHECK(cursorAtEnd(&c));
    for (int i = 0; i < 5; i++) {
      CHECK_INT(cursorNext(&c)->kind, TOK_NEWLINE);
      CHECK(cursorAtEnd(&c));
    }
    CHECK_INT(cursorPeekAt(&c, 3)->kind, TOK_NEWLINE);
    CHECK(cursorAccept(&c, TOK_NEWLINE));
    CHECK(cursorAtEnd(&c));
    fxFree(&f);
  }

  {
    Fx f;
    fxInit(&f, "", 0);
    TokenLine line = lexLine(&f.lx, &f.arena);
    TokenCursor c;
    cursorInit(&c, &line);
    CHECK(cursorAtEnd(&c));
    CHECK_INT(cursorPeekAt(&c, 9)->kind, TOK_EOF);
    CHECK_INT(cursorNext(&c)->kind, TOK_EOF);
    CHECK_INT(cursorPeek(&c)->kind, TOK_EOF);
    fxFree(&f);
  }

  return TEST_RESULT();
}