#ifndef AVRASM_DEVICE_H
#define AVRASM_DEVICE_H

#include <stddef.h>
#include <stdint.h>

// what the program knows about its device. a device file (like m324Adef.inc) says it in
// #pragma AVRPART lines, which the reader hands to devicePragma. '.device NAME' only gives a name.
// without a device file nothing is known and nothing is checked
typedef struct Device
{
  char name[32];        // "ATmega324A". "" until .device or the device file names it
  int fromFile;         // 1 once a #pragma AVRPART line was read, the fields below are the devices
  char core[12];        // CORE_VERSION: V0, V0E, V1, V2, V2E, V3 or V4
  uint32_t flashBytes;  // PROG_FLASH
  uint32_t eepromBytes; // EEPROM
  uint32_t ramStart;    // INT_SRAM START_ADDR (0x60 until a device file says otherwise)
  uint32_t ramSize;     // INT_SRAM SIZE (0: no RAM like the AT90S1200)
  uint32_t removed;     // INSTRUCTIONS_NOT_SUPPORTED, one bit per DeviceGroup
  uint32_t added;       // NEW_INSTRUCTIONS, one bit per DeviceGroup
} Device;

// the groups of instructions that devices have or lack.
// each opcode belongs to exactly one.
typedef enum DeviceGroup {
  DG_BASIC,  // what every AVR has
  DG_LDZ,    // ld Rd, Z and st Z, Rr: the only memory access of the oldest core
  DG_LPM,    // lpm with no operands (r0 from Z)
  DG_LDALL,  // every other ld, st, ldd and std
  DG_ADIW,   // adiw, sbiw
  DG_PUSH,   // push, pop
  DG_LDS,    // lds, sts
  DG_IJMP,   // ijmp, icall
  DG_BREAK,  // break
  DG_MOVW,   // movw
  DG_LPMZ,   // lpm Rd, Z
  DG_LPMZP,  // lpm Rd, Z+
  DG_SPM,    // spm
  DG_MUL,    // mul, muls, mulsu, fmul, fmuls, fmulsu
  DG_JMP,    // jmp, call
  DG_ELPM,   // elpm with no operands
  DG_ELPMZ,  // elpm Rd, Z and elpm Rd, Z+
  DG_EIJMP,  // eijmp, eicall
  DG_XMEGA   // spm Z+, des, xch, las, lac, lat: no ATmega or ATtiny has them
} DeviceGroup;

typedef enum DevicePragma {
  DEVICE_PRAGMA_OK,
  DEVICE_PRAGMA_IGNORED,  // not understood
  DEVICE_PRAGMA_ERROR     // understood and wrong or not supported
} DevicePragma;

// nothing known: no name, no limits, every instruction allowed, .dseg at 0x60.
void deviceInit(Device *d);

// sets the name (from .device or PART_NAME). returns 0 if it does not fit in Device.name.
int deviceSetName(Device *d, const char *name, size_t len);

// one #pragma AVRPART line given as the text after AVRPART ("MEMORY PROG_FLASH 32768").
// sets fromFile. on IGNORED or ERROR *why is a message for the line.
DevicePragma devicePragma(Device *d, const char *text, size_t len, const char **why);

// the group of the instruction whose first word is 'opcode'.
DeviceGroup deviceGroup(uint16_t opcode);

// checks if the device has that group of instructions. always 1 when no device file was read
int deviceHas(const Device *d, DeviceGroup g);

// the distance that rjmp and rcall encode for a jump from the word at 'pc' to the word 'target':
// target - (pc + 1). on a device of at most 4096 words ('flashWords' 0 when not known) the
// program counter wraps around, and encodes the distance modulo the flash size in
// -flashWords/2 .. flashWords/2 - 1. 'target' must lie inside the flash.
int64_t deviceRelative(uint32_t flashWords, int64_t pc, int64_t target);

#endif