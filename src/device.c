#include "avrasm/device.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BIT(g) (1u << (g))

static char lowerAscii(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int sameWord(const char *text, size_t len, const char *word)
{
  if (strlen(word) != len) return 0;
  for (size_t i = 0; i < len; i++) {
    if (lowerAscii(text[i]) != lowerAscii(word[i])) return 0;
  }
  return 1;
}

void deviceInit(Device *d)
{
  memset(d, 0, sizeof *d);
  d->ramStart = 0x60;
}

int deviceSetName(Device *d, const char *name, size_t len)
{
  if (len >= sizeof d->name) return 0;
  memcpy(d->name, name, len);
  d->name[len] = '\0';
  return 1;
}

// ------- reading a pragma ----------------------------------------------

// The next word of text. skips spaces and tabs, returns its start and sets *wlen.
static const char *nextWord(const char *text, size_t len, size_t *pos, size_t *wlen)
{
  while (*pos < len && (text[*pos] == ' ' || text[*pos] == '\t')) (*pos)++;
  size_t start = *pos;
  while (*pos < len && text[*pos] != ' ' && text[*pos] != '\t') (*pos)++;
  *wlen = *pos - start;
  return text + start;
}

// a decimal or 0x number that fills text. returns 0 if it is not one.
static int parseNumber(const char *text, size_t len, uint32_t *out)
{
  uint32_t radix = 10;
  size_t i = 0;
  if (len > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    radix = 16;
    i = 2;
  }
  if (i == len) return 0;
  uint64_t v = 0;
  for (; i < len; i++) {
    char c = lowerAscii(text[i]);
    uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10) : 99;
    if (d >= radix) return 0;
    v = v * radix + d;
    if (v > 0xFFFFFFFFu) return 0;
  }
  *out = (uint32_t)v;
  return 1;
}

// a list of instructions as the device files write it: "movw:break:lpm rd,z:spm". sets one bit per
// group in *bits. returns 0 if a name in it is not one of these.
static int parseList(const char *text, size_t len, uint32_t *bits)
{
  static const char *const names[] = {"break", "movw", "spm", "lpm rd,z", "lpm rd,z+"};
  static const DeviceGroup groups[] = {DG_BREAK, DG_MOVW, DG_SPM, DG_LPMZ, DG_LPMZP};

  size_t start = 0;
  while (start <= len) {
    size_t end = start;
    while (end < len && text[end] != ':') end++;
    size_t a = start, b = end;
    while (a < b && text[a] == ' ') a++;
    while (b > a && text[b - 1] == ' ') b--;
    size_t i = 0;
    while (i < sizeof names / sizeof names[0] && !sameWord(text + a, b - a, names[i])) i++;
    if (i == sizeof names / sizeof names[0]) return 0;
    *bits |= BIT(groups[i]);
    start = end + 1;
  }
  return 1;
}

DevicePragma devicePragma(Device *d, const char *text, size_t len, const char **why)
{
  static const char *const cores[] = { "V0", "V0E", "V1", "V2", "V2E", "V3", "V4" };
  d->fromFile = 1;
  size_t pos = 0, l1, l2;
  const char *w1 = nextWord(text, len, &pos, &l1);
  const char *w2 = nextWord(text, len, &pos, &l2);
  while (pos < len && text[pos] == ' ') pos++;
  const char *rest = text + pos;
  size_t restLen = len - pos;
  uint32_t n;

  if (sameWord(w1, l1, "ADMIN") && sameWord(w2, l2, "PART_NAME")) {
    if (d->name[0] != '\0' && !sameWord(rest, restLen, d->name)) {
      *why = "this device file is for another device than .device named";
      return DEVICE_PRAGMA_ERROR;
    }
    if (!deviceSetName(d, rest, restLen)) {
      *why = "the part name is too long";
      return DEVICE_PRAGMA_ERROR;
    }
    return DEVICE_PRAGMA_OK;
  }
  if (sameWord(w1, l1, "CORE") && sameWord(w2, l2, "CORE_VERSION")) {
    if (sameWord(rest, restLen, "AVR8L_0")) {
      *why = "the reduced core of the ATtiny4 to ATtiny40 (only r16 to r31, another lds and sts) is not supported";
      return DEVICE_PRAGMA_ERROR;
    }
    for (size_t i = 0; i < sizeof cores / sizeof cores[0]; i++) {
      if (sameWord(rest, restLen, cores[i])) {
        strcpy(d->core, cores[i]);
        return DEVICE_PRAGMA_OK;
      }
    }
    *why = "this core version is not known";
    return DEVICE_PRAGMA_ERROR;
  }
  if (sameWord(w1, l1, "CORE") && (sameWord(w2, l2, "INSTRUCTIONS_NOT_SUPPORTED") || sameWord(w2, l2, "NEW_INSTRUCTIONS"))) {
    uint32_t bits = 0;
    if (!parseList(rest, restLen, &bits)) {
      *why = "an instruction in this list is not known, so the list is ignored";
      return DEVICE_PRAGMA_IGNORED;
    }
    if (sameWord(w2, l2, "NEW_INSTRUCTIONS")) d->added |= bits;
    else d->removed |= bits;
    return DEVICE_PRAGMA_OK;
  }
  if (sameWord(w1, l1, "MEMORY") && parseNumber(rest, restLen, &n)) {
    if (sameWord(w2, l2, "PROG_FLASH")) d->flashBytes = n;
    else if (sameWord(w2, l2, "EEPROM")) d->eepromBytes = n;
    else n = 0xFFFFFFFFu;
    if (n != 0xFFFFFFFFu) return DEVICE_PRAGMA_OK;
  }
  if (sameWord(w1, l1, "MEMORY") && sameWord(w2, l2, "INT_SRAM")) {
    size_t l3, l4;
    const char *w3 = nextWord(text, len, &pos, &l3);
    const char *w4 = nextWord(text, len, &pos, &l4);
    if (parseNumber(w4, l4, &n)) {
      if (sameWord(w3, l3, "SIZE")) {
        d->ramSize = n;
        return DEVICE_PRAGMA_OK;
      }
      if (sameWord(w3, l3, "START_ADDR")) {
        d->ramStart = n;
        return DEVICE_PRAGMA_OK;
      }
    }
  }
  *why = "this #pragma AVRPART line is not understood and is ignored";
  return DEVICE_PRAGMA_IGNORED;
}

DeviceGroup deviceGroup(uint16_t opcode)
{
  // the first entry whose bits match wins, so the narrow patterns come before the wide ones.
  static const struct {
    uint16_t mask, value;
    DeviceGroup group;
  } table[] = {
    { 0xFFFF, 0x95C8, DG_LPM },   { 0xFFFF, 0x95D8, DG_ELPM }, { 0xFFFF, 0x95E8, DG_SPM },
    { 0xFFFF, 0x95F8, DG_XMEGA }, { 0xFFFF, 0x9598, DG_BREAK },
    { 0xFEFF, 0x9409, DG_IJMP },   // ijmp, icall
    { 0xFEFF, 0x9419, DG_EIJMP },  // eijmp, eicall
    { 0xFF0F, 0x940B, DG_XMEGA },  // des
    { 0xFE0C, 0x940C, DG_JMP },    // jmp, call
    { 0xFC00, 0x9C00, DG_MUL },    // mul
    { 0xFF00, 0x0200, DG_MUL },    // muls
    { 0xFF00, 0x0300, DG_MUL },    // mulsu, fmul, fmuls, fmulsu
    { 0xFF00, 0x0100, DG_MOVW },
    { 0xFE00, 0x9600, DG_ADIW },   // adiw, sbiw
    { 0xFC0F, 0x9000, DG_LDS },    // lds, sts
    { 0xFC0F, 0x900F, DG_PUSH },   // pop, push
    { 0xFE0F, 0x9004, DG_LPMZ },
    { 0xFE0F, 0x9005, DG_LPMZP },
    { 0xFE0E, 0x9006, DG_ELPMZ },  // elpm Rd, Z and Rd, Z+
    { 0xFE0C, 0x9204, DG_XMEGA },  // xch, las, lac, lat
    { 0xFC0F, 0x8000, DG_LDZ },    // ld Rd, Z and st Z, Rr: ldd and std with Z and no displacement
    { 0xD000, 0x8000, DG_LDALL },  // the other ldd and std (ld Rd, Y is ldd Rd, Y+0)
    { 0xFC00, 0x9000, DG_LDALL },  // ld and st with X, Y+, -Y, Z+, -Z, X+, -X (the rest of 1001 00xx)
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
    if ((opcode & table[i].mask) == table[i].value) return table[i].group;
  }
  return DG_BASIC;
}

// each core version has the groups of the one before and more.
#define V0 BIT(DG_LDZ)
#define V0E (V0 | BIT(DG_LPM))
#define V1 (V0E | BIT(DG_LDALL) | BIT(DG_ADIW) | BIT(DG_PUSH) | BIT(DG_LDS) | BIT(DG_IJMP) | BIT(DG_BREAK))
#define V2 (V1 | BIT(DG_MOVW) | BIT(DG_LPMZ) | BIT(DG_SPM))
#define V2E (V2 | BIT(DG_MUL) | BIT(DG_LPMZP))

int deviceHas(const Device *d, DeviceGroup g)
{
  // the groups of each core version from the 78 device files of Microchip Studio 7 and
  // avrasm2.exe answers for each of them.
  static const struct {
    const char *name;
    uint32_t groups;
    int bigCore;  // V2 and later: jmp and call above 8 KB of flash
  } cores[] = {
    { "V0", V0, 0 },
    { "V0E", V0E, 0 },
    { "V1", V1, 0 },
    { "V2", V2, 1 },
    { "V2E", V2E, 1 },
    { "V3", V2E, 1 },
    { "V4", V2E & ~BIT(DG_SPM), 1 },
  };
  // if no device file nothing is checked
  if (d->core[0] == '\0') return 1;
  size_t i = 0;
  while (i < sizeof cores / sizeof cores[0] && strcmp(cores[i].name, d->core) != 0) i++;
  if (i == sizeof cores / sizeof cores[0]) return 0;
  uint32_t groups = cores[i].groups | BIT(DG_BASIC);
  if (cores[i].bigCore && d->flashBytes > 8 * 1024) groups |= BIT(DG_JMP);
  if (d->flashBytes > 64 * 1024) {
    groups |= BIT(DG_ELPM);
    if (strcmp(d->core, "V2E") == 0 || strcmp(d->core, "V3") == 0) groups |= BIT(DG_ELPMZ);
  }
  if (d->flashBytes > 128 * 1024) groups |= BIT(DG_EIJMP);
  groups = (groups & ~d->removed) | d->added;
  return (groups >> g) & 1u;
}

int64_t deviceRelative(uint32_t flashWords, int64_t pc, int64_t target)
{
  int64_t k = target - (pc + 1);
  if (flashWords == 0 || flashWords > 4096) return k;
  int64_t f = flashWords;
  k %= f;
  if (k < 0) k += f;
  if (k >= f / 2) k -= f;
  return k;
}