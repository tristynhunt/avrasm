#ifndef AVRASM_LEXER_H
#define AVRASM_LEXER_H

#include <stdint.h>

#include "avrasm/diag.h"
#include "avrasm/source.h"

typedef enum TokenKind {
  TOK_EOF,       // end of file; returned again and again once reached
  TOK_ERROR,     // a malformed lexeme; a diagnostic has already been emitted for it
  TOK_NEWLINE,   // one line terminator: \n, \r\n or a lone \r
  TOK_IDENT,     // names, registers, mnemonics and directives alike: loop, r16, .db, @0
  TOK_NUMBER,    // integer literal; its value is in Token.value
  TOK_FLOAT,     // 1.5; its value is in Token.fvalue
  TOK_CHAR,      // 'a'; its code is in Token.value
  TOK_STRING,    // "text"; the span includes both quotes
  TOK_LPAREN, TOK_RPAREN,
  TOK_COMMA, TOK_COLON,
  TOK_QUESTION,
  TOK_EQ, TOK_HASH,
  TOK_PLUS, TOK_MINUS,
  TOK_STAR, TOK_SLASH,
  TOK_PERCENT,
  TOK_SHL, TOK_SHR, // << >>
  TOK_LT, TOK_LE, TOK_GT, TOK_GE,
  TOK_EQEQ, TOK_NE, // ==, !=
  TOK_AMP, TOK_CARET, TOK_PIPE, // |
  TOK_ANDAND, TOK_OROR, // &&, ||
  TOK_TILDE, TOK_BANG,
  TOK_PLUSPLUS,  // ++ and -- are tokens of their own (AVRASM2 reserves them)
  TOK_MINUSMINUS
} TokenKind;

typedef struct Token {
  TokenKind kind;
  Span span;       // the bytes of the lexeme, for TOK_EOF an empty span at the end
  uint64_t value;  // TOK_NUMBER and TOK_CHAR
  double fvalue;   // TOK_FLOAT
} Token;

typedef struct Lexer {
  const SourceFile *file;  // NULL if the id was not in the map: the lexer then sees an empty file
  FileId id;
  DiagList *diags;
  uint32_t pos;            // offset of the next unread byte
} Lexer;

void lexerInit(Lexer *lx, const SourceMap *map, FileId id, DiagList *diags);

// returns the next token. spaces, tabs, comments and line continuations (a backslash
// right before a line terminator) are skipped and produce no token. (a block comment
// with a line break inside it comes back as one TOK_NEWLINE, spanning the
// whole comment.) a malformed lexeme emits one error diagnostic and comes back as
// TOK_ERROR, so the caller can carry on without reporting it a second time. after
// TOK_EOF it keeps returning TOK_EOF
Token lexerNext(Lexer *lx);

// "IDENT", "COMMA", "EOF", ... the enum name without its TOK_ prefix.
// "UNKNOWN" for a value that is not a TokenKind.
const char *tokenKindName(TokenKind kind);

#endif