#include "avrasm/tokline.h"

#include <stdint.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/lexer.h"

TokenLine lexLine(Lexer *lx, Arena *arena)
{
  uint32_t count = 0, cap = 16;
  Token *tokens = arenaAlloc(arena, cap * sizeof *tokens, _Alignof(Token));
  
  while (1) {
    if (count >= cap) {
      uint32_t newCap = cap * 2;
      Token *bigger = arenaAlloc(arena, newCap * sizeof *bigger, _Alignof(Token));
      memcpy(bigger, tokens, count * sizeof *tokens);
      tokens = bigger;
      cap = newCap;
    }
    Token t = lexerNext(lx);
    tokens[count++] = t;
    if (t.kind == TOK_NEWLINE || t.kind == TOK_EOF) break;
  }

  TokenLine line = { tokens, count };
  return line;
}

void cursorInit(TokenCursor *c, const TokenLine *line)
{
  c->line = line;
  c->pos = 0;
}

const Token *cursorPeekAt(const TokenCursor *c, uint32_t n)
{
  uint32_t last = c->line->count - 1;
  uint32_t i = n > last - c->pos ? last : c->pos + n;
  return &c->line->tokens[i];
}

const Token *cursorPeek(const TokenCursor *c)
{
  return cursorPeekAt(c, 0);
}

const Token *cursorNext(TokenCursor *c)
{
  const Token *t = cursorPeek(c);
  if (c->pos < c->line->count - 1) c->pos++;
  return t;
}

int cursorAtEnd(const TokenCursor *c)
{
  TokenKind k = cursorPeek(c)->kind;
  return k == TOK_NEWLINE || k == TOK_EOF;
}

int cursorAccept(TokenCursor *c, TokenKind kind)
{
  if (cursorPeek(c)->kind != kind) return 0;
  (void)cursorNext(c);
  return 1;
}

