#include "avrasm/symtab.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "avrasm/arena.h"
#include "avrasm/source.h"

#define INITIAL_BUCKETS 64

static unsigned char fold(unsigned char c, int caseSensitive)
{
  if (!caseSensitive && c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
  return c;
}

// FNV-1a hash over the bytes after case folding.
static uint32_t hashName(const char *s, size_t len, int caseSensitive)
{
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= fold((unsigned char)s[i], caseSensitive);
    h *= 16777619u;
  }
  return h;
}

// returns 1 if a symbol matches 'name' while accounting for case sensitivity
int sameName(const Symbol *sym, const char *name, size_t len, int caseSensitive)
{
  if (sym->len != len) return 0;
  for (size_t i = 0; i < len; i++) {
    if (fold((unsigned char)sym->name[i], caseSensitive) !=
        fold((unsigned char)name[i], caseSensitive))
        return 0;
  }
  return 1;
}

// 
const Symbol *symtabFind(const Symtab *t, const char *name, size_t len)
{
  uint32_t b = hashName(name, len, t->caseSensitive) & (t->bucketCount - 1);
  Symbol *s = t->buckets[b];
  while (s != NULL) {
    if (sameName(s, name, len, t->caseSensitive)) return s;
    s = s->next;
  }
  return NULL;
}

// double the amount of buckets and rehashes symbols.
// the symbols themselves are not reallocated and only their pointers change.
static void grow(Symtab *t)
{
  uint32_t newCount = 2 * t->bucketCount;
  Symbol **newBuckets = arenaAlloc(t->arena, newCount * sizeof *newBuckets, _Alignof(Symbol *));
  for (uint32_t i = 0; i < t->bucketCount; i++) {
    Symbol *s = t->buckets[i];
    while (s != NULL) {
      Symbol *next = s->next;
      uint32_t b = hashName(s->name, s->len, t->caseSensitive) & (newCount - 1);
      s->next = newBuckets[b];
      newBuckets[b] = s;
      s = next;
    }
  }
  t->buckets = newBuckets;
  t->bucketCount = newCount;
}

void symtabInit(Symtab *t, Arena *arena, int caseSensitive)
{
  t->buckets = arenaAlloc(arena, INITIAL_BUCKETS * sizeof *t->buckets, _Alignof(Symbol *));
  t->arena = arena;
  t->bucketCount = INITIAL_BUCKETS;
  t->count = 0;
  t->caseSensitive = caseSensitive;
}

Symbol *symtabAdd(Symtab *t, const char *name, size_t len, SymKind kind, 
  int64_t value, Span defined, const Symbol **existing)
{
  // if it already exists, set it and return null
  const Symbol *found = symtabFind(t, name, len);
  if (found != NULL) {
    if (existing != NULL) *existing = found;
    return NULL;
  }
  
  if (t->count >= t->bucketCount) grow(t);
  Symbol *sym = arenaAlloc(t->arena, sizeof(Symbol), _Alignof(Symbol));
  sym->name = arenaStrndup(t->arena, name, len);
  sym->len = len;
  sym->value = value;
  sym->kind = kind;
  sym->defined = defined;

  uint32_t b = hashName(name, len, t->caseSensitive) & (t->bucketCount - 1);
  sym->next = t->buckets[b];
  t->buckets[b] = sym;
  t->count++;
  return sym;
}

Symbol *symtabAssign(Symtab *t, const char *name, size_t len, int64_t value, 
  Span defined, const Symbol **existing)
{
  const Symbol *found = symtabFind(t, name, len);
  if (found == NULL) return symtabAdd(t, name, len, SYM_SET, value, defined, NULL);
  if (found->kind != SYM_SET) {
    if (existing != NULL) *existing = found;
    return NULL;
  }
  Symbol *s = (Symbol *)found;
  s->value = value;
  s->defined = defined;
  return s;
}

int symtabLookup(void *user, const char *name, size_t len, int64_t *value)
{
  const Symbol *s = symtabFind((const Symtab *)user, name, len);
  if (s == NULL) return 0;
  *value = s->value;
  return 1;
}