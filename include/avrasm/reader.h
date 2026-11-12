#ifndef AVRASM_READER_H
#define AVRASM_READER_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/ctx.h"
#include "avrasm/outline.h"
#include "avrasm/tokline.h"

// the reader sits between the files and the assembler. the assembler asks it for the next line
// to assemble and the reader decides what that is:
//
//   - the lines of the main file, then of each .include file, in place
//   - the preprocessor (#define #undef #if #ifdef #ifndef #elif #else #endif #include #error
//     #warning #message #pragma), which replaces names and drops what is not wanted
//   - the conditionals (.if .ifdef .ifndef .elif .else .endif), which look at the symbols the
//     assembler has defined so far so the reader asks it (ReaderHooks)
//   - macros (.macro .endm, and the calls): a call comes out as the lines of the body, with
//     @0 to @9 replaced by the arguments
//   - .message .warning .error .exit
//
// none of those directives come out of readerNext. every other line comes out as it is.
typedef struct ReaderHooks
{
  // evaluates the expression at the cursor (what follows .if or .elif) and stores the value.
  // reports all errors and returns 0 after one.
  int (*eval)(void *user, TokenCursor *cur, int64_t *value);
  // checks if 'name' is a label, an .equ or .set symbol, or a .def alias, as far
  // as the lines handed out so far have defined them.
  int (*isDefined)(void *user, const char *name, size_t len);
  // checks if 'name' can be the name of a macro. reports errors itself.
  int (*nameOk)(void *user, const Token *name);
  // a #pragma AVRPART line of a device file. 'text' is what follows AVRPART on the line
  // ("MEMORY PROG_FLASH 32768"), 'at' its span. may be NULL.
  void (*pragma)(void *user, Span at, const char *text, size_t len);
  void *user;
} ReaderHooks;

typedef struct ReaderLine
{
  TokenLine line;   // what to assemble
  Span callSite;    // the macro call this line cam out of. FILE_NONE if no macro.
  uint32_t scope;   // 0: the file itself, n > 0: the n-th macro expansion (its labels are private)
} ReaderLine;

typedef struct Reader Reader;

// the reader live in the ctx->arena. 'main' is the already loaded top-level file.
// .include "name" looks for 'name' next to it (not next to the file that includes),
// then in each of the 'dirCount' directories of 'includeDirs'.
Reader *readerCreate(AvrCtx *ctx, const ReaderHooks *hooks, FileId main,
  const char *const *includeDirs, uint32_t dirCount);

// defines a preprocessor name as the command line option -D.
// does "NAME" is "NAME" 1, and "NAME=text" is Name text. returns 0 after an error. 
int readerDefine(Reader *r, const char *spec);

// hands out the next line. returns 0 when everything is read.
int readerNext(Reader *r, ReaderLine *out);

// checks if 'name' is the name of a macro
int readerHasMacro(const Reader *r, const char *name, size_t len);

// adds every macro and every #define name the reader knows now to the outline.
void readerOutline(const Reader *r, Outline *o);

#endif
