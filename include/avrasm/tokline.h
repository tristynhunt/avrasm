#ifndef AVRASM_TOKLINE_H
#define AVRASM_TOKLINE_H

#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/lexer.h"

// because asm is line oriented the lexer output is split into token lines
// all further parts will use these lines of tokens. each line will 
// always end with either TOK_EOF or TOK_NEWLINE.
typedef struct TokenLine
{
  Token *tokens;  // tokens in source order
  uint32_t count; // always >= 1
} TokenLine;

// lexes token up to and including the next TOK_NEWLINE or TOK_EOF.
// TOK_ERROR tokens stay in the line and are reported by the lexer.
// a blank line is a single TOK_NEWLINE. 
// at the end of the file you get TOK_EOF again and again.
TokenLine lexLine(Lexer *lx, Arena *arena);

// a position in a line, reading can never go past the last token.
typedef struct TokenCursor
{
  const TokenLine *line;
  uint32_t pos;
} TokenCursor;

void cursorInit(TokenCursor *c, const TokenLine *line);

// the current token, never is NULL
const Token *cursorPeek(const TokenCursor *c);

// the token n places ahead (0 is the current one).
// returns the last token in the line (EOF or NEWLINE) if n > count
const Token *cursorPeekAt(const TokenCursor *c, uint32_t n);

// returns the current token and moves on to the next one.
// this will never move past the last token.
const Token *cursorNext(TokenCursor *c);

// true if current token is TOK_NEWLINE or TOK_EOF
int cursorAtEnd(const TokenCursor *c);

// if the current token has this kind then consume and return 1.
// otherwise consume it and return 0.
int cursorAccept(TokenCursor *c, TokenKind kind);

#endif