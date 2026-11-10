#ifndef AVRASM_ASSEMBLE_H
#define AVRASM_ASSEMBLE_H

#include "avrasm/ctx.h"
#include "avrasm/image.h"
#include "avrasm/source.h"

// assemble the already loaded source file 'file' into 'image'.
// every message goes to ctx-diags. returns 1 if there was no error.
// the image is only meaningful if no errors occured.
int assemble(AvrCtx *ctx, FileId file, Image *image);

// the same, and the EEPROM image (what .eseg builds) goes to 'eeprom'.
// assemble() is this with an image it throws away.
int assembleAll(AvrCtx *ctx, FileId file, Image *flash, Image *eeprom);

// the size of the program mem image in bytes for now.
// this will be set later with device directives
#define ASSEMBLE_DEFAULT_IMAGE_SIZE (1u << 17)
#define ASSEMBLE_DEFAULT_EEPROM_SIZE (1u << 16)

#endif