#include <stdio.h>
#include <string.h>

#include "avrasm/assemble.h"
#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/report.h"
#include "avrasm/source.h"

static void usage(FILE *out)
{
  fputs("usage: avrasm [--image] file.asm\n"
    "\n"
    "  --image   print the program memory image to stdout (the form the tests compare)\n"
    "\n"
    "exit status: 0 assembled, 1 the source has errors, 2 bad command line\n",
    out);
}

int main(int argc, char **argv)
{
  int showImage = 0;
  const char *path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(argv[i], "--image") == 0) {
      showImage = 1;
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
  Image image;
  Span nowhere = { FILE_NONE, 0, 0 };
  FileId file = ctxLoad(&ctx, path, nowhere);
  if (file != FILE_NONE) {
    imageInit(&image, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
    ok = assemble(&ctx, file, &image);
  }

  reportDiags(stderr, &ctx.sources, &ctx.diags);
  if (ok && showImage) fputs(imageToText(&image, &ctx.arena), stdout);

  ctxFree(&ctx);
  return ok ? 0 : 1;
}