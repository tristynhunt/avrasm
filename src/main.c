#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avrasm/assemble.h"
#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/report.h"
#include "avrasm/source.h"

static void usage(FILE *out)
{
  fputs("usage: avrasm [--image] [-I dir] [-D name[=value]] file.asm\n"
    "\n"
    "  --image          print the program memory image to stdout (the form the tests compare)\n"
    "  -I dir           look for .include files in dir too (after the directory of file.asm); repeatable\n"
    "  -D name[=value]  define a preprocessor name, like #define (value 1 if left out); repeatable\n"
    "\n"
    "exit status: 0 assembled, 1 the source has errors, 2 bad command line\n",
    out);
}

int main(int argc, char **argv)
{
  int showImage = 0;
  const char *path = NULL;
  const char *dirs[64];
  const char *defines[64];
  uint32_t dirCount = 0, defineCount = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(argv[i], "--image") == 0) {
      showImage = 1;
    } else if(strncmp(argv[i], "-I", 2) == 0 || strncmp(argv[i], "-D", 2) == 0) {
      int isDir = argv[i][1] == 'I';
      const char *value = argv[i] + 2;
      // both "-I dir" and "-Idir" work
      if (value[0] == '\0') value = (i + 1 < argc) ? argv[++i] : NULL;
      if (value == NULL) {
        fprintf(stderr, "avrasm: option '%s' needs a value\n", argv[i]);
        return 2;
      }
      if ((isDir ? dirCount : defineCount) == 64) {
        fprintf(stderr, "avrasm: too many %s options\n", isDir ? "-I" : "-D");
        return 2;
      }
      if (isDir) dirs[dirCount++] = value;
      else defines[defineCount++] = value;
    } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "avrasm: unknown option '%s'\n", argv[i]);
      usage(stderr);
      return 2;
    } else if (path != NULL) {
      fprintf(stderr, "avrasm: only one input file is supported (got '%s' and '%s')\n", path, argv[i]);
      return 2;
    } else {
      path = argv[i];
    }
  }

  if (path == NULL) {
    usage(stderr);
    return 2;
  }

  AvrCtx ctx;
  ctxInit(&ctx, avrDiskVfs());
  
  int ok = 0;
  Image flash;
  Span nowhere = { FILE_NONE, 0, 0 };
  FileId file = ctxLoad(&ctx, path, nowhere);
  if (file != FILE_NONE) {
    Image eeprom;
    imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
    imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
    AsmOptions opt = { dirs, dirCount, defines, defineCount };
    ok = assembleOpt(&ctx, file, &flash, &eeprom, &opt);
  }

  reportDiags(stderr, &ctx.sources, &ctx.diags);
  if (ok && showImage) fputs(imageToText(&flash, &ctx.arena), stdout);

  ctxFree(&ctx);
  return ok ? 0 : 1;
}