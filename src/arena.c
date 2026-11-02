#include "avrasm/arena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// one chunk of memory from the system allocator
struct ArenaBlock
{
  ArenaBlock *next;
  size_t cap;
  size_t used;
  char data[];
};

void arenaInit(Arena *a, size_t blockSize)
{
  a->head = NULL;
  a->blockSize = blockSize ? blockSize : 64 * 1024;
}

// rounds up x to nearest multiple of align (power of 2)
static uintptr_t alignUp(uintptr_t x, size_t align)
{
  return (x + (align - 1)) & ~(align - 1);
}

void *arenaAlloc(Arena *a, size_t size, size_t align)
{
  ArenaBlock *b = a->head;
  if (b != NULL) {
    uintptr_t cursor = (uintptr_t)(b->data + b->used);
    uintptr_t aligned = alignUp(cursor, align);
    size_t pad = aligned - cursor;
    if (pad + size <= b->cap - b->used) {
      b->used += pad + size;
      return (void *)aligned;
    }
  }
  size_t need = size + align;
  size_t newSize = need > a->blockSize
    ? ((need + a->blockSize - 1) / a->blockSize) * a->blockSize
    : a->blockSize;
  ArenaBlock *block = calloc(1, sizeof *block + newSize);
  if (block == NULL) {
    fputs("avrasm: out of memory\n", stderr);
    abort();
  }
  block->cap = newSize;
  block->next = a->head;
  a->head = block;

  uintptr_t start = alignUp((uintptr_t)block->data, align);
  block->used = (start - (uintptr_t)block->data) + size;
  return (void *)start;
}

char *arenaStrdup(Arena *a, const char *s)
{
  return arenaStrndup(a, s, strlen(s));
}

char *arenaStrndup(Arena *a, const char *s, size_t n)
{
  const char *nul = memchr(s, 0, n);
  size_t len = nul != NULL ? (size_t)(nul - s) : n;
  char *cpy = arenaAlloc(a, len + 1, 1);
  memcpy(cpy, s, len);
  cpy[len] = '\0';
  return cpy;
}

void arenaFree(Arena *a)
{
  ArenaBlock *b = a->head;
  while (b != NULL) {
    ArenaBlock *next = b->next;
    free(b);
    b = next;
  }
  a->head = NULL;
}