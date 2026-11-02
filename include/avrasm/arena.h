#ifndef AVRASM_ARENA_H
#define AVRASM_ARENA_H

#include <stddef.h>

// a bump allocator which can allocate freely and free everything at once

// this is defined in arena.c
typedef struct ArenaBlock ArenaBlock;

typedef struct Arena
{
  ArenaBlock *head;
  size_t blockSize;
} Arena;

// blockSize 0 selects the default (64KiB)
void arenaInit(Arena *a, size_t blockSize);

// returns 0 filled memory to "align" (a power of 2 of at least 2)
// a request too big for one block gets a block of its own
// out of memoery is a fatal error, prints a message and abort()
void *arenaAlloc(Arena *a, size_t size, size_t align);

char *arenaStrdup(Arena *a, const char *s);

// copies at most n bytes of s, stopping early at a NUL, and always
// NUL-terminates the copy
char *arenaStrndup(Arena *a, const char *s, size_t n);

// release every block
void arenaFree(Arena *a);

#endif