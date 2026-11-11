#include "avrasm/assemble.h"
#include "avrasm/device.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT(g) (1u << (g))

static DevicePragma pragma(Device *d, const char *text)
{
  const char *why = NULL;
  DevicePragma r = devicePragma(d, text, strlen(text), &why);
  CHECK((r == DEVICE_PRAGMA_OK) == (why == NULL));
  return r;
}

static void testPragmas(void)
{
  Device d;
  deviceInit(&d);
  CHECK_INT(d.fromFile, 0);
  CHECK_INT(d.ramStart, 0x60);
  CHECK_INT(pragma(&d, "ADMIN PART_NAME ATmega324A"), DEVICE_PRAGMA_OK);
  CHECK_INT(pragma(&d, "CORE CORE_VERSION V2E"), DEVICE_PRAGMA_OK);
  CHECK_INT(pragma(&d, "MEMORY PROG_FLASH 32768"), DEVICE_PRAGMA_OK);
  CHECK_INT(pragma(&d, "MEMORY EEPROM 1024"), DEVICE_PRAGMA_OK);
  CHECK_INT(pragma(&d, "MEMORY INT_SRAM SIZE 2048"), DEVICE_PRAGMA_OK);
  CHECK_INT(pragma(&d, "MEMORY INT_SRAM START_ADDR 0x100"), DEVICE_PRAGMA_OK);
  CHECK_INT(d.fromFile, 1);
  CHECK_STR(d.name, "ATmega324A");
  CHECK_STR(d.core, "V2E");
  CHECK_INT(d.flashBytes, 32768);
  CHECK_INT(d.eepromBytes, 1024);
  CHECK_INT(d.ramSize, 2048);
  CHECK_INT(d.ramStart, 0x100);
  CHECK_INT(pragma(&d, "ADMIN PART_NAME ATmega8"), DEVICE_PRAGMA_ERROR);

  deviceInit(&d);
  CHECK_INT(pragma(&d, "CORE INSTRUCTIONS_NOT_SUPPORTED movw:break:lpm rd,z:spm"), DEVICE_PRAGMA_OK);
  CHECK_INT(d.removed, BIT(DG_MOVW) | BIT(DG_BREAK) | BIT(DG_LPMZ) | BIT(DG_SPM));
  CHECK_INT(pragma(&d, "CORE NEW_INSTRUCTIONS lpm rd,z+"), DEVICE_PRAGMA_OK);
  CHECK_INT(d.added, BIT(DG_LPMZP));
  CHECK_INT(pragma(&d, "CORE NEW_INSTRUCTIONS frobnicate"), DEVICE_PRAGMA_IGNORED);
  CHECK_INT(pragma(&d, "CORE CORE_VERSION AVR8L_0"), DEVICE_PRAGMA_ERROR);
  CHECK_INT(pragma(&d, "CORE CORE_VERSION V9"), DEVICE_PRAGMA_ERROR);
  CHECK_INT(pragma(&d, "MEMORY PROG_FLASH lots"), DEVICE_PRAGMA_IGNORED);
  CHECK_INT(pragma(&d, "ADMIN SIGNATURE 0x1e"), DEVICE_PRAGMA_IGNORED);
  CHECK_INT(pragma(&d, ""), DEVICE_PRAGMA_IGNORED);
  CHECK_INT(deviceSetName(&d, "ATmega324A", 10), 1);
  CHECK_INT(deviceSetName(&d, "an_extremely_long_name_for_a_device", 35), 0);
}

static const struct {
  uint16_t opcode;
  DeviceGroup group;
} groups[] = {
  { 0x9F01, DG_MUL },
  { 0x0201, DG_MUL },
  { 0x0301, DG_MUL },
  { 0x0309, DG_MUL },
  { 0x0381, DG_MUL },
  { 0x0389, DG_MUL },
  { 0x0189, DG_MOVW },
  { 0x940C, DG_JMP },
  { 0x940E, DG_JMP },
  { 0x9100, DG_LDS },
  { 0x9300, DG_LDS },
  { 0x95C8, DG_LPM },
  { 0x9104, DG_LPMZ },
  { 0x9105, DG_LPMZP },
  { 0x95D8, DG_ELPM },
  { 0x9106, DG_ELPMZ },
  { 0x9107, DG_ELPMZ },
  { 0x95E8, DG_SPM },
  { 0x95F8, DG_XMEGA },
  { 0x9409, DG_IJMP },
  { 0x9509, DG_IJMP },
  { 0x9419, DG_EIJMP },
  { 0x9519, DG_EIJMP },
  { 0x9601, DG_ADIW },
  { 0x9701, DG_ADIW },
  { 0x910C, DG_LDALL },
  { 0x910D, DG_LDALL },
  { 0x910E, DG_LDALL },
  { 0x8108, DG_LDALL },
  { 0x9109, DG_LDALL },
  { 0x910A, DG_LDALL },
  { 0x8100, DG_LDZ },
  { 0x9101, DG_LDALL },
  { 0x9102, DG_LDALL },
  { 0x8109, DG_LDALL },
  { 0x8101, DG_LDALL },
  { 0x930C, DG_LDALL },
  { 0x9309, DG_LDALL },
  { 0x9302, DG_LDALL },
  { 0x8309, DG_LDALL },
  { 0x8301, DG_LDALL },
  { 0x930F, DG_PUSH },
  { 0x910F, DG_PUSH },
  { 0x9598, DG_BREAK },
  { 0x9588, DG_BASIC },
  { 0x95A8, DG_BASIC },
  { 0xB100, DG_BASIC },
  { 0xB900, DG_BASIC },
  { 0x9A00, DG_BASIC },
  { 0x9800, DG_BASIC },
  { 0x1301, DG_BASIC },
  { 0xE001, DG_BASIC },
  { 0x2C01, DG_BASIC },
  { 0x8300, DG_LDZ },
  { 0x8309, DG_LDALL },
  { 0x0000, DG_BASIC },
  { 0xCFFF, DG_BASIC },
  { 0xF7F9, DG_BASIC },
  { 0x2C01, DG_BASIC },
  { 0x940B, DG_XMEGA },
  { 0x9304, DG_XMEGA },
  { 0x9307, DG_XMEGA },
};

static const struct {
  const char *name;
  const char *pragmas[10];
  uint32_t missing;
} devices[] = {
  { "AT90S1200",
    {
      "ADMIN PART_NAME AT90S1200",
      "CORE CORE_VERSION V0",
      "MEMORY PROG_FLASH 1024",
      "MEMORY EEPROM 64",
      "MEMORY INT_SRAM SIZE 0",
      "MEMORY INT_SRAM START_ADDR 0x0",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_LPM) | BIT(DG_LDALL) | BIT(DG_ADIW) | BIT(DG_PUSH) | BIT(DG_LDS) | BIT(DG_IJMP) | BIT(DG_BREAK) | BIT(DG_MOVW) | BIT(DG_LPMZ) | BIT(DG_LPMZP) | BIT(DG_SPM) | BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATtiny12",
    {
      "ADMIN PART_NAME ATtiny12",
      "CORE CORE_VERSION V0E",
      "MEMORY PROG_FLASH 1024",
      "MEMORY EEPROM 64",
      "MEMORY INT_SRAM SIZE 0",
      "MEMORY INT_SRAM START_ADDR 0x0",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_LDALL) | BIT(DG_ADIW) | BIT(DG_PUSH) | BIT(DG_LDS) | BIT(DG_IJMP) | BIT(DG_BREAK) | BIT(DG_MOVW) | BIT(DG_LPMZ) | BIT(DG_LPMZP) | BIT(DG_SPM) | BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATtiny26",
    {
      "ADMIN PART_NAME ATtiny26",
      "CORE CORE_VERSION V1",
      "CORE NEW_INSTRUCTIONS lpm rd,z",
      "MEMORY PROG_FLASH 2048",
      "MEMORY EEPROM 128",
      "MEMORY INT_SRAM SIZE 128",
      "MEMORY INT_SRAM START_ADDR 0x60",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_BREAK) | BIT(DG_MOVW) | BIT(DG_LPMZP) | BIT(DG_SPM) | BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "AT90S2313",
    {
      "ADMIN PART_NAME AT90S2313",
      "CORE CORE_VERSION V1",
      "MEMORY PROG_FLASH 2048",
      "MEMORY EEPROM 128",
      "MEMORY INT_SRAM SIZE 128",
      "MEMORY INT_SRAM START_ADDR 0x60",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_BREAK) | BIT(DG_MOVW) | BIT(DG_LPMZ) | BIT(DG_LPMZP) | BIT(DG_SPM) | BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATtiny13",
    {
      "ADMIN PART_NAME ATtiny13",
      "CORE CORE_VERSION V2",
      "CORE NEW_INSTRUCTIONS lpm rd,z+",
      "MEMORY PROG_FLASH 1024",
      "MEMORY EEPROM 64",
      "MEMORY INT_SRAM SIZE 64",
      "MEMORY INT_SRAM START_ADDR 0x60",
      NULL },
    BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATtiny85",
    {
      "ADMIN PART_NAME ATtiny85",
      "CORE CORE_VERSION V2",
      "CORE NEW_INSTRUCTIONS lpm rd,z+",
      "MEMORY PROG_FLASH 8192",
      "MEMORY EEPROM 512",
      "MEMORY INT_SRAM SIZE 512",
      "MEMORY INT_SRAM START_ADDR 0x60",
      NULL },
    BIT(DG_MUL) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega103",
    {
      "ADMIN PART_NAME ATmega103",
      "CORE CORE_VERSION V2",
      "CORE INSTRUCTIONS_NOT_SUPPORTED movw:break:lpm rd,z:spm",
      "MEMORY PROG_FLASH 131072",
      "MEMORY EEPROM 4096",
      "MEMORY INT_SRAM SIZE 4000",
      "MEMORY INT_SRAM START_ADDR 0x60",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_BREAK) | BIT(DG_MOVW) | BIT(DG_LPMZ) | BIT(DG_LPMZP) | BIT(DG_SPM) | BIT(DG_MUL) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega8",
    {
      "ADMIN PART_NAME ATmega8",
      "CORE CORE_VERSION V2E",
      "MEMORY PROG_FLASH 8192",
      "MEMORY EEPROM 512",
      "MEMORY INT_SRAM SIZE 1024",
      "MEMORY INT_SRAM START_ADDR 0x60",
      "CORE INSTRUCTIONS_NOT_SUPPORTED break",
      NULL },
    BIT(DG_BREAK) | BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega48",
    {
      "ADMIN PART_NAME ATmega48",
      "CORE CORE_VERSION V2E",
      "MEMORY PROG_FLASH 4096",
      "MEMORY EEPROM 256",
      "MEMORY INT_SRAM SIZE 512",
      "MEMORY INT_SRAM START_ADDR 0x100",
      NULL },
    BIT(DG_JMP) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega324A",
    {
      "ADMIN PART_NAME ATmega324A",
      "CORE CORE_VERSION V2E",
      "MEMORY PROG_FLASH 32768",
      "MEMORY EEPROM 1024",
      "MEMORY INT_SRAM SIZE 2048",
      "MEMORY INT_SRAM START_ADDR 0x100",
      NULL },
    BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega128",
    {
      "ADMIN PART_NAME ATmega128",
      "CORE CORE_VERSION V2E",
      "MEMORY PROG_FLASH 131072",
      "MEMORY EEPROM 4096",
      "MEMORY INT_SRAM SIZE 4096",
      "MEMORY INT_SRAM START_ADDR 0x100",
      NULL },
    BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega2560",
    {
      "ADMIN PART_NAME ATmega2560",
      "CORE CORE_VERSION V3",
      "MEMORY PROG_FLASH 262144",
      "MEMORY EEPROM 4096",
      "MEMORY INT_SRAM SIZE 8192",
      "MEMORY INT_SRAM START_ADDR 0x200",
      NULL },
    BIT(DG_XMEGA) },
  { "ATmega1280",
    {
      "ADMIN PART_NAME ATmega1280",
      "CORE CORE_VERSION V3",
      "MEMORY PROG_FLASH 131072",
      "MEMORY EEPROM 4096",
      "MEMORY INT_SRAM SIZE 8192",
      "MEMORY INT_SRAM START_ADDR 0x200",
      NULL },
    BIT(DG_EIJMP) | BIT(DG_XMEGA) },
  { "ATmega4809",
    {
      "ADMIN PART_NAME ATmega4809",
      "CORE CORE_VERSION V4",
      "MEMORY PROG_FLASH 0xC000",
      "MEMORY EEPROM 0x0100",
      "MEMORY INT_SRAM SIZE 0x1800",
      "MEMORY INT_SRAM START_ADDR 0x2800",
      NULL },
    BIT(DG_SPM) | BIT(DG_ELPM) | BIT(DG_ELPMZ) | BIT(DG_EIJMP) | BIT(DG_XMEGA) },
};

static void testDevices(void)
{
  for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) {
    if (deviceGroup(groups[i].opcode) != groups[i].group) {
      fprintf(stderr, "deviceGroup(0x%04X) is %d, want %d\n", groups[i].opcode, deviceGroup(groups[i].opcode), groups[i].group);
      check_failures_++;
    }
  }
  for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
    Device d;
    deviceInit(&d);
    for (const char *const *p = devices[i].pragmas; *p != NULL; p++) CHECK_INT(pragma(&d, *p), DEVICE_PRAGMA_OK);
    CHECK_STR(d.name, devices[i].name);
    for (int g = DG_BASIC; g <= DG_XMEGA; g++) {
      int want = !(devices[i].missing & BIT(g));
      if (deviceHas(&d, (DeviceGroup)g) != want) {
        fprintf(stderr, "%s: deviceHas(group %d) is %d, want %d\n", devices[i].name, g, !want, want);
        check_failures_++;
      }
    }
  }
  Device none;
  deviceInit(&none);
  for (int g = DG_BASIC; g <= DG_XMEGA; g++) CHECK_INT(deviceHas(&none, (DeviceGroup)g), 1);
}

static void testRelative(void)
{
  CHECK_INT(deviceRelative(0, 0, 0x100), 0xFF);
  CHECK_INT(deviceRelative(16384, 0, 0x3000), 0x2FFF);
  CHECK_INT(deviceRelative(4096, 0, 0xF00), -257);
  CHECK_INT(deviceRelative(4096, 0xF00, 0), 255);
  CHECK_INT(deviceRelative(4096, 0, 0x800), 2047);
  CHECK_INT(deviceRelative(4096, 0, 0x801), -2048);
  CHECK_INT(deviceRelative(512, 0, 0x1FF), -2);
  CHECK_INT(deviceRelative(512, 0x1FF, 0), 0);
  CHECK_INT(deviceRelative(512, 0, 0x100), 255);
  CHECK_INT(deviceRelative(512, 0, 0x101), -256);
}

static const struct {
  const char *path;
  const char *text;
} fixtures[] = {
  { "big.inc", ".device ATmega324A\n#pragma AVRPART ADMIN PART_NAME ATmega324A\n#pragma AVRPART CORE CORE_VERSION V2E\n"
              "#pragma AVRPART MEMORY PROG_FLASH 32768\n#pragma AVRPART MEMORY EEPROM 1024\n"
              "#pragma AVRPART MEMORY INT_SRAM SIZE 2048\n#pragma AVRPART MEMORY INT_SRAM START_ADDR 0x100\n" },
  { "tiny.inc", ".device ATtiny13\n#pragma AVRPART ADMIN PART_NAME ATtiny13\n#pragma AVRPART CORE CORE_VERSION V2\n"
               "#pragma AVRPART CORE NEW_INSTRUCTIONS lpm rd,z+\n#pragma AVRPART MEMORY PROG_FLASH 1024\n"
               "#pragma AVRPART MEMORY EEPROM 64\n#pragma AVRPART MEMORY INT_SRAM SIZE 64\n"
               "#pragma AVRPART MEMORY INT_SRAM START_ADDR 0x60\n" },
  { "noram.inc", ".device AT90S1200\n#pragma AVRPART ADMIN PART_NAME AT90S1200\n#pragma AVRPART CORE CORE_VERSION V0\n"
                "#pragma AVRPART MEMORY PROG_FLASH 1024\n#pragma AVRPART MEMORY EEPROM 64\n"
                "#pragma AVRPART MEMORY INT_SRAM SIZE 0\n#pragma AVRPART MEMORY INT_SRAM START_ADDR 0x0\n" },
};

static char *readFixture(void *user, const char *path, size_t *len)
{
  (void)user;
  for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
    if (strcmp(fixtures[i].path, path) == 0) {
      *len = strlen(fixtures[i].text);
      char *copy = malloc(*len + 1);
      memcpy(copy, fixtures[i].text, *len + 1);
      return copy;
    }
  }
  return NULL;
}

static const struct {
  const char *src;
  const char *flash;
  unsigned errors, warnings;
} programs[] = {
  { ".include \"big.inc\"\n.dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", "000000: 00 e0 11 e0\n", 0, 0 },
  { ".include \"big.inc\"\nmul r16, r17\njmp 0\n", "000000: 01 9f 0c 94 00 00\n", 0, 0 },
  { ".include \"big.inc\"\nelpm\n", "", 1, 0 },
  { ".include \"big.inc\"\n.org 0x3fff\nnop\n", "007ffe: 00 00\n", 0, 0 },
  { ".include \"big.inc\"\n.org 0x3fff\nnop\nnop\n", "", 1, 0 },
  { ".include \"big.inc\"\n.dseg\n.org 0xff\n", "", 1, 0 },
  { ".include \"big.inc\"\n.dseg\n.org 0x8ff\n.byte 2\n", "", 1, 0 },
  { ".include \"big.inc\"\n.eseg\n.org 0x3ff\n.db 1, 2\n", "", 1, 0 },
  { ".include \"big.inc\"\njmp 0x4000\n", "", 1, 0 },
  { ".include \"big.inc\"\n.device ATmega8\n", "", 1, 0 },
  { ".include \"big.inc\"\n.device atmega324a\nnop\n", "000000: 00 00\n", 0, 0 },
  { ".device ATmega324A\nnop\n", "000000: 00 00\n", 0, 1 },
  { ".include \"tiny.inc\"\nmul r16, r17\n", "", 1, 0 },
  { ".include \"tiny.inc\"\nlpm r16, Z+\nmovw r16, r18\n", "000000: 05 91 89 01\n", 0, 0 },
  { ".include \"tiny.inc\"\n.org 0x1ff\nrjmp 0\n", "0003fe: 00 c0\n", 0, 0 },
  { ".include \"tiny.inc\"\nrjmp 0x1ff\n", "000000: fe cf\n", 0, 0 },
  { ".include \"tiny.inc\"\nrjmp 0x200\n", "", 1, 0 },
  { ".include \"noram.inc\"\n.dseg\nv: .byte 1\n", "", 1, 0 },
  { ".include \"noram.inc\"\nld r16, Z\nlpm\n", "", 1, 0 },
  { "#pragma AVRPART CORE CORE_VERSION AVR8L_0\nnop\n", "", 1, 0 },
  { ".device ATmega324A\n.db __PART_NAME__\n#ifdef __ATmega324A__\nnop\n#endif\n",
    "000000: 41 54 6d 65 67 61 33 32 34 41 00 00\n", 0, 1 },
};

static void testPrograms(void)
{
  for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
    AvrCtx ctx;
    ctxInit(&ctx, (AvrVfs){readFixture, NULL});
    FileId id = sourceMapAdd(&ctx.sources, "t.asm", programs[i].src, strlen(programs[i].src));
    Image flash, eeprom;
    imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
    imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
    int ok = assembleAll(&ctx, id, &flash, &eeprom);
    unsigned warnings = 0;
    for (Diag *d = ctx.diags.head; d != NULL; d = d->next) warnings += d->severity == DIAG_WARNING;
    const char *text = ok ? imageToText(&flash, &ctx.arena) : "";
    if (ok != (programs[i].errors == 0) || ctx.diags.errorCount != programs[i].errors || warnings != programs[i].warnings ||
        strcmp(text, programs[i].flash) != 0) {
      fprintf(stderr, "program %zu: ok=%d, %u errors (want %u), %u warnings (want %u), flash \"%s\" (want \"%s\") for:\n%s\n",
              i, ok, ctx.diags.errorCount, programs[i].errors, warnings, programs[i].warnings, text, programs[i].flash,
              programs[i].src);
      for (Diag *d = ctx.diags.head; d != NULL; d = d->next) fprintf(stderr, "  %s\n", d->message);
      check_failures_++;
    }
    ctxFree(&ctx);
  }
}

int main(void)
{
  testPragmas();
  testDevices();
  testRelative();
  testPrograms();
  return TEST_RESULT();
}