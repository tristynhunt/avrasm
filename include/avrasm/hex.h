#ifndef AVRASM_HEX_H
#define AVRASM_HEX_H

#include "avrasm/arena.h"
#include "avrasm/image.h"

// the intel HEX text avrasm2 writes for an image (-fI) byte for byte:
//   - the blocks in the order the program wrote them (Image.blocks), each cut into data records of
//     at most 16 bytes counted from the start of the block, and cut again at every 64 KB boundary
//   - with 'segments' (the flash file and the EEPROM file has none), an extended segment address
//     record before the first data record and before every record whose 64 KB segment differs from
//     the one before: ":02000002SSSS" with SSSS = 0x1000 for each 64 KB
//   - a data record is ":LLAAAA00DD..CC": LL bytes at address AAAA within its segment, and CC the
//     two's complement of the sum of all the bytes before it
//   - the end-of-file record ":00000001FF". Every line ends in "\r\n" and the hex digits are upper case.
// allocated from the arena. an image with nothing written is just the end-of-file record.
char *hexText(const Image *im, int segments, Arena *arena);

#endif