#include "avrasm/assemble.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { T_GOOD, T_BAD, T_STRICT } Kind;
typedef struct Row {
  int n;
  const char *src;
  Kind kind;
  const char *image;
  const char *define;
  const char *incDir;
} Row;

typedef struct Fixture {
  const char *path;
  const char *text;
} Fixture;

static const Fixture fixtures[] = {
    {"inc/a.inc", "ldi r16, 1\n"},
    {"inc/b.inc", ".include \"c.inc\"\n"},
    {"inc/c.inc", "ldi r17, 2\n"},
    {"inc/n.inc", ".include \"inc/a.inc\"\n"},
    {"inc/d.inc", ".equ dval = 7\n"},
    {"inc/e.inc", ".org 4\nnop\n"},
    {"inc/f.inc", "ldi r16, 1"},
    {"inc/h.inc", ".dseg\nhv: .byte 2\n"},
    {"inc/i.inc", ".macro im\nnop\n.endm\n"},
    {"inc/j.inc", ".if 1\nnop\n"},
    {"inc/bad.inc", "nop\nfoo\n"},
    {"inc/l.inc", "ldi r16, 1\r\nldi r17, 2\r\n"},
    {"inc/q.inc", "ldi r16, 1\n.exit\nldi r17, 2\n"},
    {"inc/self.inc", ".include \"inc/self.inc\"\nnop\n"},
    {"third-party/includes/m324Adef.inc",
     "#ifndef _M324ADEF_INC_\n#define _M324ADEF_INC_\n.device ATmega324A\n"
     ".equ\tSREG\t= 0x3f\n.def\tXL\t= r26\n.def\tXH\t= r27\n#endif\n"},
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

static const Row rows[] = {
  {1300, ".if 1\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1301, ".if 0\nldi r16, 1\n.endif\nldi r17, 2\n", T_GOOD, "000000: 12 e0\n", NULL, NULL},
  {1302, ".if 1\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1303, ".if 0\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1304, ".if 0\nldi r16, 1\n.elif 1\nldi r16, 2\n.else\nldi r16, 3\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1305, ".if 0\nldi r16, 1\n.elif 0\nldi r16, 2\n.else\nldi r16, 3\n.endif\n", T_GOOD, "000000: 03 e0\n", NULL, NULL},
  {1306, ".if 1\nldi r16, 1\n.elif 1\nldi r16, 2\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1307, ".if 1\n.if 0\nldi r16, 1\n.else\nldi r16, 2\n.endif\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1308, ".if 0\n.if 1\nldi r16, 1\n.else\nldi r16, 2\n.endif\n.endif\nldi r17, 3\n", T_GOOD, "000000: 13 e0\n", NULL, NULL},
  {1309, ".if 2 > 1 && 3 != 3\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1310, ".if -1\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1311, ".equ k = 3\n.if k == 3\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1312, ".set k = 0\n.if k\nldi r16, 1\n.endif\n.set k = 1\n.if k\nldi r17, 2\n.endif\n", T_GOOD, "000000: 12 e0\n", NULL, NULL},
  {1313, ".if later\nldi r16, 1\n.endif\nlater: nop\n", T_BAD, "", NULL, NULL},
  {1314, ".if later == 1\nldi r16, 1\n.endif\n.equ later = 1\n", T_BAD, "", NULL, NULL},
  {1315, ".if nope\nldi r16, 1\n.endif\n", T_BAD, "", NULL, NULL},
  {1316, ".equ k = 1\n.if defined(k)\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1317, ".if defined(nope)\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1318, ".ifdef k\nldi r16, 1\n.endif\n.equ k = 1\n", T_GOOD, "", NULL, NULL},
  {1319, ".equ k = 1\n.ifdef k\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1320, ".ifndef k\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1321, ".equ k = 1\n.ifndef k\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1322, "k: nop\n.ifdef k\nldi r17, 1\n.endif\n", T_GOOD, "000000: 00 00 11 e0\n", NULL, NULL},
  {1323, ".set k = 1\n.ifdef k\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1324, ".def tmp = r16\n.ifdef tmp\nldi tmp, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1325, ".macro m\nnop\n.endm\n.ifdef m\nldi r16, 1\n.endif\n", T_BAD, "", NULL, NULL},
  {1326, ".if\nnop\n.endif\n", T_BAD, "", NULL, NULL},
  {1327, ".if 1 2\nnop\n.endif\n", T_BAD, "", NULL, NULL},
  {1328, ".else\nnop\n", T_BAD, "", NULL, NULL},
  {1329, ".endif\nnop\n", T_BAD, "", NULL, NULL},
  {1330, ".elif 1\nnop\n", T_BAD, "", NULL, NULL},
  {1331, ".if 1\nnop\n", T_BAD, "", NULL, NULL},
  {1332, ".if 1\nnop\n.else\nret\n.else\nnop\n.endif\n", T_BAD, "", NULL, NULL},
  {1333, ".if 1\nnop\n.else\nret\n.elif 1\nnop\n.endif\n", T_BAD, "", NULL, NULL},
  {1334, ".if 1\nnop\n.endif\n.endif\n", T_BAD, "", NULL, NULL},
  {1335, ".if 0\nfoo bar\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1336, ".if 0\nldi r16\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1337, ".if 0\nlabel: nop\n.endif\nrjmp label\n", T_BAD, "", NULL, NULL},
  {1338, ".if 0\n.equ k = 1\n.endif\nldi r16, k\n", T_BAD, "", NULL, NULL},
  {1339, ".if 0\n.dseg\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1340, ".if 0\n.include \"inc/nofile.inc\"\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1341, ".if 0\n.error \"boom\"\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1342, ".if 0\n.macro m\nnop\n.endm\n.endif\nm\n", T_BAD, "", NULL, NULL},
  {1343, ".if 0\nldi r16, \"unterminated\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1344, ".error \"boom\"\nnop\n", T_BAD, "", NULL, NULL},
  {1345, ".message \"hello\"\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1346, ".warning \"careful\"\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1347, ".error\nnop\n", T_BAD, "", NULL, NULL},
  {1348, ".message hello\nnop\n", T_BAD, "", NULL, NULL},
  {1349, "nop\n.exit\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1350, ".list\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1351, ".nolist\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1352, ".listmac\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1353, ".overlap\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1354, ".nooverlap\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1355, ".macro m\nldi r16, @0\n.endmacro\nm 5\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1356, ".macro m\nnop\n.endm\nm\nm\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1357, ".macro m\nldi @0, @1\n.endm\nm r17, 9\n", T_GOOD, "000000: 19 e0\n", NULL, NULL},
  {1358, ".macro m\nldi r16, @0 * 2\n.endm\nm 1+2\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1359, ".macro m\nldi r16, @0\n.endm\nm 1 + 2 ; comment\n", T_GOOD, "000000: 03 e0\n", NULL, NULL},
  {1360, ".macro m\nldi r16, @0\n.endm\nm low(0x1234)\n", T_GOOD, "000000: 04 e3\n", NULL, NULL},
  {1361, ".macro m\n.db @0\n.endm\nm \"ab\"\n", T_GOOD, "000000: 61 62\n", NULL, NULL},
  {1362, ".macro m\nldi r16, @1\n.endm\nm , 5\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1363, ".macro m\nldi r16, @1\n.endm\nm 5\n", T_BAD, "", NULL, NULL},
  {1364, ".macro m\nnop\n.endm\nm 1, 2, 3\n", T_STRICT, "", NULL, NULL},
  {1365, ".macro m\nldi r16, @0\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1366, ".macro m\nldi r16, @9\n.endm\nm 0,0,0,0,0,0,0,0,0,7\n", T_GOOD, "000000: 07 e0\n", NULL, NULL},
  {1367, ".macro m\nldi r16, @10\n.endm\nm 1\n", T_BAD, "", NULL, NULL},
  {1368, "ldi r16, @0\n", T_BAD, "", NULL, NULL},
  {1369, ".macro m\nl@0: nop\n.endm\nm 1\nrjmp l1\n", T_BAD, "", NULL, NULL},
  {1370, ".macro m\nnop\n.endm\nstart: m\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1371, ".macro MyM\nnop\n.endm\nmym\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1372, ".macro m\nl: nop\n.endm\nm\nm\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1373, ".macro m\nl: nop\n.endm\nm\nrjmp l\n", T_BAD, "", NULL, NULL},
  {1374, ".macro m\nnop\n.endm\n.macro m\nret\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1375, ".macro nop\nret\n.endm\nnop\n", T_BAD, "", NULL, NULL},
  {1376, ".macro r5\nnop\n.endm\n", T_BAD, "", NULL, NULL},
  {1377, ".macro\nnop\n.endm\n", T_BAD, "", NULL, NULL},
  {1378, ".macro m foo\nnop\n.endm\nm 1\n", T_BAD, "", NULL, NULL},
  {1379, "m: nop\n.macro m\nnop\n.endm\n", T_BAD, "", NULL, NULL},
  {1380, ".equ m = 1\n.macro m\nnop\n.endm\n", T_BAD, "", NULL, NULL},
  {1381, ".macro m\nnop\n.endm\n.equ m = 1\n", T_BAD, "", NULL, NULL},
  {1382, "m\n.macro m\nnop\n.endm\n", T_BAD, "", NULL, NULL},
  {1383, ".macro m\nnop\n", T_BAD, "", NULL, NULL},
  {1384, ".endm\n", T_BAD, "", NULL, NULL},
  {1385, ".macro a\nnop\n.endm\n.macro b\na\na\n.endm\nb\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1386, ".macro a\nldi r16, @0\n.endm\n.macro b\na @0 + 1\n.endm\nb 7\n", T_GOOD, "000000: 08 e0\n", NULL, NULL},
  {1387, ".macro a\n.macro b\nnop\n.endm\n.endm\na\nb\n", T_BAD, "", NULL, NULL},
  {1388, ".macro cnt\n.if @0 > 0\nnop\ncnt @0-1\n.endif\n.endm\ncnt 3\n", T_GOOD, "000000: 00 00 00 00 00 00\n", NULL, NULL},
  {1389, ".macro m\n.set k = @0\n.endm\nm 1\nm 2\nldi r16, k\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1390, ".macro m\n.equ k = @0\n.endm\nm 1\nm 2\n", T_BAD, "", NULL, NULL},
  {1391, ".macro m\n.if @0\nnop\n.endif\n.endm\nm 1\nm 0\nm 1\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1392, ".macro m\n.db @0\n.endm\nm 1\n", T_GOOD, "000000: 01 00\n", NULL, NULL},
  {1393, ".macro m\n.org @0\n.endm\nm 4\nnop\n", T_GOOD, "000008: 00 00\n", NULL, NULL},
  {1394, ".macro m\nldi r16, 300\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1395, ".macro m\n.dseg\n.endm\nm\nv: .byte 1\n.cseg\nldi r16, low(v)\n", T_GOOD, "000000: 00 e6\n", NULL, NULL},
  {1396, "#define K 5\nldi r16, K\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1397, "#define K 5+1\nldi r16, K*2\n", T_GOOD, "000000: 07 e0\n", NULL, NULL},
  {1398, "#define K 5 ; comment\nldi r16, K\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1399, "#define K\n#ifdef K\nldi r16, 1\n#endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1400, "#ifdef K\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1401, "#define K 1\n#ifndef K\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1402, "#define K 2\n#if K == 2\nldi r16, 1\n#endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1403, "#define K 2\n#if K > 5\nldi r16, 1\n#elif K > 1\nldi r16, 2\n#endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1404, "#define K 2\n#if defined(K)\nldi r16, 1\n#endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1405, "#define K 2\n#undef K\n#ifdef K\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1406, ".equ k = 1\n#ifdef k\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1407, "#define F(a) a+1\nldi r16, F(3)\n", T_GOOD, "000000: 04 e0\n", NULL, NULL},
  {1408, "#define F(a, b) a+b\nldi r16, F(3, 4)\n", T_GOOD, "000000: 07 e0\n", NULL, NULL},
  {1409, "#define A B\n#define B 3\nldi r16, A\n", T_GOOD, "000000: 03 e0\n", NULL, NULL},
  {1410, "#define K 5\n.db \"K\", K\n", T_GOOD, "000000: 4b 05\n", NULL, NULL},
  {1411, "#define K 5\n.equ K2 = K\nldi r16, K2\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1412, "#define foo nop\nfoo\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1413, "#define K 5\n#define K 6\nldi r16, K\n", T_GOOD, "000000: 06 e0\n", NULL, NULL},
  {1414, "#define K 5\n.equ K = 6\n", T_BAD, "", NULL, NULL},
  {1415, "#define K \"ab\"\n.db K\n", T_GOOD, "000000: 61 62\n", NULL, NULL},
  {1416, "#error \"boom\"\nnop\n", T_BAD, "", NULL, NULL},
  {1417, "#message \"hello\"\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1418, "#warning \"careful\"\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1419, "#pragma message \"hello\"\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1420, "#pragma foo\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1421, "#pragma AVRPART ADMIN PART_NAME ATmega324A\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1422, "#if 0\nfoo bar\n#endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1423, "#if 1\nnop\n", T_BAD, "", NULL, NULL},
  {1424, "#endif\n", T_BAD, "", NULL, NULL},
  {1425, "#define\nnop\n", T_BAD, "", NULL, NULL},
  {1426, "#DEFINE K 5\nldi r16, K\n", T_BAD, "", NULL, NULL},
  {1427, "#include \"inc/a.inc\"\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1428, ".include \"inc/a.inc\"\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1429, ".include \"tests/corpus/inc/a.inc\"\n", T_BAD, "", NULL, NULL},
  {1430, ".include \"a.inc\"\n", T_BAD, "", NULL, NULL},
  {1431, ".include \"inc\\a.inc\"\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1432, ".include \"inc/b.inc\"\n", T_BAD, "", NULL, NULL},
  {1433, ".include \"inc/n.inc\"\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1434, ".include \"inc/nofile.inc\"\n", T_BAD, "", NULL, NULL},
  {1435, ".include inc/a.inc\n", T_BAD, "", NULL, NULL},
  {1436, ".include <inc/a.inc>\n", T_BAD, "", NULL, NULL},
  {1437, ".include \"INC/A.INC\"\n", T_STRICT, "", NULL, NULL},
  {1438, ".include \"m324Adef.inc\"\nin r16, SREG\nldi XL, 1\n", T_GOOD, "000000: 0f b7 a1 e0\n", NULL, NULL},
  {1439, ".include \"M324ADEF.INC\"\nnop\n", T_STRICT, "", NULL, NULL},
  {1440, ".include \"inc/a.inc\"\n.include \"inc/a.inc\"\n", T_GOOD, "000000: 01 e0 01 e0\n", NULL, NULL},
  {1441, ".include \"inc/d.inc\"\nldi r16, dval\n", T_GOOD, "000000: 07 e0\n", NULL, NULL},
  {1442, ".include \"inc/e.inc\"\nnop\n", T_GOOD, "000008: 00 00 00 00\n", NULL, NULL},
  {1443, ".include \"inc/f.inc\"\nldi r17, 2\n", T_GOOD, "000000: 01 e0 12 e0\n", NULL, NULL},
  {1444, ".include \"inc/h.inc\"\nnop\n", T_BAD, "", NULL, NULL},
  {1445, ".include \"inc/i.inc\"\nim\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1446, ".include \"inc/j.inc\"\n.endif\n", T_BAD, "", NULL, NULL},
  {1447, ".include \"inc/j.inc\"\n", T_BAD, "", NULL, NULL},
  {1448, ".include \"inc/bad.inc\"\n", T_BAD, "", NULL, NULL},
  {1449, ".include \"inc/l.inc\"\n", T_GOOD, "000000: 01 e0 12 e0\n", NULL, NULL},
  {1450, ".include \"inc/q.inc\"\nldi r18, 3\n", T_GOOD, "000000: 01 e0 23 e0\n", NULL, NULL},
  {1451, ".macro m\n.include \"inc/a.inc\"\n.endm\nm\nm\n", T_BAD, "", NULL, NULL},
  {1500, ".device ATmega324A\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1501, ".device atmega324a\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1502, ".device ATmega324P\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1503, ".device ATmega99\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1504, ".device\nnop\n", T_BAD, "", NULL, NULL},
  {1505, ".device ATmega324A\n.device ATmega324A\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1506, ".device ATmega324A\n.device ATmega328P\nnop\n", T_STRICT, "", NULL, NULL},
  {1507, "nop\n.device ATmega324A\nnop\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1508, ".device ATmega324A\n.dseg\n.device ATmega324A\nnop\n", T_BAD, "", NULL, NULL},
  {1509, ".dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", T_GOOD, "000000: 00 e6 10 e0\n", NULL, NULL},
  {1510, ".device ATmega324A\n.dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", T_GOOD, "000000: 00 e6 10 e0\n", NULL, NULL},
  {1511, ".device ATmega8\n.dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", T_GOOD, "000000: 00 e6 10 e0\n", NULL, NULL},
  {1512, ".device ATtiny13\n.dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", T_GOOD, "000000: 00 e6 10 e0\n", NULL, NULL},
  {1513, ".device AT90S1200\n.dseg\nv: .byte 1\n.cseg\nldi r16, low(v)\nldi r17, high(v)\n", T_GOOD, "000000: 00 e6 10 e0\n", NULL, NULL},
  {1514, ".device ATmega324A\n.dseg\n.org 0x60\nv: .byte 1\n.cseg\nldi r16, low(v)\n", T_GOOD, "000000: 00 e6\n", NULL, NULL},
  {1515, ".device ATmega324A\n.org 0x3fff\nnop\n", T_GOOD, "007ffe: 00 00\n", NULL, NULL},
  {1516, ".device ATmega324A\n.org 0x4000\nnop\n", T_GOOD, "008000: 00 00\n", NULL, NULL},
  {1517, ".device ATmega324A\n.org 0x3fff\nnop\nnop\n", T_GOOD, "007ffe: 00 00 00 00\n", NULL, NULL},
  {1518, ".device ATmega324A\n.dseg\n.org 0x8ff\n.byte 1\n", T_GOOD, "", NULL, NULL},
  {1519, ".device ATmega324A\n.dseg\n.org 0x8ff\n.byte 2\n", T_GOOD, "", NULL, NULL},
  {1520, ".device ATmega324A\n.dseg\n.org 0x900\nv: .byte 1\n", T_GOOD, "", NULL, NULL},
  {1521, ".device ATmega324A\n.dseg\n.byte 0x800\n.byte 1\n", T_GOOD, "", NULL, NULL},
  {1522, ".device ATmega324A\n.eseg\n.org 0x3ff\n.db 1\n", T_GOOD, "", NULL, NULL},
  {1523, ".device ATmega324A\n.eseg\n.org 0x400\n.db 1\n", T_GOOD, "", NULL, NULL},
  {1524, ".device ATmega324A\n.eseg\n.org 0x3ff\n.db 1, 2\n", T_GOOD, "", NULL, NULL},
  {1526, "elpm\n", T_GOOD, "000000: d8 95\n", NULL, NULL},
  {1527, "eijmp\n", T_GOOD, "000000: 19 94\n", NULL, NULL},
  {1528, "des 1\n", T_STRICT, "", NULL, NULL},
  {1529, "xch Z, r16\n", T_STRICT, "", NULL, NULL},
  {1530, ".device ATmega324A\nelpm\n", T_GOOD, "000000: d8 95\n", NULL, NULL},
  {1531, ".device ATmega324A\nelpm r16, Z\n", T_GOOD, "000000: 06 91\n", NULL, NULL},
  {1532, ".device ATmega324A\neijmp\n", T_GOOD, "000000: 19 94\n", NULL, NULL},
  {1533, ".device ATmega324A\neicall\n", T_GOOD, "000000: 19 95\n", NULL, NULL},
  {1534, ".device ATmega324A\ndes 1\n", T_STRICT, "", NULL, NULL},
  {1535, ".device ATmega324A\nxch Z, r16\n", T_STRICT, "", NULL, NULL},
  {1536, ".device ATmega324A\nspm\n", T_GOOD, "000000: e8 95\n", NULL, NULL},
  {1537, ".device ATmega324A\nfmul r16, r17\n", T_GOOD, "000000: 09 03\n", NULL, NULL},
  {1538, ".device ATmega324A\nmovw r16, r18\n", T_GOOD, "000000: 89 01\n", NULL, NULL},
  {1539, ".device ATmega324A\nlpm r16, Z+\n", T_GOOD, "000000: 05 91\n", NULL, NULL},
  {1540, ".device ATmega324A\njmp 0\ncall 0\n", T_GOOD, "000000: 0c 94 00 00 0e 94 00 00\n", NULL, NULL},
  {1541, ".device ATmega8\njmp 0\n", T_GOOD, "000000: 0c 94 00 00\n", NULL, NULL},
  {1542, ".device ATmega8\ncall 0\n", T_GOOD, "000000: 0e 94 00 00\n", NULL, NULL},
  {1543, ".device ATmega8\nmul r16, r17\n", T_GOOD, "000000: 01 9f\n", NULL, NULL},
  {1544, ".device ATmega8\nelpm\n", T_GOOD, "000000: d8 95\n", NULL, NULL},
  {1545, ".device ATtiny13\nmul r16, r17\n", T_GOOD, "000000: 01 9f\n", NULL, NULL},
  {1546, ".device ATtiny13\nmovw r16, r18\n", T_GOOD, "000000: 89 01\n", NULL, NULL},
  {1547, ".device ATtiny13\nlpm r16, Z\n", T_GOOD, "000000: 04 91\n", NULL, NULL},
  {1548, ".device ATtiny13\nadiw r24, 1\n", T_GOOD, "000000: 01 96\n", NULL, NULL},
  {1549, ".device ATtiny13\nlds r16, 0x60\n", T_GOOD, "000000: 00 91 60 00\n", NULL, NULL},
  {1550, ".device ATtiny13\nrjmp 0\n", T_GOOD, "000000: ff cf\n", NULL, NULL},
  {1551, ".device AT90S1200\nmul r16, r17\n", T_GOOD, "000000: 01 9f\n", NULL, NULL},
  {1552, ".device AT90S1200\nlpm\n", T_GOOD, "000000: c8 95\n", NULL, NULL},
  {1553, ".device AT90S1200\nld r16, X\n", T_GOOD, "000000: 0c 91\n", NULL, NULL},
  {1554, ".device ATmega324A\nrjmp 0x3000\n", T_BAD, "", NULL, NULL},
  {1555, ".device ATmega324A\n.org 0x3000\nrjmp 0\n", T_BAD, "", NULL, NULL},
  {1556, ".device ATmega324A\nbrne 0x100\n", T_BAD, "", NULL, NULL},
  {1557, ".device ATmega8\nrjmp 0xf00\n", T_BAD, "", NULL, NULL},
  {1558, ".device ATmega8\n.org 0xf00\nrjmp 0\n", T_BAD, "", NULL, NULL},
  {1559, ".device ATmega8\n.org 0xfff\nrjmp 0\n", T_BAD, "", NULL, NULL},
  {1560, "rjmp 0x3000\n", T_BAD, "", NULL, NULL},
  {1600, ".if 0\n#define K 5\n.endif\nldi r16, K\n", T_GOOD, "000000: 05 e0\n", NULL, NULL},
  {1601, ".if 0\n#error \"boom\"\n.endif\nnop\n", T_BAD, "", NULL, NULL},
  {1602, ".if 0\n#warning \"w\"\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1603, ".if 0\n#include \"inc/a.inc\"\n.endif\nnop\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1604, "#define K 1\n.if K\nldi r16, 1\n.endif\n", T_GOOD, "000000: 01 e0\n", NULL, NULL},
  {1605, "#if 0\n.if 1\nnop\n.endif\n#endif\nldi r16, 2\n", T_GOOD, "000000: 02 e0\n", NULL, NULL},
  {1606, "l: nop\n.macro m\nl: nop\n.endm\nm\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1607, ".macro m\nl: nop\n.endm\nm\nl: nop\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1608, ".macro m\nl: nop\n.endm\nl: nop\nm\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1609, ".macro m\nrjmp g\n.endm\nm\ng: nop\n", T_GOOD, "000000: 00 c0 00 00\n", NULL, NULL},
  {1610, ".macro m\nrjmp @0\n.endm\nm l\nl: nop\n", T_GOOD, "000000: 00 c0 00 00\n", NULL, NULL},
  {1611, ".macro m\nl: nop\nl: nop\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1612, ".macro m\nm\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1613, ".macro m\nnop\nm\n.endm\nm\n", T_BAD, "", NULL, NULL},
  {1614, ".include \"inc/self.inc\"\n", T_BAD, "", NULL, NULL},
  {1615, ".macro m\n.equ k@0 = 1\n.endm\nm 1\nldi r16, k1\n", T_STRICT, "", NULL, NULL},
  {1616, ".macro m\nl@0: nop\n.endm\nm 1\nrjmp l1\n", T_BAD, "", NULL, NULL},
  {1617, ".macro m\nnop\n.endm\n.macro n\nm\n.endm\nn\nn\n", T_GOOD, "000000: 00 00 00 00\n", NULL, NULL},
  {1618, "#define A(x) x+1\n.macro m\nldi r16, A(@0)\n.endm\nm 2\n", T_GOOD, "000000: 03 e0\n", NULL, NULL},
  {1619, "#define M m\n.macro m\nnop\n.endm\nM\n", T_GOOD, "000000: 00 00\n", NULL, NULL},
  {1650, "ldi r16, K\n", T_GOOD, "000000: 05 e0\n", "K=5", NULL},
  {1651, "#ifdef K\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 01 e0\n", "K=5", NULL},
  {1652, ".ifdef K\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_BAD, "", "K=5", NULL},
  {1660, "ldi r16, K\n", T_GOOD, "000000: 01 e0\n", "K", NULL},
  {1661, "#ifdef K\nldi r16, 1\n#else\nldi r16, 2\n#endif\n", T_GOOD, "000000: 01 e0\n", "K", NULL},
  {1662, ".ifdef K\nldi r16, 1\n.else\nldi r16, 2\n.endif\n", T_BAD, "", "K", NULL},
  {1670, ".include \"a.inc\"\n", T_GOOD, "000000: 01 e0\n", NULL, "inc"},
  {1671, ".include <a.inc>\n", T_GOOD, "000000: 01 e0\n", NULL, "inc"},
  {9001, "#define K 5\n.ifdef K\nldi r16, 1\n.endif\n", T_BAD, "", NULL, NULL},
  {9002, "#define K 5\n.ifndef K\nldi r16, 1\n.endif\n", T_BAD, "", NULL, NULL},
};

static int assembleRow(const Row *row, char **text)
{
  AvrCtx ctx;
  ctxInit(&ctx, (AvrVfs){readFixture, NULL});
  FileId id = sourceMapAdd(&ctx.sources, "t.asm", row->src, strlen(row->src));
  Image flash, eeprom;
  imageInit(&flash, &ctx.arena, ASSEMBLE_DEFAULT_IMAGE_SIZE);
  imageInit(&eeprom, &ctx.arena, ASSEMBLE_DEFAULT_EEPROM_SIZE);
  const char *dirs[] = {"third-party/includes", row->incDir};
  const char *defs[] = {row->define};
  AsmOptions opt = {dirs, row->incDir != NULL ? 2u : 1u, defs, row->define != NULL ? 1u : 0u};
  int ok = assembleOpt(&ctx, id, &flash, &eeprom, &opt);
  const char *t = imageToText(&flash, &ctx.arena);
  *text = malloc(strlen(t) + 1);
  strcpy(*text, t);
  if (!ok && row->kind == T_GOOD) {
    for (Diag *d = ctx.diags.head; d; d = d->next) fprintf(stderr, "  %s\n", d->message);
  }
  ctxFree(&ctx);
  return ok;
}

int main(void)
{
  for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
    const Row *row = &rows[i];
    char *text;
    int ok = assembleRow(row, &text);
    if (row->kind == T_GOOD) {
      if (!ok || strcmp(text, row->image) != 0) {
        fprintf(stderr, "%d: ok=%d, image \"%s\" (want \"%s\") for:\n%s\n", row->n, ok, text, row->image, row->src);
        check_failures_++;
      }
    } else if (ok) {
      fprintf(stderr, "%d: %s but it was accepted:\n%s\n", row->n,
              row->kind == T_BAD ? "AVRASM2 refuses this" : "we refuse this on purpose", row->src);
      check_failures_++;
    }
    free(text);
  }
  return TEST_RESULT();
}