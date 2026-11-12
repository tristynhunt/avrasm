#include "avrasm/lint.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avrasm/assemble.h"
#include "avrasm/ctx.h"
#include "avrasm/hex.h"
#include "avrasm/image.h"
#include "avrasm/report.h"
#include "avrasm/source.h"

#define AVRASM_VERSION "1.1"

static void usage(FILE *out)
{
  fputs("usage: avrasm [options] file.asm\n"
    "\n"
    "  -o file          write the program memory as Intel HEX to file (default: file.hex next to file.asm)\n"
    "  -e file          write the EEPROM as Intel HEX to file, when the program has EEPROM data\n"
    "  -fI              Intel HEX, the only output format (accepted, as AVRASM2 takes it)\n"
    "  -I dir           look for .include files in dir too (after the directory of file.asm); repeatable\n"
    "  -D name[=value]  define a preprocessor name, like #define (value 1 if left out); repeatable\n"
    "  --image          print the program memory image to stdout (the form the tests compare); with it,\n"
    "                   a .hex file is only written when -o asks for one\n"
    "  --no-lint        leave out the warnings about code that assembles but is probably wrong\n"
    "  -h, --help       this text\n"
    "  --version        the version\n"
    "\n"
    "exit status: 0 assembled (perhaps with warnings), 1 the source has errors,\n"
    "             2 a bad command line or a file that cannot be written\n",
    out);
}

// the value of an option that takes one: "-ofile" or "-o file". NULL (after a message) if there is none
static const char *optionValue(int argc, char **argv, int *i)
{
  const char *value = argv[*i] + 2;
  if (value[0] == '\0') value = (*i + 1 < argc) ? argv[++*i] : NULL;
  if (value == NULL) fprintf(stderr, "avrasm: option '%s' needs a value\n", argv[*i]);
  return value;
}

// file.asm -> file.hex (the extension of the last path component is replaced, or .hex is added).
static char *defaultHexName(const char *path)
{
  const char *slash = strrchr(path, '/');
  const char *back = strrchr(path, '\\');  // windows paths
  if (back != NULL && (slash == NULL || back > slash)) slash = back;
  const char *dot = strrchr(path, '.');
  size_t stem = (dot != NULL && (slash == NULL || dot > slash)) ? (size_t)(dot - path) : strlen(path);
  char *name = malloc(stem + 5);
  if (name == NULL) return NULL;
  memcpy(name, path, stem);
  strcpy(name + stem, ".hex");
  return name;
}

// removes 'path' if it is there. returns 1 if a file was removed
static int removeOld(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) return 0;
  fclose(f);
  return remove(path) == 0;
}

static int writeText(const char *path, const char *text)
{
  // binary, the "\r\n" line ends stay as they are
  FILE *f = fopen(path, "wb");
  int ok = f != NULL && fputs(text, f) >= 0;
  if (f != NULL && fclose(f) != 0) ok = 0;
  if (!ok) fprintf(stderr, "avrasm: cannot write '%s'\n", path);
  return ok;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
  // --image writes "\n" not "\r\n"
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  int showImage = 0;
  int lint = 1;
  const char *path = NULL;
  const char *flashPath = NULL;
  const char *eepromPath = NULL;
  const char *dirs[64];
  const char *defines[64];
  uint32_t dirCount = 0, defineCount = 0;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout);
      return 0;
    } else if (strcmp(arg, "--image") == 0) {
      showImage = 1;
    } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
      puts("avrasm " AVRASM_VERSION);
      return 0;
    } else if (strcmp(arg, "--no-lint") == 0) {
      lint = 0;
    } else if (arg[0] == '-' && (arg[1] == 'I' || arg[1] == 'D' || arg[1] == 'o' || arg[1] == 'e' || arg[1] == 'f')) {
      char which = arg[1];
      const char *value = optionValue(argc, argv, &i);
      if (value == NULL) return 2;
      if (which == 'o') {
        flashPath = value;
      } else if (which == 'e') {
        eepromPath = value;
      } else if (which == 'f') {
        if (strcmp(value, "I") != 0) {
          fprintf(stderr, "avrasm: only Intel HEX (-fI) is written; -f%s is not supported\n", value);
          return 2;
        }
      } else if ((which == 'I' ? dirCount : defineCount) == 64) {
        fprintf(stderr, "avrasm: too many -%c options\n", which);
        return 2;
      } else if (which == 'I') {
        dirs[dirCount++] = value;
      } else {
        defines[defineCount++] = value;
      }
    } else if (arg[0] == '-' && arg[1] != '\0') {
      fprintf(stderr, "avrasm: unknown option '%s'\n", arg);
      usage(stderr);
      return 2;
    } else if (path != NULL) {
      fprintf(stderr, "avrasm: only one input file is supported (got '%s' and '%s')\n", path, arg);
      return 2;
    } else {
      path = arg;
    }
  }

  if (path == NULL) {
    usage(stderr);
    return 2;
  }

  char *defaultName = NULL;
  if (flashPath == NULL && !showImage) flashPath = defaultName = defaultHexName(path);
  if ((flashPath != NULL && strcmp(flashPath, path) == 0) || (eepromPath != NULL && strcmp(eepromPath, path) == 0)) {
    fprintf(stderr, "avrasm: an output file cannot be the input file '%s'\n", path);
    free(defaultName);
    return 2;
  }

  AvrCtx ctx;
  ctxInit(&ctx, avrDiskVfs());
  
  int result = 0;
  Image flash, eeprom;
  imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  Span nowhere = { FILE_NONE, 0, 0 };
  FileId file = ctxLoad(&ctx, path, nowhere);
  if (file != FILE_NONE) {
    Outline outline;
    outlineInit(&outline, &ctx.arena);
    AsmOptions opt = { dirs, dirCount, defines, defineCount, &outline };
    result = assembleOpt(&ctx, file, &flash, &eeprom, &opt);
    if (result == 1 && lint) lintProgram(&ctx.diags, &outline, &flash);
  }
  reportDiags(stderr, &ctx.sources, &ctx.diags);

  // no output file may outlive a run that does not write it
  int status = 0;
  if (result == ASSEMBLE_EMPTY) {
    fprintf(stderr, "%s: warning: the program puts nothing into memory, so no file is written\n", path);
  }
  if (result != 1) {
    const char *why = result == 0 
      ? "nothing is written when there are errors" 
      : "the program puts nothing into memory"; 
    const char *outputs[2] = { flashPath, eepromPath };
    for (int i = 0; i < 2; i++) {
      if (outputs[i] != NULL && removeOld(outputs[i])) 
        fprintf(stderr, "avrasm: removed the old '%s' (%s)\n", outputs[i], why);
    }
    status = result == 0 ? 1 : 0;
  } else {
    if (flashPath != NULL && !writeText(flashPath, hexText(&flash, 1, &ctx.arena))) status = 2;
    if (eepromPath != NULL && eeprom.blockCount > 0) {
      if (!writeText(eepromPath, hexText(&eeprom, 0, &ctx.arena))) status = 2;
    } else if (eepromPath != NULL) {
      int removed = removeOld(eepromPath);
      fprintf(stderr, "avrasm: no EEPROM data, so '%s' is not written%s\n", eepromPath, removed ? " (the old one is removed)" : "");
    }
    if (showImage) fputs(imageToText(&flash, &ctx.arena), stdout);
  }

  ctxFree(&ctx);
  free(defaultName);
  return status;
}