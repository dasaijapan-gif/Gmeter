// Runs the behaviour tests (TEST) and the reference-output cases
// (golden_register) and compares each case's digest with the recorded one.
//
//   gm_tests [filter]                 everything whose name contains filter
//   gm_tests --record [filter]        rewrite the recorded digests (all, or the matching cases)
//   gm_tests --dump <case> [--against <file>]
//                                     per-step digests of one case; with --against, compare
//                                     them with an earlier dump and name the first step that differs
//   gm_tests --list                   names of the tests and cases
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"

// ---- registries ---------------------------------------------------------------------------
#define MAX_TESTS 256
#define MAX_CASES 512
#define CASE_NAME 48

typedef struct {
  const char* name;
  test_fn_t fn;
  const char* file;
  int line;
} test_t;

typedef struct {
  char name[CASE_NAME];
  golden_fn_t fn;
  int arg;
} case_t;

static test_t s_tests[MAX_TESTS];
static int s_test_count;
static case_t s_cases[MAX_CASES];
static int s_case_count;

void test_register(const char* name, test_fn_t fn, const char* file, int line) {
  if (s_test_count == MAX_TESTS) {
    fprintf(stderr, "too many tests (MAX_TESTS)\n");
    exit(2);
  }
  s_tests[s_test_count++] = (test_t){name, fn, file, line};
}

void golden_register(const char* name, golden_fn_t fn, int arg) {
  if (s_case_count == MAX_CASES || strlen(name) >= CASE_NAME) {
    fprintf(stderr, "cannot register case %s (MAX_CASES or CASE_NAME)\n", name);
    exit(2);
  }
  case_t* c = &s_cases[s_case_count++];
  snprintf(c->name, sizeof(c->name), "%s", name);
  c->fn = fn;
  c->arg = arg;
}

// Constructors run in an order the toolchain picks: list the tests by file,
// then by line.
static int by_file_line(const void* a, const void* b) {
  const test_t* x = (const test_t*)a;
  const test_t* y = (const test_t*)b;
  const int c = strcmp(x->file, y->file);
  return c ? c : x->line - y->line;
}

// ---- checks -------------------------------------------------------------------------------
static int s_checks, s_failures, s_notes;
static const char* s_current = "";

void test_check(bool ok, const char* what, const char* file, int line) {
  ++s_checks;
  if (!ok) {
    ++s_failures;
    printf("  FAIL [%s] %s:%d: %s\n", s_current, file, line, what);
  }
}

void test_note(const char* fmt, ...) {
  if (++s_notes > 200) return;  // keep a broken run readable
  printf("  note [%s] ", s_current);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
}

// ---- digests ------------------------------------------------------------------------------
#define FNV_OFFSET 0xCBF29CE484222325ull

void gd_step(gd_t* g, const char* what) {
  if (g->dump) fprintf(g->dump, "step %ld %016llx %s\n", g->step, (unsigned long long)g->h, what);
  if (g->ref && g->first_diff < 0 && (g->step >= g->ref_count || g->ref[g->step] != g->h)) g->first_diff = g->step;
  ++g->step;
}

void gd_event(gd_t* g, const char* fmt, ...) {
  if (!g->dump) return;
  fprintf(g->dump, "  ");
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g->dump, fmt, ap);
  va_end(ap);
  fprintf(g->dump, "\n");
}

static uint64_t run_case(const case_t* c, FILE* dump, const uint64_t* ref, long ref_count, long* steps,
                         long* first_diff) {
  gd_t g = {FNV_OFFSET, 0, dump, ref, ref_count, -1};
  c->fn(&g, c->arg);
  if (ref && g.first_diff < 0 && g.step != ref_count) g.first_diff = g.step < ref_count ? g.step : ref_count;
  if (steps) *steps = g.step;
  if (first_diff) *first_diff = g.first_diff;
  return g.h;
}

// ---- the recorded file --------------------------------------------------------------------
typedef struct {
  char name[CASE_NAME];
  uint64_t digest;
} entry_t;

static entry_t s_recorded[MAX_CASES];
static int s_recorded_count;

static void load_recorded(void) {
  FILE* f = fopen(GM_GOLDEN_FILE, "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char name[CASE_NAME];
    unsigned long long d;
    if (sscanf(line, "%47s %llx", name, &d) != 2 || s_recorded_count == MAX_CASES) continue;
    snprintf(s_recorded[s_recorded_count].name, CASE_NAME, "%s", name);
    s_recorded[s_recorded_count++].digest = d;
  }
  fclose(f);
}

static const entry_t* recorded(const char* name) {
  for (int i = 0; i < s_recorded_count; ++i)
    if (!strcmp(s_recorded[i].name, name)) return &s_recorded[i];
  return NULL;
}

static const case_t* find_case(const char* name) {
  for (int i = 0; i < s_case_count; ++i)
    if (!strcmp(s_cases[i].name, name)) return &s_cases[i];
  return NULL;
}

static bool matches(const char* name, const char* filter) { return !filter || strstr(name, filter); }

static double seconds_since(clock_t t0) { return (double)(clock() - t0) / CLOCKS_PER_SEC; }

static int record(const char* filter) {
  load_recorded();
  static uint64_t digest[MAX_CASES];
  static bool have[MAX_CASES];
  int ran = 0;
  for (int i = 0; i < s_case_count; ++i) {
    const entry_t* old = recorded(s_cases[i].name);
    if (matches(s_cases[i].name, filter)) {
      digest[i] = run_case(&s_cases[i], NULL, NULL, 0, NULL, NULL);
      have[i] = true;
      ++ran;
      if (!old || old->digest != digest[i]) printf("%s %s\n", old ? "changed" : "new", s_cases[i].name);
    } else if (old) {
      digest[i] = old->digest;
      have[i] = true;
    }
  }
  if (filter && !ran) {
    fprintf(stderr, "no case matches %s (--list)\n", filter);
    return 2;
  }
  FILE* f = fopen(GM_GOLDEN_FILE, "w");
  if (!f) {
    perror(GM_GOLDEN_FILE);
    return 2;
  }
  fprintf(f, "# Reference digests of the gm_tests cases at GM_PANEL_H=%d (idf/test/README.md).\n", GM_PANEL_H);
  fprintf(f, "# Written by gm_tests --record: every re-record must be justified in the commit message.\n");
  for (int i = 0; i < s_case_count; ++i)
    if (have[i]) fprintf(f, "%s %016llx\n", s_cases[i].name, (unsigned long long)digest[i]);
  fclose(f);
  printf("recorded %d case(s) into %s\n", ran, GM_GOLDEN_FILE);
  return 0;
}

// Reads the "step <n> <digest>" lines of an earlier --dump.
static uint64_t* load_dump(const char* path, long* count) {
  FILE* f = fopen(path, "r");
  if (!f) {
    perror(path);
    return NULL;
  }
  long cap = 1024, n = 0;
  uint64_t* d = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)cap);
  char line[512];
  while (d && fgets(line, sizeof(line), f)) {
    long step;
    unsigned long long h;
    if (sscanf(line, "step %ld %llx", &step, &h) != 2 || step != n) continue;
    if (n == cap) {
      cap *= 2;
      uint64_t* bigger = (uint64_t*)realloc(d, sizeof(uint64_t) * (size_t)cap);
      if (!bigger) free(d);
      d = bigger;
      if (!d) break;
    }
    d[n++] = h;
  }
  fclose(f);
  *count = n;
  return d;
}

static int dump(const char* name, const char* against) {
  const case_t* c = find_case(name);
  if (!c) {
    fprintf(stderr, "no case named %s (--list)\n", name);
    return 2;
  }
  if (!against) {
    printf("# %s: running digest after each step (GM_PANEL_H=%d)\n", name, GM_PANEL_H);
    const uint64_t h = run_case(c, stdout, NULL, 0, NULL, NULL);
    printf("digest %016llx\n", (unsigned long long)h);
    return 0;
  }
  long ref_count = 0;
  uint64_t* ref = load_dump(against, &ref_count);
  if (!ref) return 2;
  long steps = 0, first = -1;
  run_case(c, NULL, ref, ref_count, &steps, &first);
  free(ref);
  if (first < 0) {
    printf("%s: all %ld steps match %s\n", name, steps, against);
    return 0;
  }
  printf("%s: first step that differs from %s: %ld (of %ld here, %ld there); see --dump %s for what it does\n", name,
         against, first, steps, ref_count, name);
  return 1;
}

int main(int argc, char** argv) {
  qsort(s_tests, (size_t)s_test_count, sizeof(s_tests[0]), by_file_line);
  const char* filter = NULL;
  if (argc > 1 && !strcmp(argv[1], "--list")) {
    for (int i = 0; i < s_test_count; ++i) printf("test %s\n", s_tests[i].name);
    for (int i = 0; i < s_case_count; ++i) printf("case %s\n", s_cases[i].name);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--record")) return record(argc > 2 ? argv[2] : NULL);
  if (argc > 2 && !strcmp(argv[1], "--dump"))
    return dump(argv[2], argc > 4 && !strcmp(argv[3], "--against") ? argv[4] : NULL);
  if (argc > 1 && argv[1][0] == '-') {
    fprintf(stderr, "usage: gm_tests [filter] | --record [filter] | --dump <case> [--against <file>] | --list\n");
    return 2;
  }
  if (argc > 1) filter = argv[1];

  const clock_t t0 = clock();
  int run = 0;
  for (int i = 0; i < s_test_count; ++i) {
    if (!matches(s_tests[i].name, filter)) continue;
    s_current = s_tests[i].name;
    const int before = s_failures;
    s_tests[i].fn();
    ++run;
    printf("%s %s\n", s_failures == before ? "PASS" : "FAIL", s_tests[i].name);
  }
  const double t_tests = seconds_since(t0);

  load_recorded();
  int cases = 0, mismatches = 0;
  for (int i = 0; i < s_case_count; ++i) {
    const case_t* c = &s_cases[i];
    if (!matches(c->name, filter)) continue;
    ++cases;
    const uint64_t h = run_case(c, NULL, NULL, 0, NULL, NULL);
    const entry_t* want = recorded(c->name);
    if (want && want->digest == h) {
      printf("PASS %s\n", c->name);
      continue;
    }
    ++mismatches;
    if (want)
      printf("FAIL %s: digest %016llx, recorded %016llx (find the step: gm_tests --dump %s, README.md)\n", c->name,
             (unsigned long long)h, (unsigned long long)want->digest, c->name);
    else
      printf("FAIL %s: digest %016llx, nothing recorded (gm_tests --record %s)\n", c->name, (unsigned long long)h,
             c->name);
  }
  // A recorded case that no longer exists means the file is out of date.
  for (int i = 0; i < s_recorded_count; ++i)
    if (matches(s_recorded[i].name, filter) && !find_case(s_recorded[i].name)) {
      ++mismatches;
      printf("FAIL %s: recorded, but there is no such case (gm_tests --record)\n", s_recorded[i].name);
    }

  if (filter && !run && !cases && !mismatches) {  // a mistyped filter must not pass
    fprintf(stderr, "no test or case matches %s (--list)\n", filter);
    return 2;
  }
  printf("\n%d tests, %d checks, %d failures; %d reference cases, %d mismatches (GM_PANEL_H=%d, %.1f s + %.1f s)\n",
         run, s_checks, s_failures, cases, mismatches, GM_PANEL_H, t_tests, seconds_since(t0) - t_tests);
  return s_failures || mismatches ? 1 : 0;
}
