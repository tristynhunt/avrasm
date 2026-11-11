#ifndef AVRASM_ASSEMBLE_H
#define AVRASM_ASSEMBLE_H

#include <stdint.h>

#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/source.h"

typedef struct AsmOptions
{
  const char *const *includeDirs;   // -I: where .include looks after the main dir
  uint32_t includeDirCount;
  const char *const *defines;       // -D: "NAME" or "NAME=text"
  uint32_t defineCount;
} AsmOptions;

// assemble the already loaded source file 'file' into 'image'.
// every message goes to ctx-diags. returns 1 if there was no error.
// the image is only meaningful if no errors occured.
int assemble(AvrCtx *ctx, FileId file, Image *image);

// the same, and the EEPROM image (what .eseg builds) goes to 'eeprom'.
// assemble() is this with an image it throws away.
int assembleAll(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom);

// assembleAll with options (which can be NULL)
int assembleOpt(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom, const AsmOptions *opt);

// the max size of the program mem image in bytes for now.
#define ASSEMBLE_DEFAULT_IMAGE_SIZE (1u << 18)
#define ASSEMBLE_DEFAULT_EEPROM_SIZE (1u << 16)

#endif