#include "avrasm/outline.h"
#include "avrasm/arena.h"

#include <stdint.h>
#include <string.h>

static char lowerAscii(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int sameNoCase(const char *a, size_t aLen, const char *b, size_t bLen)
{
  if (aLen != bLen) return 0;
  for (size_t i = 0; i < aLen; i++) {
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return 0;
  }
  return 1;
}

static void *grow(Arena *arena, void *items, uint32_t count, uint32_t *cap, size_t size, size_t align)
{
  if (count < *cap) return items;
  uint32_t newCap = *cap ? *cap * 2 : 64;
  void *bigger = arenaAlloc(arena, (size_t)newCap * size, align);
  if (count > 0) memcpy(bigger, items, (size_t)count * size);
  *cap = newCap;
  return bigger;
}

void outlineInit(Outline *o, Arena *arena)
{
  memset(o, 0, sizeof *o);
  o->arena = arena;
}

void outlineAddName(Outline *o, OutlineKind kind, const char *name, size_t len,
  int64_t value, char seg, Span defined)
{
  if (kind == OUTLINE_DEF || kind == OUTLINE_SET) {
    for (uint32_t i = 0; i < o->nameCount; i++) {
      if (o->names[i].kind == kind && sameNoCase(o->names[i].name, o->names[i].len, name, len)) return;
    }
  }
  o->names = grow(o->arena, o->names, o->nameCount, &o->nameCap, sizeof *o->names, _Alignof(OutlineName));
  OutlineName *n = &o->names[o->nameCount++];
  n->kind = kind;
  n->name = arenaStrndup(o->arena, name, len);
  n->len = (uint32_t)len;
  n->value = value;
  n->seg = seg;
  n->defined = defined;
}

void outlineAddCode(Outline *o, const OutlineCode *code)
{
  o->code = grow(o->arena, o->code, o->codeCount, &o->codeCap, sizeof *o->code, _Alignof(OutlineCode));
  o->code[o->codeCount++] = *code;
}

const OutlineName *outlineFind(const Outline *o, const char *name, size_t len)
{
  for (uint32_t i = 0; i < o->nameCount; i++) {
    if (sameNoCase(name, len, o->names[i].name, o->names[i].len)) return &o->names[i];
  }
  return NULL;
}