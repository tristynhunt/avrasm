#include "avrasm/ctx.h"
#include "avrasm/arena.h"
#include "avrasm/diag.h"
#include "avrasm/source.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static char *diskRead(void *user, const char *path, size_t *len)
{
  (void)user;
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  size_t cap = 4096, used = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }

  while (1) {
    size_t got = fread(buf + used, 1, cap - used, f);
    used += got;
    if (used < cap) break;
    char *bigger = realloc(buf, cap * 2);
    if (bigger == NULL) {
      free(buf);
      fclose(f);
      return NULL;
    }
    buf = bigger;
    cap *= 2;
  }
  int failed = ferror(f);
  fclose(f);
  if (failed) {
    free(buf);
    return NULL;
  }
  *len = used;
  return buf;
}

AvrVfs avrDiskVfs(void)
{
  AvrVfs v = { diskRead, NULL };
  return v;
}

void ctxInit(AvrCtx *c, AvrVfs vfs)
{
  arenaInit(&c->arena, 0);
  diagListInit(&c->diags, &c->arena);
  sourceMapInit(&c->sources, &c->arena);
  c->vfs = vfs.read != NULL ? vfs : avrDiskVfs();
}

void ctxFree(AvrCtx *c)
{
  arenaFree(&c->arena);
}

FileId ctxLoad(AvrCtx *c, const char *path, Span from)
{
  size_t len;
  char *text = c->vfs.read(c->vfs.user, path, &len);
  if (text == NULL) {
    diagEmit(&c->diags, DIAG_ERROR, from, "cannot open file '%s'", path);
    return FILE_NONE;
  }

  FileId id = sourceMapAdd(&c->sources, path, text, len);
  free(text);
  if (id == FILE_NONE) {
    diagEmit(&c->diags, DIAG_ERROR, from, "file '%s' is too large", path);
  }
  
  return id;
}

FileId ctxLoadQuiet(AvrCtx *c, const char *path)
{
  size_t len = 0;
  char *text = c->vfs.read(c->vfs.user, path, &len);
  if (text == NULL) return FILE_NONE;
  FileId id = sourceMapAdd(&c->sources, path, text, len);
  free(text);
  return id;
}