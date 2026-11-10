#ifndef AVRASM_SYMTAB_H
#define AVRASM_SYMTAB_H

#include <stddef.h>
#include <stdint.h>

#include "avrasm/arena.h"
#include "avrasm/source.h"

typedef enum SymKind
{
  SYM_LABEL,  // name at the current PC (a word address in code)
  SYM_EQU,    // .equ name = expr,
  SYM_SET     // .set name = expr: may get a new val later
} SymKind;

typedef struct Symbol
{
  const char *name; // nul-terminated copy
  uint32_t len;
  int64_t value;
  SymKind kind;
  Span defined;     // where the definition is
  struct Symbol *next; // the next symbol in the same bucket
} Symbol;

typedef struct Symtab
{
  Arena *arena;
  Symbol **buckets;
  uint32_t bucketCount;
  uint32_t count;
  int caseSensitive;  // default is false
} Symtab;

void symtabInit(Symtab *t, Arena *arena, int caseSensitive);

// the symbol with this name or NULL. 'name' is 'len' bytes and is not nul-terminated
const Symbol *symtabFind(const Symtab *t, const char *name, size_t len);

// adds a symbol. if the name is already taken nothing is added. returns NULL 
// and if 'existing' is not NULL, stores the symbol already there.
Symbol *symtabAdd(Symtab *t, const char *name, size_t len, SymKind kind, 
  int64_t value, Span defined, const Symbol **existing);

// defines a symbol as a .set symbol, or gives a new value to one that already exists.
// a name that is taken by a label or .equ symbol is left alone and returns NULL and
// stores the existing symbol.
Symbol *symtabAssign(Symtab *t, const char *name, size_t len, int64_t value,
  Span defined, const Symbol **existing);

// an ExprEnv.lookup that searches a Symtab
int symtabLookup(void *user, const char *name, size_t len, int64_t *value);

#endif