#include "avrasm/lint.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "avrasm/diag.h"
#include "avrasm/image.h"
#include "avrasm/outline.h"
#include "avrasm/source.h"

// where an instruction can send the processor next
typedef enum Flow
{
  FLOW_NEXT,           // the next instruction
  FLOW_JUMP,           // rjmp, jmp: the target only
  FLOW_CALL,           // rcall, call: the target, and back to the next instruction
  FLOW_BRANCH,         // brbs, brbc, breq ...: the target or the next
  FLOW_SKIP,           // cpse, sbrc, sbrs, sbic, sbis: the next, or the one after it
  FLOW_RET,            // ret
  FLOW_RETI,           // reti
  FLOW_INDIRECT_JUMP,  // ijmp, eijmp: wherever Z points
  FLOW_INDIRECT_CALL   // icall, eicall
} Flow;

// one thing in program memory such as an instruction or the words of one
typedef struct Item
{
  const OutlineCode *code;
  int64_t pc;
  uint32_t words;
  int isData;
  uint16_t op;        // the first word
  uint16_t op2;       // the second word of jmp, call, lds and sts
  Flow flow;
  int64_t target;     // FLOW_JUMP, FLOW_CALL and FLOW_BRANCH: the word address
  int stack;          // push +1, pop -1
  int changesFlags;   // writes a status flag (C Z N V S H T): what an interrupt must not do before saving SREG
  int readsSreg;      // in Rd, SREG
} Item;

// each kind of warning is given once per item
enum {
  WARN_SKIP         = 1 << 0,
  WARN_REGISTER     = 1 << 1,
  WARN_FLOW         = 1 << 2,
  WARN_UNREACHABLE  = 1 << 3,
  WARN_STACK        = 1 << 4,
  WARN_RETURN       = 1 << 5,
  WARN_SREG         = 1 << 6
};

typedef enum Context { CONTEXT_RESET, CONTEXT_ROUTINE, CONTEXT_INTERRUPT } Context;

typedef struct Lint
{
  DiagList *diags;
  Item *items;      // sorted by address
  uint32_t count;
  uint8_t *warned;  // per item: the WARN_ bits given already
  int32_t *depth;   // per item for one walk: bytes pushed when it runs INT32_MIN: not reached yet
  uint8_t *saved;   // per item for one walk: bit 0 reached with SREG not saved, bit 1 with it saved
} Lint;

#define SREG_IO 0x3F

// ---- reading the instructions ----

static uint16_t wordAt(const Image *flash, int64_t pc)
{
  uint32_t a = (uint32_t)pc * 2;
  return (uint16_t)(flash->bytes[a] | flash->bytes[a + 1] << 8);
}

static int changesFlags(uint16_t w)
{
  static const struct {
    uint16_t mask, value;
  } alu[] = {
    { 0xFF00, 0x0200 }, { 0xFF00, 0x0300 },                      // muls, mulsu fmul fmuls fmulsu
    { 0xFC00, 0x0400 }, { 0xFC00, 0x0800 }, { 0xFC00, 0x0C00 },  // cpc, sbc, add (lsl)
    { 0xFC00, 0x1400 }, { 0xFC00, 0x1800 }, { 0xFC00, 0x1C00 },  // cp, sub, adc (rol)
    { 0xFC00, 0x2000 }, { 0xFC00, 0x2400 }, { 0xFC00, 0x2800 },  // and (tst), eor (clr), or
    { 0xF000, 0x3000 }, { 0xF000, 0x4000 }, { 0xF000, 0x5000 },  // cpi, sbci, subi
    { 0xF000, 0x6000 }, { 0xF000, 0x7000 },                      // ori (sbr), andi (cbr)
    { 0xFE0F, 0x9400 }, { 0xFE0F, 0x9401 }, { 0xFE0F, 0x9403 },  // com, neg, inc
    { 0xFE0F, 0x9405 }, { 0xFE0F, 0x9406 }, { 0xFE0F, 0x9407 },  // asr, lsr, ror
    { 0xFE0F, 0x940A },                                          // dec
    { 0xFF00, 0x9600 }, { 0xFF00, 0x9700 }, { 0xFC00, 0x9C00 },  // adiw, sbiw, mul
    { 0xFE08, 0xFA00 },                                          // bst (the T flag)
  };

  for (size_t i = 0; i < sizeof alu / sizeof alu[0]; i++) {
    if ((w & alu[i].mask) == alu[i].value) return 1;
  }
  // bset and bclr (sec, clz, set, ...), but not sei and cli: the I flag is not what SREG is saved for.
  if ((w & 0xFF8F) == 0x9408 || (w & 0xFF8F) == 0x9488) return ((w >> 4) & 7) != 7;
  return 0;
}

// the processors program counter has just enough bits for the flash, so a relative
// jump past rither end comes back in at the other.
static int64_t wrap(int64_t target, int64_t flashWords)
{
  if (flashWords <= 0) return target;
  target %= flashWords;
  return target < 0 ? target + flashWords : target;
}

// flashWords is the devices program memory (FLASHEND + 1) or 0 when there is no device file.
static void decode(Item *it, const Image *flash, int64_t flashWords)
{
  uint16_t w = wordAt(flash, it->pc);
  it->op = w;
  it->op2 = it->words == 2 ? wordAt(flash, it->pc + 1) : 0;
  it->flow = FLOW_NEXT;
  if ((w & 0xE000) == 0xC000) {
    // rjmp 1100, rcall 1101: 12-bit relative
    int64_t k = w & 0x0FFF;
    if (k & 0x800) k -= 0x1000;
    it->flow = (w & 0x1000) ? FLOW_CALL : FLOW_JUMP;
    it->target = wrap(it->pc + 1 + k, flashWords);
  } else if ((w & 0xFE0C) == 0x940C) {
    // jmp 1001 010k kkkk 110k, call ... 111k: 22-bit absolute
    it->flow = (w & 0x0002) ? FLOW_CALL : FLOW_JUMP;
    it->target = (int64_t)((w >> 4) & 0x1F) << 17 | (int64_t)(w & 1) << 16 | it->op2;
  } else if ((w & 0xF800) == 0xF000) {
    // brbs, brbc: 7-bit relative
    int64_t k = (w >> 3) & 0x7F;
    if (k & 0x40) k -= 0x80;
    it->flow = FLOW_BRANCH;
    it->target = wrap(it->pc + 1 + k, flashWords);
  } else if ((w & 0xFC00) == 0x1000 || (w & 0xFC08) == 0xFC00 || (w & 0xFD00) == 0x9900) {
    // cpse, sbrc/sbrs, sbic/sbis
    it->flow = FLOW_SKIP;
  } else if (w == 0x9508) {
    it->flow = FLOW_RET;
  } else if (w == 0x9518) {
    it->flow = FLOW_RETI;
  } else if (w == 0x9409 || w == 0x9419) {
    it->flow = FLOW_INDIRECT_JUMP;
  } else if (w == 0x9509 || w == 0x9519) {
    it->flow = FLOW_INDIRECT_CALL;
  }
  // push/pop
  if ((w & 0xFE0F) == 0x920F) it->stack = 1;
  if ((w & 0xFE0F) == 0x900F) it->stack = -1;
  it->changesFlags = changesFlags(w);
  // in Rd, A
  it->readsSreg = (w & 0xF800) == 0xB000 && (((w >> 5) & 0x30) | (w & 0x0F)) == SREG_IO;
}

static int byAddress(const void *a, const void *b)
{
  int64_t x = ((const Item *)a)->pc, y = ((const Item*)b)->pc;
  return (x > y) - (x < y);
}

// the item that holds word address pc. -1 if none does.
static int32_t find(const Lint *L, int64_t pc)
{
  uint32_t lo = 0, hi = L->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const Item *it = &L->items[mid];
    if (pc < it->pc) hi = mid;
    else if (pc >= it->pc + it->words) lo = mid + 1;
    else return (int32_t)mid;
  }
  return -1;
}

// ---- warnings ----

// one warning at item i unless it has had one.
// for a line of a macro, its where the macro was called .
static void warn(Lint *L, uint32_t i, int kind, const char *fmt, ...) AVRASM_PRINTF(4, 5);
static void warn(Lint *L, uint32_t i, int kind, const char *fmt, ...)
{
  if (L->warned[i] & kind) return;
  L->warned[i] |= (uint8_t)kind;
  const OutlineCode *c = L->items[i].code;
  va_list ap;
  va_start(ap, fmt);
  diagEmitV(L->diags, DIAG_WARNING, c->head, fmt, ap);
  va_end(ap);
  if (c->callSite.file != FILE_NONE) diagEmit(L->diags, DIAG_INFO, c->callSite, "this line comes form the macro caller here");
}

static const char *skipName(uint16_t w)
{
  if ((w & 0xFC00) == 0x1000) return "cpse";
  if ((w & 0xFE08) == 0xFC00) return "sbrc";
  if ((w & 0xFE08) == 0xFE00) return "sbrs";
  return (w & 0x0200) ? "sbis" : "sbic";
}

// ---- the checks that look at one instruction ----

static void checkSkipAndRegister(Lint *L, uint32_t i)
{
  const Item *it = &L->items[i];
  if (it->isData) return;
  
  // lds or sts
  if (((it->op & 0xFE0F) == 0x9000 || (it->op & 0xFE0F) == 0x9200) && it->op2 < 0x20) {
    int isLoad = (it->op & 0x0200) == 0;
    warn(L, i, WARN_REGISTER, 
      "%s with address 0x%02X %s register r%u, not an I/O register: "
      "for an I/O register use %s, or add 0x20 to its address",
      isLoad ? "lds" : "sts", it->op2, isLoad ? "reads" : "writes", it->op2, isLoad ? "in" : "out"
    );
  }
  
  if (it->flow != FLOW_SKIP) return;
  int32_t j = find(L, it->pc + it->words);
  if (j < 0 || L->items[j].isData) return;
  const OutlineCode *next = L->items[j].code;
  // not the start of a macro call
  if (next->callSite.file == FILE_NONE || next->scope == it->code->scope) return;
  
  uint32_t n = 0;
  for (uint32_t k = 0; k < L->count; k++) n += !L->items[k].isData && L->items[k].code->scope == next->scope;
  if (n < 2 || (L->warned[i] & WARN_SKIP)) return;
  L->warned[i] |= WARN_SKIP;
  diagEmit(L->diags, DIAG_WARNING, next->callSite,
    "the %s before this macro call skips only its first instruction, the other %u still run%s "
    "(branch around the call instead)", skipName(it->op), n - 1, n == 2 ? "s" : "");
}

// ---- following the program ----

// where item i can go next (up to 2 addresses). with 'intoCalls', a call goes into the routine too.
static uint32_t successors(const Lint *L, uint32_t i, int intoCalls, int64_t next[3])
{
  const Item *it = &L->items[i];
  int64_t after = it->pc + it->words;
  uint32_t n = 0;
  switch (it->flow) {
    case FLOW_JUMP:
      next[n++] = it->target;
      break;
    case FLOW_CALL:
      if (intoCalls) next[n++] = it->target;
      next[n++] = after;
      break;
    case FLOW_BRANCH:
      next[n++] = after;
      next[n++] = it->target;
      break;
    case FLOW_SKIP: {
      next[n++] = after;
      int32_t j = find(L, after);
      next[n++] = after + (j >= 0 && !L->items[j].isData ? L->items[j].words : 1);
      break;
    }
    case FLOW_RET:
    case FLOW_RETI:
    case FLOW_INDIRECT_JUMP:
      break;
    default:
      next[n++] = after;
      break;
  }
  return n;
}

// checks if 'to' is where item i goes by not jumping rather than its target
static int fallsTo(const Item *it, int64_t to)
{
  return it->flow != FLOW_JUMP && (it->flow == FLOW_NEXT || it->flow == FLOW_INDIRECT_CALL || to != it->target);
}

// item i sends the processor to 'to'.
// returns the item there or -1 after a warning when no instruction starts there.
static int32_t arrive(Lint *L, uint32_t i, int64_t to)
{
  const Item *it = &L->items[i];
  int32_t j = find(L, to);
  int fall = fallsTo(it, to);
  if (j < 0) {
    if (fall) warn(L, i, WARN_FLOW, "execution runs on past this instruction into memory with no code: "
      "end the code with a jump (rjmp back to the main loop, for one)");
    else warn(L, i, WARN_FLOW, "this goes to word address 0x%04llX, where the program has no code", (long long)to);
    return -1;
  }
  if (L->items[j].isData) {
    warn(L, i, WARN_FLOW, "%s the data at word address 0x%04llX: the processor would run it as instructions",
      fall ? "execution runs on past this instruction into" : "this goes to", (long long)to);
    return -1;
  }
  if (L->items[j].pc != to) {
    warn(L, i, WARN_FLOW, "this goes to word address 0x%04llX, the second word of a two-word instruction", (long long)to);
    return -1;
  }
  return j;
}

// everything the reset code and the interrupt vectors can reach routines included.
// returns 1 if that includes an ijmp or icall whose targets cannot be known.
static int reach(Lint *L, const uint32_t *entries, uint32_t entryCount, uint8_t *reached)
{
  uint32_t *work = malloc(L->count * sizeof *work);
  uint32_t top = 0;
  int indirect = 0;
  for (uint32_t e = 0; e < entryCount; e++) {
    if (!reached[entries[e]]) work[top++] = entries[e];
    reached[entries[e]] = 1;
  }
  while (top > 0) {
    uint32_t i = work[--top];
    Flow f = L->items[i].flow;
    if (f == FLOW_INDIRECT_JUMP || f == FLOW_INDIRECT_CALL) indirect = 1;
    int64_t next[3];
    uint32_t n = successors(L, i, 1, next);
    for (uint32_t k = 0; k < n; k++) {
      int32_t j = arrive(L, i, next[k]);
      if (j >= 0 && !reached[j]) {
        reached[j] = 1;
        work[top++] = (uint32_t)j;
      }
    }
  }
  free(work);
  return indirect;
}

static void checkUnreachable(Lint *L, const uint8_t *reached)
{
  for (uint32_t i = 0; i < L->count; i++) {
    if (L->items[i].isData || reached[i]) continue;
    if (i > 0 && !L->items[i - 1].isData && !reached[i - 1] && L->items[i - 1].pc + L->items[i - 1].words == L->items[i].pc) {
      continue;
    }

    warn(L, i, WARN_UNREACHABLE, "nothing runs this code: no jump, call or branch goes here, "
      "and the code before it does not run on into it");
  }
}

// follows one routine, interrupt handler or the reset code from item 'start' without going into
// the routines it calls and counts what is pushed on each path.
static void walk(Lint *L, uint32_t start, Context context)
{
  // TODO: Depth-first from `start`, with a state per path: the item, the bytes pushed so far (depth), and whether SREG
  //       has been saved. Reset L->depth[] to INT32_MIN and L->saved[] to 0 first; the start has depth 0, saved bit 0.
  //       For each state taken off the list:
  //       - CONTEXT_INTERRUPT, not saved, and the item changes flags: WARN_SREG (once per walk), "this interrupt
  //         handler changes the status flags before it saves SREG, so the code it interrupted can take a wrong
  //         branch: save SREG first (in r16, SREG then push r16; restore it before reti)".
  //       - RET or RETI: in CONTEXT_RESET, WARN_RETURN "the reset code runs into this ret, which nothing called: it
  //         returns to a random address (end the main loop with a jump back)". Otherwise depth > 0: WARN_STACK
  //         "ret with 1 byte pushed and not popped: it returns to the wrong address"; depth < 0: "ret after popping
  //         2 bytes more than were pushed: ...". A ret (not reti) in CONTEXT_INTERRUPT also gets WARN_RETURN "an
  //         interrupt handler that ends with ret leaves interrupts off: use reti". A return ends the path.
  //       - Otherwise the new depth is depth + stack, saved is saved || readsSreg, and each successor (intoCalls = 0:
  //         a called routine is assumed to come back balanced; it is walked on its own) that is an instruction start:
  //         reached before with another depth: WARN_STACK at it, "the stack holds 1 byte more here on one path than
  //         on another: a push or pop is missing, or a loop pushes more than it pops", and do not go on; reached with
  //         this saved state already: nothing; otherwise record depth and the saved bit, and push it.
  for (uint32_t i = 0; i < L->count; i++) {
    L->depth[i] = INT32_MIN;
    L->saved[i] = 0;
  }

  typedef struct State {
    uint32_t item;
    int32_t depth;
    int saved;
  } State;
  
  // each item goes on at most twice, SREG saved or not
  State *work = malloc(2 * L->count * sizeof *work);
  uint32_t top = 0;
  work[top++] = (State){ start, 0, 0 };
  L->depth[start] = 0;
  L->saved[start] = 1;
  int sregWarned = 0;
  while (top > 0) {
    State s = work[--top];
    const Item *it = &L->items[s.item];
    
    if (context == CONTEXT_INTERRUPT && !s.saved && it->changesFlags && !sregWarned) {
      sregWarned = 1;
      warn(L, s.item, WARN_SREG, "this interrupt handler changes the status flags before it saves SREG, so the code it "
        "interrupted can take a wrong branch: save SREG first (in r16, SREG then push r16, restore it before reti)");
    }

    if (it->flow == FLOW_RET || it->flow == FLOW_RETI) {
      const char *name = it->flow == FLOW_RET ? "ret" : "reti";
      if (context == CONTEXT_RESET) {
        warn(L, s.item, WARN_RETURN, "the reset code runs into this %s, which nothing called: "
          "it returns to a random address (end the main loop with a jump back)", name);
      } else if (s.depth > 0) {
        warn(L, s.item, WARN_STACK, "%s with %d byte%s pushed and not popped: "
          "it returns to the wrong address", name, s.depth, s.depth == 1 ? "" : "s");
      } else if (s.depth < 0) {
        warn(L, s.item, WARN_STACK, "%s after popping %d byte%s more than were pushed: "
          "it returns to the wrong address", name, -s.depth, s.depth == -1 ? "" : "s");
      }
      if (context == CONTEXT_INTERRUPT && it->flow == FLOW_RET) {
        warn(L, s.item, WARN_RETURN, "an interrupt handler that ends with ret leaves interrupts off: use reti");
      }
      continue;
    }

    int32_t depth = s.depth + it->stack;
    int saved = s.saved || it->readsSreg;
    int64_t next[3];
    uint32_t n = successors(L, s.item, 0, next);

    for (uint32_t k = 0; k < n; k++) {
      int32_t j = find(L, next[k]);

      if (j < 0 || L->items[j].isData || L->items[j].pc != next[k]) continue;
      
      if (L->depth[j] != INT32_MIN && L->depth[j] != depth) {
        warn(L, (uint32_t)j, WARN_STACK, "the stack holds %d byte%s more here on one path than on another: "
        "a push or pop is missing, or a loop pushes more than it pops", 
        abs(depth - L->depth[j]), abs(depth - L->depth[j]) == 1 ? "" : "s");
        continue;
      }
      if (L->saved[j] & (1 << saved)) continue;
      
      L->depth[j] = depth;
      L->saved[j] |= (uint8_t)(1 << saved);
      work[top++] = (State){ (uint32_t)j, depth, saved };
    }
  }
  free(work);
}

// ---- 'the glue' (because its putting it all together yk) ----

static int fallsInto(const Item *before, int64_t pc)
{
  return before->pc + before->words == pc && before->flow != FLOW_JUMP && before->flow != FLOW_RET 
    && before->flow != FLOW_RETI && before->flow != FLOW_INDIRECT_JUMP;
}

void lintProgram(DiagList *diags, const Outline *outline, const Image *flash)
{
  Lint L = { diags, NULL, 0, NULL, NULL, NULL };
  const OutlineName *flashEnd = outlineFind(outline, "FLASHEND", 8);
  int64_t flashWords = flashEnd != NULL && flashEnd->kind == OUTLINE_EQU ? flashEnd->value + 1 : 0;
  L.items = calloc(outline->codeCount + 1, sizeof *L.items);

  for (uint32_t i = 0; i < outline->codeCount; i++) {
    const OutlineCode *c = &outline->code[i];
    if ((uint64_t)(c->pc + c->words) * 2 > flash->size || !flash->used[c->pc * 2]) continue;
    
    Item *it = &L.items[L.count++];
    it->code = c;
    it->pc = c->pc;
    it->words = c->words;
    it->isData = c->isData;
    
    if (!it->isData) decode(it, flash, flashWords);
  }

  qsort(L.items, L.count, sizeof *L.items, byAddress);
  
  L.warned = calloc(L.count + 1, 1);
  L.depth = malloc((L.count + 1) * sizeof *L.depth);
  L.saved = malloc(L.count + 1);

  for (uint32_t i = 0; i < L.count; i++) checkSkipAndRegister(&L, i);

  // where the processor starts, the reset address and the interrupt vectors when the program has a
  // table of them (a device file usually says how big it is and the program jumps over it at address 0).
  int32_t reset = find(&L, 0);
  if (reset >= 0 && (L.items[reset].isData || L.items[reset].pc != 0)) reset = -1;
  
  uint32_t *entries = malloc((L.count + 1) * sizeof *entries);
  uint32_t entryCount = 0, handlerStart = 0;

  if (reset >= 0) {
    entries[entryCount++] = (uint32_t)reset;
    handlerStart = entryCount;
    const OutlineName *size = outlineFind(outline, "INT_VECTORS_SIZE", 16);
    
    if (size != NULL && size->kind == OUTLINE_EQU && L.items[reset].flow == FLOW_JUMP) {
      for (uint32_t i = (uint32_t)reset + 1; i < L.count && L.items[i].pc < size->value; i++) {
        const Item *it = &L.items[i];
        if (!it->isData && (it->flow == FLOW_JUMP || it->flow == FLOW_RETI) && !fallsInto(&L.items[i - 1], it->pc)) {
          entries[entryCount++] = i;
        }
      }
    }
  }
  uint32_t handlerEnd = entryCount;

  if (reset >= 0) {
    uint8_t *reached = calloc(L.count + 1, 1);
    int indirect = reach(&L, entries, entryCount, reached);
    if (!indirect) checkUnreachable(&L, reached);

    uint8_t *isRoutine = calloc(L.count + 1, 1);
    for (uint32_t i = 0; i < L.count; i++) {
      if (!reached[i] || L.items[i].flow != FLOW_CALL) continue;

      int32_t j = find(&L, L.items[i].target);
      if (j >= 0 && !L.items[j].isData && L.items[j].pc == L.items[i].target) isRoutine[j] = 1;
    }

    walk(&L, (uint32_t)reset, CONTEXT_RESET);
    
    for (uint32_t e = handlerStart; e < handlerEnd; e++) walk(&L, entries[e], CONTEXT_INTERRUPT);
    
    for (uint32_t i = 0; i < L.count; i++) {
      if (isRoutine[i]) walk(&L, i, CONTEXT_ROUTINE);
    }
    
    free(isRoutine);
    free(reached);
  }
  
  free(entries);
  free(L.items);
  free(L.warned);
  free(L.depth);
  free(L.saved);
}