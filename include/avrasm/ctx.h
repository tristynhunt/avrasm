#ifndef AVRASM_CTX_H
#define AVRASM_CTX_H

#include <stddef.h>

#include "avrasm/arena.h"
#include "avrasm/diag.h"
#include "avrasm/source.h"

// how the assembler reads files
typedef struct AvrVfs
{
  // reads all of path, returns a malloc'd buffer (which the caller frees)
  // stores its size in *len, or returns NULL if the file cannot be read
  char *(*read)(void *user, const char *path, size_t *len);
  void *user;
} AvrVfs;

// a vfs that reads from the real file system
AvrVfs avrDiskVfs(void);

// everything one assembly run owns.
// AvrCtx must not be copied, the `arena` is borrowed by pointer
typedef struct AvrCtx
{
  Arena arena;
  SourceMap sources;
  DiagList diags;
  AvrVfs vfs;
} AvrCtx;

// a vfs with a NULL read function means "use the disk"
void ctxInit(AvrCtx *c, AvrVfs vfs);

// free everything the context owns
void ctxFree(AvrCtx *c);

// reads `path` through the vfs and adds to source map.
// each call also adds a new file, even paths loaded before.
// if the file cannot be read or is too big, an error is emitted
FileId ctxLoad(AvrCtx *c, const char *path, Span from);

// the same as ctxLoad, for a file that may not be there.
// no diagnostic are given. returns FILE_NONE if 'path' cannot be read (or is too big)
// .include uses it to try one place after another
FileId ctxLoadQuiet(AvrCtx *c, const char *path);

#endif