#include "avrasm/lexer.h"
#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Result {
  char tokens[2048];
  char spans[1024];
  uint32_t errors;
  Span firstError;
} Result;

static void put(char *buf, size_t cap, const char *fmt, ...)
{
  size_t used = strlen(buf);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + used, cap - used, fmt, ap);
  va_end(ap);
}

static void lexInto(Result *r, const char *src, size_t len)
{
  Arena arena;
  arenaInit(&arena, 0);
  SourceMap map;
  sourceMapInit(&map, &arena);
  DiagList diags;
  diagListInit(&diags, &arena);
  FileId id = sourceMapAdd(&map, "t.asm", src, len);
  const SourceFile *file = sourceMapGet(&map, id);
  Lexer lx;
  lexerInit(&lx, &map, id, &diags);

  r->tokens[0] = '\0';
  r->spans[0] = '\0';
  for (int i = 0; i < 300; i++) {
    Token t = lexerNext(&lx);
    const char *at = file->text + t.span.start;
    int n = (int)(t.span.end - t.span.start);
    if (i > 0) {
      put(r->tokens, sizeof r->tokens, " ");
      put(r->spans, sizeof r->spans, " ");
    }
    switch (t.kind) {
      case TOK_IDENT: put(r->tokens, sizeof r->tokens, "IDENT(%.*s)", n, at); break;
      case TOK_STRING: put(r->tokens, sizeof r->tokens, "STRING(%.*s)", n, at); break;
      case TOK_NUMBER: put(r->tokens, sizeof r->tokens, "NUMBER(%llu)", (unsigned long long)t.value); break;
      case TOK_CHAR: put(r->tokens, sizeof r->tokens, "CHAR(%llu)", (unsigned long long)t.value); break;
      case TOK_FLOAT: put(r->tokens, sizeof r->tokens, "FLOAT(%g)", t.fvalue); break;
      default: put(r->tokens, sizeof r->tokens, "%s", tokenKindName(t.kind)); break;
    }
    put(r->spans, sizeof r->spans, "%u-%u", t.span.start, t.span.end);
    if (t.kind == TOK_EOF) break;
  }
  r->errors = diags.errorCount;
  r->firstError = diags.head != NULL ? diags.head->span : (Span){ FILE_NONE, 0, 0 };
  arenaFree(&arena);
}

static Result lex(const char *src)
{
  Result r;
  lexInto(&r, src, strlen(src));
  return r;
}

int main(void)
{
  Result r;

  r = lex("ldi r16, 0x1F ; load\n");
  CHECK_STR(r.tokens, "IDENT(ldi) IDENT(r16) COMMA NUMBER(31) NEWLINE EOF");
  CHECK_STR(r.spans, "0-3 4-7 7-8 9-13 20-21 21-21");
  CHECK_INT(r.errors, 0);

  r = lex("10 0x0a $0a 0b1010 010 077 0 0b1100_1010 0b_11_00_10_10_ 0xFF_FF 1_000 $FFFF "
          "0xFFFFFFFFFFFFFFFF");
  CHECK_STR(r.tokens,
            "NUMBER(10) NUMBER(10) NUMBER(10) NUMBER(10) NUMBER(8) NUMBER(63) NUMBER(0) "
            "NUMBER(202) NUMBER(202) NUMBER(65535) NUMBER(1000) NUMBER(65535) "
            "NUMBER(18446744073709551615) EOF");
  CHECK_INT(r.errors, 0);
  r = lex("0X1F 0B101");
  CHECK_STR(r.tokens, "NUMBER(31) NUMBER(5) EOF");
  CHECK_INT(r.errors, 0);
  r = lex("0x1FFFFFFFFFFFFFFFF 18446744073709551616");
  CHECK_STR(r.tokens, "ERROR ERROR EOF");
  CHECK_INT(r.errors, 2);
  r = lex("0xFFFFFFFFFFFFFFFF 18446744073709551615");
  CHECK_STR(r.tokens, "NUMBER(18446744073709551615) NUMBER(18446744073709551615) EOF");
  CHECK_INT(r.errors, 0);

  r = lex("0b2");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_STR(r.spans, "0-3 3-3");
  CHECK_INT(r.errors, 1);
  r = lex("0x");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("$");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("$g");
  CHECK_STR(r.spans, "0-2 2-2");
  CHECK_INT(r.errors, 1);
  r = lex("0b");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("0x_");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("12abc");
  CHECK_STR(r.spans, "0-5 5-5");
  CHECK_INT(r.errors, 1);
  r = lex("0_b11001010");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("0b2 nop");
  CHECK_STR(r.tokens, "ERROR IDENT(nop) EOF");
  CHECK_INT(r.errors, 1);
  r = lex("08 019");
  CHECK_STR(r.tokens, "ERROR ERROR EOF");
  CHECK_INT(r.errors, 2);

  r = lex("1.5 0.25 7.0");
  CHECK_STR(r.tokens, "FLOAT(1.5) FLOAT(0.25) FLOAT(7) EOF");
  CHECK_INT(r.errors, 0);

  r = lex("loop: .db @0 _x a.b R16 LDI");
  CHECK_STR(r.tokens,
            "IDENT(loop) COLON IDENT(.db) IDENT(@0) IDENT(_x) IDENT(a.b) IDENT(R16) IDENT(LDI) EOF");

  r = lex("<< >> <= >= == != && || ++ -- + - * / % & | ^ ~ ! ? : , ( ) = < > #");
  CHECK_STR(r.tokens,
            "SHL SHR LE GE EQEQ NE ANDAND OROR PLUSPLUS MINUSMINUS PLUS MINUS STAR SLASH "
            "PERCENT AMP PIPE CARET TILDE BANG QUESTION COLON COMMA LPAREN RPAREN EQ LT GT "
            "HASH EOF");
  r = lex("<<< &&& !== <=>");
  CHECK_STR(r.tokens, "SHL LT ANDAND AMP NE EQ LE GT EOF");
  r = lex("a--1");
  CHECK_STR(r.tokens, "IDENT(a) MINUSMINUS NUMBER(1) EOF");
  r = lex("a- -1");
  CHECK_STR(r.tokens, "IDENT(a) MINUS MINUS NUMBER(1) EOF");

  r = lex("'a' '\\n' '\\\\' '\\x41' '\\101' '\\0' ' ' ';'");
  CHECK_STR(r.tokens, "CHAR(97) CHAR(10) CHAR(92) CHAR(65) CHAR(65) CHAR(0) CHAR(32) CHAR(59) EOF");
  CHECK_INT(r.errors, 0);
  r = lex("'\\'' '\\q'");
  CHECK_STR(r.tokens, "CHAR(39) CHAR(113) EOF");
  CHECK_INT(r.errors, 0);
  r = lex("''");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("'a");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("'ab' nop");
  CHECK_STR(r.tokens, "ERROR IDENT(nop) EOF");
  CHECK_STR(r.spans, "0-4 5-8 8-8");
  CHECK_INT(r.errors, 1);
  r = lex("'");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);

  r = lex("\"Hello\" \"a;b//c\" \"\"");
  CHECK_STR(r.tokens, "STRING(\"Hello\") STRING(\"a;b//c\") STRING(\"\") EOF");
  CHECK_INT(r.errors, 0);
  r = lex("\"A\\n\"");
  CHECK_STR(r.tokens, "STRING(\"A\\n\") EOF");
  CHECK_STR(r.spans, "0-5 5-5");
  r = lex("\"h\xc3\xa9llo\"");
  CHECK_STR(r.tokens, "STRING(\"h\xc3\xa9llo\") EOF");
  CHECK_INT(r.errors, 0);
  r = lex("\"abc\nnop\n");
  CHECK_STR(r.tokens, "ERROR NEWLINE IDENT(nop) NEWLINE EOF");
  CHECK_STR(r.spans, "0-4 4-5 5-8 8-9 9-9");
  CHECK_INT(r.errors, 1);

  r = lex("nop ; c\nnop // c\nnop /* c */ nop\n");
  CHECK_STR(r.tokens, "IDENT(nop) NEWLINE IDENT(nop) NEWLINE IDENT(nop) IDENT(nop) NEWLINE EOF");
  CHECK_INT(r.errors, 0);
  r = lex("a/*x*/b");
  CHECK_STR(r.tokens, "IDENT(a) IDENT(b) EOF");
  r = lex("1 /* c */ + 2");
  CHECK_STR(r.tokens, "NUMBER(1) PLUS NUMBER(2) EOF");
  r = lex("/* a /* b */ c */");
  CHECK_STR(r.tokens, "IDENT(c) STAR SLASH EOF");
  r = lex("/* // ; */ x");
  CHECK_STR(r.tokens, "IDENT(x) EOF");
  r = lex("1 /2");
  CHECK_STR(r.tokens, "NUMBER(1) SLASH NUMBER(2) EOF");
  r = lex("nop /* abc");
  CHECK_STR(r.tokens, "IDENT(nop) ERROR EOF");
  CHECK_STR(r.spans, "0-3 4-10 10-10");
  CHECK_INT(r.errors, 1);
  r = lex("/*/ x");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("nop ; c \\\nnop");
  CHECK_STR(r.tokens, "IDENT(nop) NEWLINE IDENT(nop) EOF");
  r = lex("nop /* a\nb */ nop");
  CHECK_STR(r.tokens, "IDENT(nop) NEWLINE IDENT(nop) EOF");
  CHECK_STR(r.spans, "0-3 4-13 14-17 17-17");
  CHECK_INT(r.errors, 0);
  r = lex("/* header\n more */\nnop\n");
  CHECK_STR(r.tokens, "NEWLINE NEWLINE IDENT(nop) NEWLINE EOF");
  CHECK_INT(r.errors, 0);
  r = lex("ldi r16, /* a\n b */ 5");
  CHECK_STR(r.tokens, "IDENT(ldi) IDENT(r16) COMMA NEWLINE NUMBER(5) EOF");

  r = lex("a\nb\r\nc\rd");
  CHECK_STR(r.tokens, "IDENT(a) NEWLINE IDENT(b) NEWLINE IDENT(c) NEWLINE IDENT(d) EOF");
  CHECK_STR(r.spans, "0-1 1-2 2-3 3-5 5-6 6-7 7-8 8-8");
  r = lex("\n\n");
  CHECK_STR(r.tokens, "NEWLINE NEWLINE EOF");

  r = lex("a \\\n b");
  CHECK_STR(r.tokens, "IDENT(a) IDENT(b) EOF");
  CHECK_STR(r.spans, "0-1 5-6 6-6");
  r = lex("a \\\r\n b");
  CHECK_STR(r.tokens, "IDENT(a) IDENT(b) EOF");
  r = lex("a \\\r b");
  CHECK_STR(r.tokens, "IDENT(a) IDENT(b) EOF");
  r = lex("a \\\n ; c\nb");
  CHECK_STR(r.tokens, "IDENT(a) NEWLINE IDENT(b) EOF");
  r = lex("a \\ b");
  CHECK_STR(r.tokens, "IDENT(a) ERROR IDENT(b) EOF");
  CHECK_INT(r.errors, 1);
  r = lex("a \\");
  CHECK_STR(r.tokens, "IDENT(a) ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("1 + \\ \n 2");
  CHECK_STR(r.tokens, "NUMBER(1) PLUS ERROR NEWLINE NUMBER(2) EOF");
  CHECK_INT(r.errors, 1);

  r = lex("nop ` nop");
  CHECK_STR(r.tokens, "IDENT(nop) ERROR IDENT(nop) EOF");
  CHECK_STR(r.spans, "0-3 4-5 6-9 9-9");
  CHECK_INT(r.errors, 1);
  r = lex("{}");
  CHECK_STR(r.tokens, "ERROR ERROR EOF");
  CHECK_INT(r.errors, 2);
  r = lex("\x01");
  CHECK_STR(r.tokens, "ERROR EOF");
  CHECK_INT(r.errors, 1);
  r = lex("ldi \xc3\xa9\xc3\xa9 x");
  CHECK_STR(r.tokens, "IDENT(ldi) ERROR IDENT(x) EOF");
  CHECK_STR(r.spans, "0-3 4-8 9-10 10-10");
  CHECK_INT(r.errors, 1);
  r = lex("\xef\xbb\xbf  nop\n");
  CHECK_STR(r.tokens, "ERROR IDENT(nop) NEWLINE EOF");
  CHECK_INT(r.errors, 1);
  Result nul;
  lexInto(&nul, "nop\0nop", 7);
  CHECK_STR(nul.tokens, "IDENT(nop) ERROR IDENT(nop) EOF");
  CHECK_INT(nul.errors, 1);
  CHECK_INT(nul.firstError.start, 3);
  CHECK_INT(nul.firstError.end, 4);
  r = lex("; caf\xc3\xa9\n");
  CHECK_STR(r.tokens, "NEWLINE EOF");
  CHECK_INT(r.errors, 0);

  r = lex("");
  CHECK_STR(r.tokens, "EOF");
  CHECK_STR(r.spans, "0-0");
  {
    Arena arena;
    arenaInit(&arena, 0);
    SourceMap map;
    sourceMapInit(&map, &arena);
    DiagList diags;
    diagListInit(&diags, &arena);
    FileId id = sourceMapAdd(&map, "t.asm", "nop", 3);
    Lexer lx;
    lexerInit(&lx, &map, id, &diags);
    CHECK_INT(lexerNext(&lx).kind, TOK_IDENT);
    for (int i = 0; i < 5; i++) {
      Token t = lexerNext(&lx);
      CHECK_INT(t.kind, TOK_EOF);
      CHECK_INT(t.span.start, 3);
      CHECK_INT(t.span.end, 3);
    }
    Lexer bad;
    lexerInit(&bad, &map, 77, &diags);
    CHECK_INT(lexerNext(&bad).kind, TOK_EOF);
    arenaFree(&arena);
  }

  {
    const char *src = "nop\nldi r16, 1\r\n\n  rjmp x ; c\r.db 1, 2\nend:";
    Arena arena;
    arenaInit(&arena, 0);
    SourceMap map;
    sourceMapInit(&map, &arena);
    DiagList diags;
    diagListInit(&diags, &arena);
    FileId id = sourceMapAdd(&map, "t.asm", src, strlen(src));
    const SourceFile *file = sourceMapGet(&map, id);
    Lexer lx;
    lexerInit(&lx, &map, id, &diags);
    uint32_t newlines = 0, prevEnd = 0;
    int linesOk = 1, orderOk = 1;
    for (;;) {
      Token t = lexerNext(&lx);
      if (t.span.start < prevEnd) orderOk = 0;
      prevEnd = t.span.end;
      if (t.kind == TOK_EOF) break;
      if (sourceLineCol(file, t.span.start).line != newlines) linesOk = 0;
      if (t.kind == TOK_NEWLINE) newlines++;
    }
    CHECK(linesOk);
    CHECK(orderOk);
    CHECK_INT(newlines, 5);
    CHECK_INT(diags.errorCount, 0);
    arenaFree(&arena);
  }

  {
    enum { LONG = 100000 };
    char *big = malloc(LONG + 1);
    REQUIRE(big != NULL);
    memset(big, 'a', LONG);
    big[LONG] = '\0';
    Result lr;
    lexInto(&lr, big, LONG);
    free(big);
    CHECK_STR(lr.spans, "0-100000 100000-100000");
  }

  CHECK_STR(tokenKindName(TOK_COMMA), "COMMA");
  CHECK_STR(tokenKindName(TOK_MINUSMINUS), "MINUSMINUS");
  CHECK_STR(tokenKindName((TokenKind)999), "UNKNOWN");

  return TEST_RESULT();
}