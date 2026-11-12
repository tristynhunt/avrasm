#ifndef AVRASM_OUTLINE_H
#define AVRASM_OUTLINE_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/source.h"

// what an editor wants to know about a program besides its bytes. every name it defines,
// with its value and where it is defined, and where each instruction and data directive put its
// words. the assembler fills one when AssembleOptions.outline is set and the language server answers
// hover, go to definition, document symbols and completion from it.

typedef enum OutlineKind {
  OUTLINE_LABEL,  // value: the address (a word address in .cseg, a byte address in .dseg and .eseg)
  OUTLINE_EQU,    // value: its value
  OUTLINE_SET,    // value: the value of the last .set, defined: the first .set
  OUTLINE_DEF,    // value: the register number of the first .def of the name
  OUTLINE_MACRO,  // value: how many arguments the body uses (@0 up to @value-1)
  OUTLINE_DEFINE  // a #define name, value 0
} OutlineKind;

typedef struct OutlineName {
  OutlineKind kind;
  const char *name;  // NUL-terminated, in the arena
  uint32_t len;
  int64_t value;
  char seg;          // a label's segment: 'c', 'd' or 'e' (.cseg .dseg .eseg). 0 for the others
  Span defined;      // file FILE_NONE if it came from the command line (-D)
} OutlineName;

// one instruction or data directive that put words into program memory.
typedef struct OutlineCode {
  Span head;       // the mnemonic, or the .db .dw .dd .dq
  int64_t pc;      // the word address of its first word
  uint32_t words;  // how many words it takes
  int isData;      // 1 for .db .dw .dd .dq, 0 for an instruction
  Span callSite;   // the macro call it came out of, file FILE_NONE if not from a macro
  uint32_t scope;  // 0 or the macro expansion it is in. the lines of one call share it
} OutlineCode;

typedef struct Outline {
  Arena *arena;
  OutlineName *names;  // in the order they were added
  uint32_t nameCount;
  uint32_t nameCap;
  OutlineCode *code;   // in the order of the program
  uint32_t codeCount;
  uint32_t codeCap;
} Outline;

void outlineInit(Outline *o, Arena *arena);

// adds a name. a .set or .def name that is there already is not added again and the first wins.
void outlineAddName(Outline *o, OutlineKind kind, const char *name, size_t len,
  int64_t value, char seg, Span defined);

void outlineAddCode(Outline *o, const OutlineCode *code);

// the first name called name ignoring case. NULL if there is none.
const OutlineName *outlineFind(const Outline *o, const char *name, size_t len);

#endif