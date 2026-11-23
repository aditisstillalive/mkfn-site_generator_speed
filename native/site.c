// site.c: makefun-site 1.0.0's build in C, as a Node-API addon (site.node) that solve.js loads. build(cases, slugify)
// reads /input/cases.json's bytes as they are, runs every case's site through makefun-site's pipeline (front matter,
// permalinks, Markdown, collections, tags, layouts, report) and returns, per case, the case's answer already written as
// JSON (a Buffer), or null where the case holds something this port leaves to solve.js's run() (see `unsupported`).
//
// Text is UTF-8 throughout. Every rule below compares and cuts on ASCII bytes only, which gives the same result on
// UTF-8 as makefun-site's JavaScript gives on UTF-16 for every character of the Basic Multilingual Plane outside the
// whitespace classes; a case holding anything else is handed back (null). A slug of text outside ASCII (NFKD, marks
// dropped) is the one rule asked of JavaScript: `slugify`, solve.js's own, is called for it.
//
// Freestanding: no libc. Memory comes from mmap; every case's work lives in arenas reset per case, its answer in an
// output arena kept to the end; anything a case cannot fit or this port does not take jumps back (longjmp) and that
// case's answer is null.

#define NAPI_VERSION 8
#include <node_api.h>
#include <stddef.h>
#include <stdint.h>
#include <emmintrin.h>

typedef unsigned char u8;
typedef struct { const char *p; long n; } Str;
#define LIT(s) ((Str){ s, sizeof(s) - 1 })
#define HIDDEN __attribute__((visibility("hidden")))

// ---------------------------------------------------------------- what libc would give

HIDDEN void *memcpy(void *d, const void *s, size_t n) {
  void *r = d;
  if (n < 128) {
    // short copies (most of them): 16-byte moves, the last one overlapping
    unsigned char *dd = d;
    const unsigned char *ss = s;
    if (n >= 16) {
      size_t i = 0;
      for (; i + 16 <= n; i += 16) _mm_storeu_si128((__m128i *)(dd + i), _mm_loadu_si128((const __m128i *)(ss + i)));
      if (i < n) _mm_storeu_si128((__m128i *)(dd + n - 16), _mm_loadu_si128((const __m128i *)(ss + n - 16)));
    } else if (n >= 8) {
      uint64_t a, b;
      __builtin_memcpy(&a, ss, 8);
      __builtin_memcpy(&b, ss + n - 8, 8);
      __builtin_memcpy(dd, &a, 8);
      __builtin_memcpy(dd + n - 8, &b, 8);
    } else if (n >= 4) {
      uint32_t a, b;
      __builtin_memcpy(&a, ss, 4);
      __builtin_memcpy(&b, ss + n - 4, 4);
      __builtin_memcpy(dd, &a, 4);
      __builtin_memcpy(dd + n - 4, &b, 4);
    } else
      for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return r;
  }
  {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
      __m128i a = _mm_loadu_si128((const __m128i *)(ss + i)), b = _mm_loadu_si128((const __m128i *)(ss + i + 16)),
              c = _mm_loadu_si128((const __m128i *)(ss + i + 32)), e = _mm_loadu_si128((const __m128i *)(ss + i + 48));
      _mm_storeu_si128((__m128i *)(dd + i), a);
      _mm_storeu_si128((__m128i *)(dd + i + 16), b);
      _mm_storeu_si128((__m128i *)(dd + i + 32), c);
      _mm_storeu_si128((__m128i *)(dd + i + 48), e);
    }
    for (; i + 16 <= n; i += 16) _mm_storeu_si128((__m128i *)(dd + i), _mm_loadu_si128((const __m128i *)(ss + i)));
    if (i < n) _mm_storeu_si128((__m128i *)(dd + n - 16), _mm_loadu_si128((const __m128i *)(ss + n - 16)));
  }
  return r;
}
HIDDEN void *memmove(void *d, const void *s, size_t n) {
  u8 *dd = d;
  const u8 *ss = s;
  if (dd <= ss || dd >= ss + n) return memcpy(d, s, n);
  while (n--) dd[n] = ss[n];
  return d;
}
HIDDEN void *memset(void *d, int c, size_t n) {
  void *r = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return r;
}
HIDDEN int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    int m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(x + i)), _mm_loadu_si128((const __m128i *)(y + i))));
    if (m != 0xFFFF) {
      int k = __builtin_ctz(~m & 0xFFFF);
      return x[i + k] < y[i + k] ? -1 : 1;
    }
  }
  for (; i < n; i++)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

static void *sys_mmap(size_t size) {
  long r;
  register long r10 __asm__("r10") = 0x22 | 0x4000; // MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
  register long r8 __asm__("r8") = -1;
  register long r9 __asm__("r9") = 0;
  __asm__ volatile("syscall" : "=a"(r) : "a"(9), "D"(0), "S"(size), "d"(3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return r < 0 && r > -4096 ? 0 : (void *)r;
}

#ifdef PROF
static unsigned long peak[3];
static unsigned long long prof_t[16], prof_last;
static inline unsigned long long rdtsc(void) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
#define PHASE(k) do { unsigned long long _t = rdtsc(); prof_t[k] += _t - prof_last; prof_last = _t; } while (0)
#else
#define PHASE(k) do {} while (0)
#endif

// ---------------------------------------------------------------- failure: the case goes back to JavaScript

static void *jb[5];
__attribute__((noreturn, noinline)) static void unsupported(void) { __builtin_longjmp(jb, 1); }

// ---------------------------------------------------------------- arenas and streams

typedef struct { char *base, *cur, *end; } Arena;
static Arena A; // a case's structures and strings
static Arena B; // a case's Markdown HTML, written as a stream
static Arena O; // the answers, written as a stream, kept

static long sys3(long n, long a, long b, long c) {
  long r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
  return r;
}
static int arena_init(Arena *a, size_t size) {
  const size_t huge = (size_t)1 << 21;
  char *m = sys_mmap(size + huge);
  if (m == 0) return 0;
  a->base = a->cur = (char *)(((uintptr_t)m + huge - 1) & ~(uintptr_t)(huge - 1));
  a->end = a->base + size;
  sys3(28, (long)a->base, (long)size, 14); // madvise(MADV_HUGEPAGE): fewer faults on first touch, where the kernel allows
  return 1;
}
/** All of p written to fd (write(2), again after a short write or EINTR). */
static int write_all(long fd, const char *p, long n) {
  while (n > 0) {
    long r = sys3(1, fd, (long)p, n);
    if (r == -4) continue;
    if (r <= 0) return 0;
    p += r;
    n -= r;
  }
  return 1;
}
static inline void *alloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  char *p = A.cur;
  if ((size_t)(A.end - p) < n) unsupported();
  A.cur = p + n;
  return p;
}
static inline void *allocz(size_t n) {
  void *p = alloc(n);
  memset(p, 0, n);
  return p;
}
static inline void need(Arena *o, size_t n) {
  if ((size_t)(o->end - o->cur) < n) unsupported();
}
/** A copy tuned for the short pieces the streams take (overlapping word moves), rep movsb past 32 bytes. */
static inline void cpy(char *d, const char *s, size_t n) {
  if (n <= 16) {
    if (n >= 8) {
      uint64_t a, b;
      __builtin_memcpy(&a, s, 8);
      __builtin_memcpy(&b, s + n - 8, 8);
      __builtin_memcpy(d, &a, 8);
      __builtin_memcpy(d + n - 8, &b, 8);
    } else if (n >= 4) {
      uint32_t a, b;
      __builtin_memcpy(&a, s, 4);
      __builtin_memcpy(&b, s + n - 4, 4);
      __builtin_memcpy(d, &a, 4);
      __builtin_memcpy(d + n - 4, &b, 4);
    } else
      for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else if (n <= 32) {
    __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)(s + n - 16));
    _mm_storeu_si128((__m128i *)d, a);
    _mm_storeu_si128((__m128i *)(d + n - 16), b);
  } else
    memcpy(d, s, n);
}
static inline void put(Arena *o, const char *p, size_t n) {
  need(o, n);
  cpy(o->cur, p, n);
  o->cur += n;
}
static inline void puts_(Arena *o, Str s) { put(o, s.p, s.n); }
static inline void putch(Arena *o, char c) {
  need(o, 1);
  *o->cur++ = c;
}
#define PUTL(o, s) put(o, s, sizeof(s) - 1)
static void putnum(Arena *o, long v) {
  char tmp[24];
  int k = 0;
  int neg = v < 0;
  unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
  do tmp[k++] = '0' + u % 10; while ((u /= 10) != 0);
  if (neg) tmp[k++] = '-';
  need(o, k);
  while (k > 0) *o->cur++ = tmp[--k];
}

// ---------------------------------------------------------------- strings

static inline int str_eq(Str a, Str b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }
static inline int str_eqc(Str a, const char *c, long n) { return a.n == n && memcmp(a.p, c, n) == 0; }
#define EQL(a, s) str_eqc(a, s, sizeof(s) - 1)
/** Code-unit order: on UTF-8 of the Basic Multilingual Plane, byte order. */
static int str_cmp(Str a, Str b) {
  long n = a.n < b.n ? a.n : b.n;
  int c = memcmp(a.p, b.p, n);
  if (c != 0) return c;
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}
static inline int starts(Str s, const char *c, long n) { return s.n >= n && memcmp(s.p, c, n) == 0; }
#define STARTS(s, c) starts(s, c, sizeof(c) - 1)
static inline int ends(Str s, const char *c, long n) { return s.n >= n && memcmp(s.p + s.n - n, c, n) == 0; }
static inline Str sub(Str s, long from, long to) { return (Str){ s.p + from, to - from }; }
static inline long find_ch(Str s, char c, long from) {
  if (from < 0) from = 0;
  long i = from;
  const __m128i cc = _mm_set1_epi8(c);
  for (; i + 16 <= s.n; i += 16) {
    int m = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s.p + i)), cc));
    if (m) return i + __builtin_ctz(m);
  }
  for (; i < s.n; i++)
    if (s.p[i] == c) return i;
  return -1;
}
static long find_str(Str s, const char *w, long wn, long from) {
  for (long i = from; i + wn <= s.n; i++)
    if (s.p[i] == w[0] && memcmp(s.p + i, w, wn) == 0) return i;
  return -1;
}
static Str cat(int k, const Str *parts) {
  long n = 0;
  for (int i = 0; i < k; i++) n += parts[i].n;
  char *p = alloc(n + 1);
  long at = 0;
  for (int i = 0; i < k; i++) {
    memcpy(p + at, parts[i].p, parts[i].n);
    at += parts[i].n;
  }
  return (Str){ p, n };
}
#define CAT(...) cat(sizeof((Str[]){ __VA_ARGS__ }) / sizeof(Str), (Str[]){ __VA_ARGS__ })
static Str num_str(long v) {
  char *p = alloc(24);
  char tmp[24];
  int k = 0;
  do tmp[k++] = '0' + v % 10; while ((v /= 10) != 0);
  int n = 0;
  while (k > 0) p[n++] = tmp[--k];
  return (Str){ p, n };
}
/** JavaScript's whitespace of \s and trim(), the ASCII ones (a case holding any other is not taken). */
static inline int js_ws(u8 c) { return c == ' ' || (c >= 9 && c <= 13); }
static Str js_trim(Str s) {
  long a = 0, b = s.n;
  while (a < b && js_ws(s.p[a])) a++;
  while (b > a && js_ws(s.p[b - 1])) b--;
  return sub(s, a, b);
}
static inline int sp_tab(char c) { return c == ' ' || c == '\t'; }
static int all_sp_tab(Str s) {
  for (long i = 0; i < s.n; i++)
    if (!sp_tab(s.p[i])) return 0;
  return 1;
}
static Str trim_sp_tab(Str s) {
  long a = 0, b = s.n;
  while (a < b && sp_tab(s.p[a])) a++;
  while (b > a && sp_tab(s.p[b - 1])) b--;
  return sub(s, a, b);
}
static int has_js_ws(Str s) {
  for (long i = 0; i < s.n; i++)
    if (js_ws(s.p[i])) return 1;
  return 0;
}

// ---------------------------------------------------------------- a hash map of strings

typedef struct { Str *k; void **v; uint32_t cap, n; } Map;
static inline uint32_t hash(Str s) {
  uint32_t h = 2166136261u;
  for (long i = 0; i < s.n; i++) h = (h ^ (u8)s.p[i]) * 16777619u;
  return h;
}
static void map_init(Map *m, uint32_t hint) {
  uint32_t cap = 16;
  while (cap < hint * 2) cap <<= 1;
  m->cap = cap;
  m->n = 0;
  m->k = allocz(cap * sizeof(Str));
  m->v = allocz(cap * sizeof(void *));
}
static void **map_get(Map *m, Str key) {
  uint32_t mask = m->cap - 1;
  for (uint32_t i = hash(key) & mask;; i = (i + 1) & mask) {
    if (m->k[i].p == 0) return 0;
    if (str_eq(m->k[i], key)) return &m->v[i];
  }
}
static void **map_put(Map *m, Str key, int *isnew);
static void map_grow(Map *m) {
  Map g;
  map_init(&g, m->cap);
  for (uint32_t i = 0; i < m->cap; i++)
    if (m->k[i].p != 0) {
      int nw;
      *map_put(&g, m->k[i], &nw) = m->v[i];
    }
  *m = g;
}
static void **map_put(Map *m, Str key, int *isnew) {
  if ((m->n + 1) * 2 > m->cap) map_grow(m);
  uint32_t mask = m->cap - 1;
  if (key.p == 0) key.p = "";
  for (uint32_t i = hash(key) & mask;; i = (i + 1) & mask) {
    if (m->k[i].p == 0) {
      m->k[i] = key;
      m->n++;
      *isnew = 1;
      return &m->v[i];
    }
    if (str_eq(m->k[i], key)) {
      *isnew = 0;
      return &m->v[i];
    }
  }
}
static inline int map_has(Map *m, Str key) { return map_get(m, key) != 0; }
static inline void set_add(Map *m, Str key) {
  int nw;
  map_put(m, key, &nw);
}

// ---------------------------------------------------------------- a stable sort of pointers

typedef int (*Cmp)(const void *, const void *);
static void msort(void **a, long n, Cmp cmp) {
  if (n < 2) return;
  void **tmp = alloc(n * sizeof(void *));
  for (long w = 1; w < n; w *= 2) {
    for (long lo = 0; lo < n; lo += 2 * w) {
      long mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
      long i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tmp[k++] = cmp(a[j], a[i]) < 0 ? a[j++] : a[i++];
      while (i < mid) tmp[k++] = a[i++];
      while (j < hi) tmp[k++] = a[j++];
    }
    memcpy(a, tmp, n * sizeof(void *));
  }
}

// ---------------------------------------------------------------- JSON in

typedef struct JV {
  u8 t, isint;
  long num;
  Str s;
  long n;
  struct JV *kids; // array items or object values
  Str *keys;
  struct JV *next;
} JV;
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
static const u8 *jp, *je;

static inline void jws(void) {
  while (jp < je && (*jp == ' ' || *jp == '\n' || *jp == '\r' || *jp == '\t')) jp++;
}
/** A character outside ASCII that the port does not take: outside the Basic Multilingual Plane, or whitespace or a
 * line terminator to JavaScript (U+00A0, U+1680, U+2000-200A, U+2028, U+2029, U+202F, U+205F, U+3000, U+FEFF). */
static inline void check_high(const u8 *q) {
  u8 c = q[0];
  if (c >= 0xF0) unsupported();
  if (c == 0xC2) {
    if (q[1] == 0xA0) unsupported();
  } else if (c == 0xE1) {
    if (q[1] == 0x9A && q[2] == 0x80) unsupported();
  } else if (c == 0xE2) {
    if (q[1] == 0x80 && ((q[2] >= 0x80 && q[2] <= 0x8A) || q[2] == 0xA8 || q[2] == 0xA9 || q[2] == 0xAF)) unsupported();
    if (q[1] == 0x81 && q[2] == 0x9F) unsupported();
  } else if (c == 0xE3) {
    if (q[1] == 0x80 && q[2] == 0x80) unsupported();
  } else if (c == 0xEF) {
    if (q[1] == 0xBB && q[2] == 0xBF) unsupported();
  }
}
static int hexv(u8 c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  unsupported();
}
/** From p (at most to e): the first byte that is ", \\ or past ASCII. */
static inline const u8 *scan_jstr(const u8 *p, const u8 *e) {
  const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\');
  while (p + 16 <= e) {
    __m128i x = _mm_loadu_si128((const __m128i *)p);
    int m = _mm_movemask_epi8(_mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, q), _mm_cmpeq_epi8(x, bs)), x));
    if (m) return p + __builtin_ctz(m);
    p += 16;
  }
  while (p < e && *p != '"' && *p != '\\' && *p < 0x80) p++;
  return p;
}
static Str jstr(void) {
  jp++;
  const u8 *st = jp;
  for (;;) {
    jp = scan_jstr(jp, je);
    if (jp >= je) unsupported();
    u8 c = *jp;
    if (c == '"') {
      Str r = { (const char *)st, jp - st };
      jp++;
      return r;
    }
    if (c == '\\') break;
    check_high(jp);
    jp++;
  }
  // Escapes: decoded into what is left of the arena, which is then cut to the string's length.
  char *out = A.cur;
  long room = A.end - A.cur;
  long n = jp - st;
  if (n > room) unsupported();
  memcpy(out, st, n);
  for (;;) {
    const u8 *r = scan_jstr(jp, je);
    if (r >= je) unsupported();
    long k = r - jp;
    if (n + k + 8 > room) unsupported();
    cpy(out + n, (const char *)jp, k);
    n += k;
    jp = r;
    u8 c = *jp;
    if (c == '"') {
      jp++;
      break;
    }
    if (c >= 0x80) {
      check_high(jp);
      out[n++] = c;
      jp++;
      continue;
    }
    if (jp + 1 >= je) unsupported();
    u8 e = jp[1];
    jp += 2;
    switch (e) {
      case '"': out[n++] = '"'; break;
      case '\\': out[n++] = '\\'; break;
      case '/': out[n++] = '/'; break;
      case 'b': out[n++] = '\b'; break;
      case 'f': out[n++] = '\f'; break;
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case 'u': {
        if (je - jp < 4) unsupported();
        int v = (hexv(jp[0]) << 12) | (hexv(jp[1]) << 8) | (hexv(jp[2]) << 4) | hexv(jp[3]);
        if (v >= 0x80) unsupported();
        out[n++] = (char)v;
        jp += 4;
        break;
      }
      default: unsupported();
    }
  }
  A.cur = out + ((n + 8) & ~7L);
  return (Str){ out, n };
}
static JV *jval(int depth) {
  if (depth > 64) unsupported();
  jws();
  if (jp >= je) unsupported();
  JV *v = alloc(sizeof(JV));
  v->next = 0;
  u8 c = *jp;
  if (c == '"') {
    v->t = J_STR;
    v->s = jstr();
  } else if (c == '{' || c == '[') {
    int obj = c == '{';
    jp++;
    v->t = obj ? J_OBJ : J_ARR;
    JV *head = 0, **tail = &head;
    Str *kbuf = 0;
    long n = 0, kcap = 0;
    jws();
    if (jp < je && *jp == (obj ? '}' : ']')) jp++;
    else
      for (;;) {
        Str key = { 0, 0 };
        if (obj) {
          jws();
          if (jp >= je || *jp != '"') unsupported();
          key = jstr();
          jws();
          if (jp >= je || *jp != ':') unsupported();
          jp++;
        }
        JV *k = jval(depth + 1);
        if (obj) {
          if (n == kcap) {
            long nc = kcap ? kcap * 2 : 8;
            Str *nb = alloc(nc * sizeof(Str));
            if (n) memcpy(nb, kbuf, n * sizeof(Str));
            kbuf = nb;
            kcap = nc;
          }
          kbuf[n] = key;
        }
        *tail = k;
        tail = &k->next;
        n++;
        jws();
        if (jp >= je) unsupported();
        if (*jp == ',') {
          jp++;
          continue;
        }
        if (*jp == (obj ? '}' : ']')) {
          jp++;
          break;
        }
        unsupported();
      }
    v->n = n;
    v->kids = alloc((n ? n : 1) * sizeof(JV));
    long i = 0;
    for (JV *k = head; k; k = k->next) v->kids[i++] = *k;
    v->keys = kbuf;
  } else if (c == 't' && je - jp >= 4 && memcmp(jp, "true", 4) == 0) {
    v->t = J_BOOL;
    v->num = 1;
    jp += 4;
  } else if (c == 'f' && je - jp >= 5 && memcmp(jp, "false", 5) == 0) {
    v->t = J_BOOL;
    v->num = 0;
    jp += 5;
  } else if (c == 'n' && je - jp >= 4 && memcmp(jp, "null", 4) == 0) {
    v->t = J_NULL;
    jp += 4;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    v->t = J_NUM;
    v->isint = 1;
    int neg = c == '-';
    if (neg) jp++;
    long x = 0;
    int digits = 0;
    while (jp < je && *jp >= '0' && *jp <= '9') {
      if (digits++ > 15) v->isint = 0;
      x = x * 10 + (*jp++ - '0');
    }
    if (digits == 0) unsupported();
    while (jp < je && (*jp == '.' || *jp == 'e' || *jp == 'E' || *jp == '+' || *jp == '-' || (*jp >= '0' && *jp <= '9'))) {
      v->isint = 0;
      jp++;
    }
    v->num = neg ? -x : x;
  } else
    unsupported();
  return v;
}
static JV *jget(JV *o, const char *key) {
  if (o == 0 || o->t != J_OBJ) return 0;
  long kn = 0;
  while (key[kn]) kn++;
  JV *found = 0;
  for (long i = 0; i < o->n; i++)
    if (str_eqc(o->keys[i], key, kn)) found = &o->kids[i]; // the last of a repeated key, as JSON.parse keeps
  return found;
}
/** Skip one JSON object or array (finding a case's bytes without reading them). */
static void jskip(void) {
  jws();
  if (jp >= je || (*jp != '{' && *jp != '[')) unsupported();
  int depth = 0;
  do {
    if (jp >= je) unsupported();
    u8 c = *jp;
    if (c == '"') {
      jp++;
      for (;;) {
        jp = scan_jstr(jp, je);
        if (jp >= je) unsupported();
        if (*jp == '"') break;
        jp += *jp == '\\' ? 2 : 1;
      }
      jp++;
      continue;
    }
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
    jp++;
  } while (depth > 0);
}

// ---------------------------------------------------------------- JSON out

static u8 CLS_J[256], CLS_HJ[256], CLS_H[256];
static void tables_init(void) {
  for (int c = 0; c < 256; c++) {
    CLS_J[c] = c < 0x20 || c == '"' || c == '\\';
    CLS_H[c] = c == '&' || c == '<' || c == '>' || c == '"' || c == '\'';
    CLS_HJ[c] = CLS_J[c] || CLS_H[c];
  }
}
static void json_special(Arena *o, u8 c) {
  switch (c) {
    case '"': PUTL(o, "\\\""); break;
    case '\\': PUTL(o, "\\\\"); break;
    case '\n': PUTL(o, "\\n"); break;
    case '\r': PUTL(o, "\\r"); break;
    case '\t': PUTL(o, "\\t"); break;
    case '\b': PUTL(o, "\\b"); break;
    case '\f': PUTL(o, "\\f"); break;
    default: {
      static const char hx[] = "0123456789abcdef";
      char b[6] = { '\\', 'u', '0', '0', hx[c >> 4], hx[c & 15] };
      put(o, b, 6);
    }
  }
}
static inline void html_special(Arena *o, u8 c) {
  switch (c) {
    case '&': PUTL(o, "&amp;"); break;
    case '<': PUTL(o, "&lt;"); break;
    case '>': PUTL(o, "&gt;"); break;
    case '"': PUTL(o, "&quot;"); break;
    default: PUTL(o, "&#39;"); break;
  }
}
/** The length of the run from p (at most n) holding no byte of the class (the 16-byte steps by SSE2). */
static inline long run_json(const u8 *p, long n) {
  long i = 0;
  const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\'), lim = _mm_set1_epi8(0x1F);
  for (; i + 16 <= n; i += 16) {
    __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, q), _mm_cmpeq_epi8(x, bs)), _mm_cmpeq_epi8(_mm_min_epu8(x, lim), x));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + __builtin_ctz(mask);
  }
  while (i < n && !CLS_J[p[i]]) i++;
  return i;
}
static inline long run_hj(const u8 *p, long n) {
  long i = 0;
  const __m128i q = _mm_set1_epi8('"'), bs = _mm_set1_epi8('\\'), lim = _mm_set1_epi8(0x1F), am = _mm_set1_epi8('&'),
                lt = _mm_set1_epi8('<'), gt = _mm_set1_epi8('>'), ap = _mm_set1_epi8('\'');
  for (; i + 16 <= n; i += 16) {
    __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, q), _mm_cmpeq_epi8(x, bs)), _mm_cmpeq_epi8(_mm_min_epu8(x, lim), x));
    m = _mm_or_si128(m, _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, am), _mm_cmpeq_epi8(x, lt)), _mm_or_si128(_mm_cmpeq_epi8(x, gt), _mm_cmpeq_epi8(x, ap))));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + __builtin_ctz(mask);
  }
  while (i < n && !CLS_HJ[p[i]]) i++;
  return i;
}
static inline long run_h(const u8 *p, long n) {
  long i = 0;
  const __m128i q = _mm_set1_epi8('"'), am = _mm_set1_epi8('&'), lt = _mm_set1_epi8('<'), gt = _mm_set1_epi8('>'), ap = _mm_set1_epi8('\'');
  for (; i + 16 <= n; i += 16) {
    __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, q), _mm_cmpeq_epi8(x, am)), _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, lt), _mm_cmpeq_epi8(x, gt)), _mm_cmpeq_epi8(x, ap)));
    int mask = _mm_movemask_epi8(m);
    if (mask) return i + __builtin_ctz(mask);
  }
  while (i < n && !CLS_H[p[i]]) i++;
  return i;
}
/** s as a JSON string's body. */
static void emit_j(Arena *o, Str s) {
  const u8 *p = (const u8 *)s.p;
  long n = s.n;
  while (n > 0) {
    long r = run_json(p, n);
    put(o, (const char *)p, r);
    if (r == n) return;
    json_special(o, p[r]);
    p += r + 1;
    n -= r + 1;
  }
}
/** s HTML-escaped, as a JSON string's body. */
static void emit_hj(Arena *o, Str s) {
  const u8 *p = (const u8 *)s.p;
  long n = s.n;
  while (n > 0) {
    long r = run_hj(p, n);
    put(o, (const char *)p, r);
    if (r == n) return;
    if (CLS_H[p[r]]) html_special(o, p[r]);
    else json_special(o, p[r]);
    p += r + 1;
    n -= r + 1;
  }
}
/** s HTML-escaped (makefun-site's escape table). */
static void emit_h(Arena *o, Str s) {
  const u8 *p = (const u8 *)s.p;
  long n = s.n;
  while (n > 0) {
    long r = run_h(p, n);
    put(o, (const char *)p, r);
    if (r == n) return;
    html_special(o, p[r]);
    p += r + 1;
    n -= r + 1;
  }
}
/** JSON.stringify(s), into the arena A (for a template error's message). */
static Str json_quote(Str s) {
  // Written through a small stream on A: room for the worst case (6 bytes a byte, the quotes).
  char *buf = alloc(s.n * 6 + 2);
  Arena t = { buf, buf, buf + s.n * 6 + 2 };
  putch(&t, '"');
  emit_j(&t, s);
  putch(&t, '"');
  return (Str){ buf, t.cur - buf };
}

// ---------------------------------------------------------------- slugs

static napi_env genv;
static napi_value gslug;

/** Kept for the whole build: each run of characters outside ASCII met in a slug, and its NFKD with the combining
 * marks dropped (asked of JavaScript once: solve.js's plainOf). NFKD never reorders across an ASCII character (each
 * is a starter), so a text's NFKD is its runs' NFKDs between its ASCII characters. */
static Arena P;
typedef struct { Str k, v; } Plain;
static Plain *plains;
static long nplains, cplains;
static void *palloc(size_t n) {
  n = (n + 7) & ~(size_t)7;
  if ((size_t)(P.end - P.cur) < n) unsupported();
  void *p = P.cur;
  P.cur += n;
  return p;
}
static Str plain_of(Str run) {
  for (long i = 0; i < nplains; i++)
    if (str_eq(plains[i].k, run)) return plains[i].v;
#ifdef PROF
  unsigned long long _c0 = rdtsc();
#endif
  napi_value s, r, und;
  size_t len = 0;
  if (napi_create_string_utf8(genv, run.p, run.n, &s) != napi_ok) unsupported();
  napi_get_undefined(genv, &und);
  if (napi_call_function(genv, und, gslug, 1, &s, &r) != napi_ok) unsupported();
  if (napi_get_value_string_utf8(genv, r, 0, 0, &len) != napi_ok) unsupported();
  char *b = palloc(len + 1);
  napi_get_value_string_utf8(genv, r, b, len + 1, &len);
  char *k = palloc(run.n);
  memcpy(k, run.p, run.n);
  if (nplains == cplains) {
    long nc = cplains ? cplains * 2 : 64;
    Plain *np = palloc(nc * sizeof(Plain));
    if (nplains) memcpy(np, plains, nplains * sizeof(Plain));
    plains = np;
    cplains = nc;
  }
  plains[nplains++] = (Plain){ { k, run.n }, { b, (long)len } };
#ifdef PROF
  prof_t[10] += rdtsc() - _c0;
  prof_t[11] += 1;
#endif
  return (Str){ b, (long)len };
}

/** makefun-site's slug of text (README "Text"): NFKD, combining marks dropped, A-Z lowered, every run of other
 * characters than a-z and 0-9 one "-", none at either end, at most 64, "untitled" when empty. */
static Str slugify(Str t) {
  long cap = t.n + 9;
  char *out = alloc(cap);
  long n = 0;
  int gap = 0;
  for (long i = 0; i < t.n;) {
    u8 c = t.p[i];
    if (c >= 0x80) {
      long j = i;
      while (j < t.n && (u8)t.p[j] >= 0x80) j++;
      Str pl = plain_of(sub(t, i, j));
      i = j;
      for (long q = 0; q < pl.n; q++) {
        u8 d = pl.p[q];
        char keep;
        if (d >= 'A' && d <= 'Z') keep = d + 32;
        else if ((d >= 'a' && d <= 'z') || (d >= '0' && d <= '9')) keep = d;
        else {
          gap = n > 0;
          while (q + 1 < pl.n && ((u8)pl.p[q + 1] & 0xC0) == 0x80) q++; // the rest of this character
          continue;
        }
        if (n + 2 > cap) {
          long nc = cap * 2 + 16;
          char *no = alloc(nc);
          memcpy(no, out, n);
          out = no;
          cap = nc;
        }
        if (gap) out[n++] = '-';
        gap = 0;
        out[n++] = keep;
      }
      continue;
    }
    char keep;
    if (c >= 'A' && c <= 'Z') keep = c + 32;
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
    else {
      gap = n > 0;
      i++;
      continue;
    }
    if (n + 2 > cap) {
      long nc = cap * 2 + 16;
      char *no = alloc(nc);
      memcpy(no, out, n);
      out = no;
      cap = nc;
    }
    if (gap) out[n++] = '-';
    gap = 0;
    out[n++] = keep;
    i++;
  }
  if (n > 64) n = 64;
  while (n > 0 && out[n - 1] == '-') n--;
  if (n == 0) return LIT("untitled");
  return (Str){ out, n };
}
/** slug made unique against `taken` (a set keyed by prefix + slug), and taken. */
static Str unique_slug(Str slug, Map *taken, Str prefix) {
  Str out = slug;
  Str key = prefix.n ? CAT(prefix, out) : out;
  for (long n = 2; map_has(taken, key); n++) {
    out = CAT(slug, LIT("-"), num_str(n));
    key = prefix.n ? CAT(prefix, out) : out;
  }
  set_add(taken, key);
  return out;
}

static const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static Str long_date(Str d) {
  // d is YYYY-MM-DD, checked
  int day = (d.p[8] - '0') * 10 + (d.p[9] - '0');
  int mon = (d.p[5] - '0') * 10 + (d.p[6] - '0');
  const char *m = MONTHS[mon - 1];
  long mn = 0;
  while (m[mn]) mn++;
  return CAT(num_str(day), LIT(" "), ((Str){ m, mn }), LIT(" "), sub(d, 0, 4));
}

// ---------------------------------------------------------------- the build's records

typedef struct Tag Tag;
typedef struct { int level; Str id, html; } Toc;
typedef struct Src {
  Str path, text, body;
  Str title, date, layout, slugfm, summary;
  u8 has_date, has_layout, has_slug, has_summary, has_tags, draft, post;
  Str *tags;
  long ntags;
  int tpl;
  Str slug, outdir, url, date_long;
  Str html, excerpt;
  Toc *toc;
  long ntoc;
  long minutes;
  Tag **trefs;
  long ntrefs;
  struct Src *newer, *older;
} Src;
struct Tag {
  Str name, slug, url;
  long count, fill;
  Src **posts;
};

// ---------------------------------------------------------------- templates

enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INC, N_CHUNK };
typedef struct Node {
  u8 kind, neg, has_else;
  int name;
  int *path;
  int plen;
  Str text; // text: as a JSON string's body; include: the template's name
  struct Node *body, *els, *next;
  int tpl;
} Node;
typedef struct { Str name; Node *nodes; int ok; } Tpl;

// Names: every path segment interned; the fields the build gives come first, in this order.
enum {
  F_title, F_url, F_date, F_date_long, F_summary, F_excerpt, F_reading_minutes, F_tags, F_slug, F_content, F_toc,
  F_newer, F_older, F_is_post, F_name, F_count, F_posts, F_level, F_id, F_html, F_number, F_total, F_newer_url,
  F_older_url, F_index, F_first, F_last, F_length, F_base_url, F_site, F_recent, F_page, F_pager, F_tag, F_loop, F_COUNT
};
static const char *FIELD_NAMES[] = { "title", "url", "date", "date_long", "summary", "excerpt", "reading_minutes", "tags", "slug", "content", "toc", "newer", "older", "is_post", "name", "count", "posts", "level", "id", "html", "number", "total", "newer_url", "older_url", "index", "first", "last", "length", "base_url", "site", "recent", "page", "pager", "tag", "loop" };
static Map names;
static int nnames;
static int intern(Str s) {
  int nw;
  void **slot = map_put(&names, s, &nw);
  if (nw) *slot = (void *)(intptr_t)nnames++;
  return (int)(intptr_t)*slot;
}

typedef struct Err { Str msg; } Err;
static Str terr; // a template's parse error, when one is met
#define TFAIL(...)            \
  do {                        \
    terr = CAT(__VA_ARGS__); \
    return 0;                 \
  } while (0)

static inline int ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static inline int ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
/** The path `text` names, or 0 with terr set. */
static int *path_of(Str name, Str text, int *plen) {
  // ^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$
  int segs = 1;
  long i = 0;
  int ok = text.n > 0;
  while (ok) {
    if (i >= text.n || !ident_start(text.p[i])) {
      ok = 0;
      break;
    }
    i++;
    while (i < text.n && ident_char(text.p[i])) i++;
    if (i == text.n) break;
    if (text.p[i] != '.') {
      ok = 0;
      break;
    }
    i++;
    segs++;
  }
  if (!ok) {
    terr = CAT(LIT("template \""), name, LIT("\": "), json_quote(text), LIT(" is not a path"));
    return 0;
  }
  int *p = alloc(segs * sizeof(int));
  long st = 0;
  int k = 0;
  for (long j = 0; j <= text.n; j++)
    if (j == text.n || text.p[j] == '.') {
      p[k++] = intern(sub(text, st, j));
      st = j + 1;
    }
  *plen = segs;
  return p;
}

typedef struct Tok {
  u8 t; // 0 text, 1 var, 2 raw, 3 tag
  Str s;
  int *path;
  int plen;
  struct Tok *next;
} Tok;

/** A template's tokens (makefun-site's tokenize), or 0 with terr set. Text is gathered as slices of src: a token's
 * text is src[i, open) pieces, kept as a list of pieces joined when the token is pushed. */
static Tok *tokenize(Str name, Str src, int *okp) {
  Tok *head = 0, **tail = &head;
  // text pieces: at most a few, joined lazily
  char *tb = alloc(src.n + 1);
  long tn = 0;
  long i = 0;
  *okp = 0;
#define PUSH_TEXT()                       \
  do {                                    \
    if (tn > 0) {                         \
      Tok *t = alloc(sizeof(Tok));        \
      t->t = 0;                           \
      t->s = (Str){ tb, tn };             \
      t->next = 0;                        \
      *tail = t;                          \
      tail = &t->next;                    \
      tb += tn;                           \
      tn = 0;                             \
    }                                     \
  } while (0)
  while (i < src.n) {
    long open = find_ch(src, '{', i);
    if (open == -1 || open == src.n - 1) {
      memcpy(tb + tn, src.p + i, src.n - i);
      tn += src.n - i;
      break;
    }
    char next = src.p[open + 1];
    if (next != '{' && next != '%') {
      memcpy(tb + tn, src.p + i, open + 1 - i);
      tn += open + 1 - i;
      i = open + 1;
      continue;
    }
    memcpy(tb + tn, src.p + i, open - i);
    tn += open - i;
    if (next == '{') {
      int raw = open + 2 < src.n && src.p[open + 2] == '{';
      long close = raw ? find_str(src, "}}}", 3, open + 3) : find_str(src, "}}", 2, open + 2);
      if (close == -1) {
        terr = CAT(LIT("template \""), name, raw ? LIT("\": a {{{ is not closed") : LIT("\": a {{ is not closed"));
        return 0;
      }
      PUSH_TEXT();
      Tok *t = alloc(sizeof(Tok));
      t->t = raw ? 2 : 1;
      t->path = path_of(name, js_trim(sub(src, open + (raw ? 3 : 2), close)), &t->plen);
      if (t->path == 0) return 0;
      t->next = 0;
      *tail = t;
      tail = &t->next;
      i = close + (raw ? 3 : 2);
      continue;
    }
    long close = find_str(src, "%}", 2, open + 2);
    if (close == -1) {
      terr = CAT(LIT("template \""), name, LIT("\": a {% is not closed"));
      return 0;
    }
    long end = close + 2;
    long ls = open - 1;
    while (ls >= 0 && src.p[ls] != '\n') ls--;
    long line_start = ls + 1;
    long nl = find_ch(src, '\n', end);
    long line_end = nl == -1 ? src.n : nl;
    int standalone = all_sp_tab(sub(src, line_start, open)) && all_sp_tab(sub(src, end, line_end));
    if (standalone) {
      if (tn < open - line_start) unsupported();
      tn -= open - line_start;
    }
    PUSH_TEXT();
    Tok *t = alloc(sizeof(Tok));
    t->t = 3;
    t->s = js_trim(sub(src, open + 2, close));
    t->next = 0;
    *tail = t;
    tail = &t->next;
    i = standalone ? (nl == -1 ? src.n : nl + 1) : end;
  }
  PUSH_TEXT();
#undef PUSH_TEXT
  *okp = 1;
  return head;
}

static int all_non_ws(Str s) {
  if (s.n == 0) return 0;
  return !has_js_ws(s);
}
/** A template parsed (makefun-site's parseTemplate): its nodes, or 0 with terr set (ok says which). */
static Node *parse_template(Str name, Str src, int *okp) {
  int tok_ok;
  Tok *toks = tokenize(name, src, &tok_ok);
  *okp = 0;
  if (!tok_ok) return 0;
  Node *root = 0;
  Node **tail = &root;
  struct { Node *node; Node **saved; } stack[256];
  int sp = 0;
  for (Tok *t = toks; t; t = t->next) {
    if (t->t == 0) {
      Node *n = allocz(sizeof(Node));
      n->kind = N_TEXT;
      // kept as a JSON string's body
      char *buf = alloc(t->s.n * 6 + 1);
      Arena s = { buf, buf, buf + t->s.n * 6 + 1 };
      emit_j(&s, t->s);
      n->text = (Str){ buf, s.cur - buf };
      *tail = n;
      tail = &n->next;
      continue;
    }
    if (t->t == 1 || t->t == 2) {
      Node *n = allocz(sizeof(Node));
      n->kind = t->t == 1 ? N_VAR : N_RAW;
      n->path = t->path;
      n->plen = t->plen;
      *tail = n;
      tail = &n->next;
      continue;
    }
    Str tag = t->s;
    Node *top = sp > 0 ? stack[sp - 1].node : 0;
    if (STARTS(tag, "for ")) {
      // ^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$
      long i = 4;
      if (i < tag.n && ident_start(tag.p[i])) {
        long st = i++;
        while (i < tag.n && ident_char(tag.p[i])) i++;
        Str var = sub(tag, st, i);
        Str rest = sub(tag, i, tag.n);
        if (STARTS(rest, " in ") && all_non_ws(sub(rest, 4, rest.n))) {
          Node *n = allocz(sizeof(Node));
          n->kind = N_FOR;
          n->name = intern(var);
          n->path = path_of(name, sub(rest, 4, rest.n), &n->plen);
          if (n->path == 0) return 0;
          *tail = n;
          if (sp == 256) unsupported();
          stack[sp].node = n;
          stack[sp].saved = &n->next;
          sp++;
          tail = &n->body;
          continue;
        }
      }
    }
    if (STARTS(tag, "if ")) {
      Str rest = sub(tag, 3, tag.n);
      int neg = -1;
      Str p = rest;
      if (STARTS(rest, "not ") && all_non_ws(sub(rest, 4, rest.n))) {
        neg = 1;
        p = sub(rest, 4, rest.n);
      } else if (all_non_ws(rest))
        neg = 0;
      if (neg >= 0) {
        Node *n = allocz(sizeof(Node));
        n->kind = N_IF;
        n->neg = neg;
        n->path = path_of(name, p, &n->plen);
        if (n->path == 0) return 0;
        *tail = n;
        if (sp == 256) unsupported();
        stack[sp].node = n;
        stack[sp].saved = &n->next;
        sp++;
        tail = &n->body;
        continue;
      }
    }
    if (EQL(tag, "else")) {
      if (top == 0 || top->kind != N_IF || top->has_else) TFAIL(LIT("template \""), name, LIT("\": an else outside an if"));
      top->has_else = 1;
      tail = &top->els;
      continue;
    }
    if (EQL(tag, "endfor") || EQL(tag, "endif")) {
      int isfor = tag.p[3] == 'f';
      if (top == 0 || top->kind != (isfor ? N_FOR : N_IF))
        TFAIL(LIT("template \""), name, isfor ? LIT("\": an endfor with no open for") : LIT("\": an endif with no open if"));
      sp--;
      tail = stack[sp].saved;
      continue;
    }
    if (STARTS(tag, "include \"") && tag.n > 10 && tag.p[tag.n - 1] == '"') {
      Str nm = sub(tag, 9, tag.n - 1);
      int ok = 1;
      for (long k = 0; k < nm.n; k++) {
        char c = nm.p[k];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = 0;
      }
      if (ok) {
        Node *n = allocz(sizeof(Node));
        n->kind = N_INC;
        n->text = nm;
        n->tpl = -1;
        *tail = n;
        tail = &n->next;
        continue;
      }
    }
    TFAIL(LIT("template \""), name, LIT("\": {% "), tag, LIT(" %} is not a tag"));
  }
  if (sp > 0) TFAIL(LIT("template \""), name, stack[sp - 1].node->kind == N_FOR ? LIT("\": a for is not closed") : LIT("\": a if is not closed"));
  *okp = 1;
  if (root == 0) {
    // an empty template still parses: an empty list of nodes, marked by ok
  }
  return root;
}

// ---------------------------------------------------------------- values, as templates read them

enum { T_U, T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ, T_JSTR };
enum { K_SITE, K_CARD, K_PAGE, K_LINK, K_TREF, K_TALL, K_TFULL, K_TOC, K_PAGER, K_LOOP };
typedef struct { u8 t, k; long n; const void *p; } Val;
typedef struct { Str title, base; } Site;
typedef struct { long number, total; Str url, newer_url, older_url; } Pager;
typedef struct Scope {
  struct Scope *parent;
  int root;
  int sh; // a for of the chain binds site, tags or recent: a chunk's cached text does not hold
  // a for: its name, the element, the loop's place
  int name;
  Val x;
  long k, len;
  // the root: site, tags, recent and the layout's own (page | posts + pager | tag)
  Val site, tags, recent;
  int e1, e2;
  Val v1, v2;
} Scope;

static inline Val vstr(Str s) { return (Val){ T_STR, 0, s.n, s.p }; }
static inline Val vjstr(Str s) { return (Val){ T_JSTR, 0, s.n, s.p }; }
static inline Val vnum(long n) { return (Val){ T_NUM, 0, n, 0 }; }
static inline Val vbool(int b) { return (Val){ T_BOOL, 0, b, 0 }; }
static inline Val vobj(int k, const void *p) { return (Val){ T_OBJ, (u8)k, 0, p }; }
static inline Val varr(int k, const void *p, long n) { return (Val){ T_ARR, (u8)k, n, p }; }
static const Val VU = { T_U, 0, 0, 0 };
static const Val VNULL = { T_NULL, 0, 0, 0 };

static Val arr_at(Val a, long i) {
  if (a.k == K_TOC) return vobj(K_TOC, (const Toc *)a.p + i);
  return vobj(a.k, ((void *const *)a.p)[i]);
}

static Val src_field(const Src *s, int f, int kind) {
  // the link record: title, url, date, date_long
  switch (f) {
    case F_title: return vstr(s->title);
    case F_url: return vstr(s->url);
    case F_date: return vstr(s->has_date ? s->date : LIT(""));
    case F_date_long: return vstr(s->has_date ? s->date_long : LIT(""));
  }
  if (kind == K_LINK) return VU;
  switch (f) {
    case F_summary: return vstr(s->has_summary ? s->summary : LIT(""));
    case F_excerpt: return vjstr(s->excerpt);
    case F_reading_minutes: return vnum(s->minutes);
    case F_tags: return varr(K_TREF, s->trefs, s->ntrefs);
  }
  if (kind == K_CARD) return VU;
  switch (f) {
    case F_slug: return vstr(s->slug);
    case F_content: return vjstr(s->html);
    case F_toc: return varr(K_TOC, s->toc, s->ntoc);
    case F_newer: return s->newer ? vobj(K_LINK, s->newer) : VNULL;
    case F_older: return s->older ? vobj(K_LINK, s->older) : VNULL;
    case F_is_post: return vbool(s->post);
  }
  return VU;
}
static Val field(Val v, int f) {
  if (v.t == T_ARR) return f == F_length ? vnum(v.n) : VU;
  if (v.t != T_OBJ) return VU;
  switch (v.k) {
    case K_SITE: {
      const Site *s = v.p;
      return f == F_title ? vstr(s->title) : f == F_base_url ? vstr(s->base) : VU;
    }
    case K_CARD:
    case K_PAGE:
    case K_LINK: return src_field(v.p, f, v.k);
    case K_TREF:
    case K_TALL:
    case K_TFULL: {
      const Tag *t = v.p;
      switch (f) {
        case F_name: return vstr(t->name);
        case F_slug: return vstr(t->slug);
        case F_url: return vstr(t->url);
      }
      if (v.k == K_TREF) return VU;
      if (f == F_count) return vnum(t->count);
      if (v.k == K_TALL) return VU;
      if (f == F_posts) return varr(K_CARD, t->posts, t->count);
      return VU;
    }
    case K_TOC: {
      const Toc *t = v.p;
      return f == F_level ? vnum(t->level) : f == F_id ? vstr(t->id) : f == F_html ? vjstr(t->html) : VU;
    }
    case K_PAGER: {
      const Pager *p = v.p;
      switch (f) {
        case F_number: return vnum(p->number);
        case F_total: return vnum(p->total);
        case F_url: return vstr(p->url);
        case F_newer_url: return vstr(p->newer_url);
        case F_older_url: return vstr(p->older_url);
      }
      return VU;
    }
    case K_LOOP: {
      const Scope *s = v.p;
      switch (f) {
        case F_index: return vnum(s->k + 1);
        case F_first: return vbool(s->k == 0);
        case F_last: return vbool(s->k == s->len - 1);
        case F_length: return vnum(s->len);
      }
      return VU;
    }
  }
  return VU;
}
static Val lookup(const Scope *sc, const int *path, int plen) {
  int f0 = path[0];
  Val v = VU;
  const Scope *s = sc;
  for (; s; s = s->parent) {
    if (s->root) {
      if (f0 == F_site) { v = s->site; break; }
      if (f0 == F_tags) { v = s->tags; break; }
      if (f0 == F_recent) { v = s->recent; break; }
      if (f0 == s->e1) { v = s->v1; break; }
      if (f0 == s->e2) { v = s->v2; break; }
    } else {
      if (f0 == F_loop) { v = vobj(K_LOOP, s); break; }
      if (f0 == s->name) { v = s->x; break; }
    }
  }
  if (s == 0) return VU;
  for (int k = 1; k < plen; k++) {
    if (v.t != T_OBJ && v.t != T_ARR) return VU;
    v = field(v, path[k]);
    if (v.t == T_U) return VU;
  }
  return v;
}
static inline int truthy(Val v) {
  switch (v.t) {
    case T_ARR: return v.n > 0;
    case T_U:
    case T_NULL: return 0;
    case T_BOOL:
    case T_NUM: return v.n != 0;
    case T_STR:
    case T_JSTR: return v.n > 0;
    default: return 1;
  }
}
/** A JSON string's body (as Markdown's HTML is kept) read back and HTML-escaped, as a JSON string's body. */
static void emit_hj_of_json(Str j) {
  for (long i = 0; i < j.n; i++) {
    u8 c = j.p[i];
    if (c == '\\' && i + 1 < j.n) {
      u8 e = j.p[++i];
      switch (e) {
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': c = (u8)((hexv(j.p[i + 3]) << 4) | hexv(j.p[i + 4])); i += 4; break;
        default: c = e;
      }
    }
    if (CLS_H[c]) html_special(&O, c);
    else if (CLS_J[c]) json_special(&O, c);
    else putch(&O, (char)c);
  }
}
static void emit_text(Val v, int html) {
  switch (v.t) {
    case T_STR:
      if (html) emit_hj(&O, (Str){ v.p, v.n });
      else emit_j(&O, (Str){ v.p, v.n });
      break;
    case T_JSTR:
      if (html) emit_hj_of_json((Str){ v.p, v.n });
      else put(&O, v.p, v.n);
      break;
    case T_NUM: putnum(&O, v.n); break;
    case T_BOOL:
      if (v.n) PUTL(&O, "true");
      else PUTL(&O, "false");
      break;
    case T_ARR:
      // every array the build gives holds objects, whose text is ""
      for (long i = 1; i < v.n; i++) PUTL(&O, ", ");
      break;
    default: break;
  }
}

static Tpl *tpls;
static long ntpls;

// Chunks: a run of sibling nodes that reads nothing but site, tags and recent (and names its own fors bind) renders
// the same on every page of a case: its text is kept at its first render, per include depth, and copied after.
static int chunk_n;
static Str *chunk_text;
static inline int name_ok(int f, const int *bound, int nb) {
  for (int i = 0; i < nb; i++)
    if (bound[i] == f) return 1;
  return f == F_site || f == F_tags || f == F_recent;
}
static int stat_list(const Node *n, const int *bound, int nb, int depth, long *budget);
static int stat_node(const Node *n, const int *bound, int nb, int depth, long *budget) {
  if (--*budget < 0) return 0;
  switch (n->kind) {
    case N_TEXT:
    case N_CHUNK: return 1;
    case N_VAR:
    case N_RAW: return name_ok(n->path[0], bound, nb);
    case N_IF: return name_ok(n->path[0], bound, nb) && stat_list(n->body, bound, nb, depth, budget) && stat_list(n->els, bound, nb, depth, budget);
    case N_FOR: {
      if (!name_ok(n->path[0], bound, nb) || nb > 60) return 0;
      int b2[64];
      for (int i = 0; i < nb; i++) b2[i] = bound[i];
      b2[nb] = n->name;
      b2[nb + 1] = F_loop;
      return stat_list(n->body, b2, nb + 2, depth, budget);
    }
    case N_INC:
      if (n->tpl < 0 || depth >= 8) return 1;
      return stat_list(tpls[n->tpl].nodes, bound, nb, depth + 1, budget);
  }
  return 0;
}
static int stat_list(const Node *n, const int *bound, int nb, int depth, long *budget) {
  for (; n; n = n->next)
    if (!stat_node(n, bound, nb, depth, budget)) return 0;
  return 1;
}
static inline int is_static(const Node *n) {
  long budget = 20000;
  return stat_node(n, 0, 0, 0, &budget);
}
/** list with each maximal run of static nodes holding more than text made one chunk. */
static Node *chunkify(Node *list) {
  Node *head = 0, **tail = &head;
  Node *n = list;
  while (n) {
    if (is_static(n)) {
      Node *last = n;
      int more = n->kind != N_TEXT;
      while (last->next && is_static(last->next)) {
        last = last->next;
        more |= last->kind != N_TEXT;
      }
      Node *after = last->next;
      if (more) {
        Node *c = allocz(sizeof(Node));
        c->kind = N_CHUNK;
        c->body = n;
        c->tpl = chunk_n++;
        last->next = 0;
        *tail = c;
        tail = &c->next;
      } else {
        *tail = n;
        tail = &last->next;
      }
      n = after;
      continue;
    }
    Node *next = n->next;
    if (n->kind == N_IF) {
      n->body = chunkify(n->body);
      n->els = chunkify(n->els);
    } else if (n->kind == N_FOR)
      n->body = chunkify(n->body);
    *tail = n;
    tail = &n->next;
    n = next;
  }
  *tail = 0;
  return head;
}

static void render(const Node *n, const Scope *sc, int depth) {
  for (; n; n = n->next) {
    switch (n->kind) {
      case N_TEXT: put(&O, n->text.p, n->text.n); break;
      case N_VAR: emit_text(lookup(sc, n->path, n->plen), 1); break;
      case N_RAW: emit_text(lookup(sc, n->path, n->plen), 0); break;
      case N_IF: {
        int t = truthy(lookup(sc, n->path, n->plen));
        render(t != n->neg ? n->body : n->els, sc, depth);
        break;
      }
      case N_FOR: {
        Val xs = lookup(sc, n->path, n->plen);
        if (xs.t != T_ARR) break;
        Scope s;
        s.parent = (Scope *)sc;
        s.root = 0;
        s.name = n->name;
        s.sh = sc->sh || n->name == F_site || n->name == F_tags || n->name == F_recent;
        s.len = xs.n;
        for (long k = 0; k < xs.n; k++) {
          s.k = k;
          s.x = arr_at(xs, k);
          render(n->body, &s, depth);
        }
        break;
      }
      case N_INC:
        if (n->tpl >= 0 && depth < 8) render(tpls[n->tpl].nodes, sc, depth + 1);
        break;
      case N_CHUNK: {
        if (sc->sh) {
          render(n->body, sc, depth);
          break;
        }
        Str *c = &chunk_text[n->tpl * 9 + depth];
        if (c->p) put(&O, c->p, c->n);
        else {
          char *st = O.cur;
          render(n->body, sc, depth);
          *c = (Str){ st, O.cur - st };
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------- front matter

typedef struct { Str err; } FmErr;
static int days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}
/** A string value, or 0 with *err. */
static int string_of(Str raw, Str *out, Str *err) {
  if (STARTS(raw, "\"")) {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      *err = LIT("a quoted string is not closed");
      return 0;
    }
    char *b = alloc(raw.n);
    long n = 0;
    for (long i = 1; i < raw.n - 1; i++) {
      char c = raw.p[i];
      if (c == '\\') {
        char nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.n - 1) {
          *err = LIT("a quoted string holds an escape other than \\\" or \\\\");
          return 0;
        }
        b[n++] = nx;
        i++;
      } else if (c == '"') {
        *err = LIT("a quoted string holds a stray quote");
        return 0;
      } else
        b[n++] = c;
    }
    *out = (Str){ b, n };
    return 1;
  }
  if (STARTS(raw, "[")) {
    *err = LIT("a bare string opens on [");
    return 0;
  }
  *out = raw;
  return 1;
}
static const char *FM_KEYS[] = { "title", "date", "tags", "draft", "layout", "slug", "summary" };
/** The front matter of s (makefun-site's parseFrontMatter): 1, or 0 with *err. */
static int front_matter(Src *s, Str *err) {
  Str t = s->text;
  long e0 = find_ch(t, '\n', 0);
  Str first = e0 == -1 ? t : sub(t, 0, e0);
  if (!EQL(first, "---")) {
    *err = LIT("no front matter");
    return 0;
  }
  // the closing line
  long close_at = -1, close_end = -1;
  long pos = e0 == -1 ? t.n : e0 + 1;
  if (e0 != -1)
    while (1) {
      long e = find_ch(t, '\n', pos);
      Str line = e == -1 ? sub(t, pos, t.n) : sub(t, pos, e);
      if (EQL(line, "---")) {
        close_at = pos;
        close_end = e;
        break;
      }
      if (e == -1) break;
      pos = e + 1;
    }
  if (close_at == -1) {
    *err = LIT("the front matter is not closed");
    return 0;
  }
  int seen = 0;
  long lineno = 1;
  pos = e0 + 1;
  while (pos < close_at) {
    long e = find_ch(t, '\n', pos);
    Str line = sub(t, pos, e);
    pos = e + 1;
    lineno++;
    if (all_sp_tab(line) || STARTS(line, "#")) continue;
    // ^([a-z_]+):(?: (.*))?$
    long k = 0;
    while (k < line.n && ((line.p[k] >= 'a' && line.p[k] <= 'z') || line.p[k] == '_')) k++;
    Str raw;
    int ok = k > 0 && k < line.n && line.p[k] == ':';
    if (ok) {
      if (k + 1 == line.n) raw = LIT("");
      else if (line.p[k + 1] == ' ') raw = sub(line, k + 2, line.n);
      else ok = 0;
    }
    if (!ok) {
      *err = CAT(LIT("line "), num_str(lineno), LIT(" is not \"key: value\""));
      return 0;
    }
    Str key = sub(line, 0, k);
    while (raw.n > 0 && sp_tab(raw.p[raw.n - 1])) raw.n--;
    int ki = -1;
    for (int j = 0; j < 7; j++) {
      long kn = 0;
      while (FM_KEYS[j][kn]) kn++;
      if (str_eqc(key, FM_KEYS[j], kn)) ki = j;
    }
    if (ki < 0) {
      *err = CAT(LIT("unknown key \""), key, LIT("\""));
      return 0;
    }
    if (seen & (1 << ki)) {
      *err = CAT(LIT("key \""), key, LIT("\" is given twice"));
      return 0;
    }
    if (raw.n == 0) {
      *err = CAT(LIT("key \""), key, LIT("\" has no value"));
      return 0;
    }
    Str e2;
    switch (ki) {
      case 0:
      case 6: {
        Str v;
        if (!string_of(raw, &v, &e2)) {
          *err = CAT(LIT("key \""), key, LIT("\": "), e2);
          return 0;
        }
        if (ki == 0) s->title = v;
        else {
          s->summary = v;
          s->has_summary = 1;
        }
        break;
      }
      case 1: {
        int okd = raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-';
        for (int j = 0; okd && j < 10; j++)
          if (j != 4 && j != 7 && !(raw.p[j] >= '0' && raw.p[j] <= '9')) okd = 0;
        int y = 0, mo = 0, d = 0;
        if (okd) {
          y = (raw.p[0] - '0') * 1000 + (raw.p[1] - '0') * 100 + (raw.p[2] - '0') * 10 + (raw.p[3] - '0');
          mo = (raw.p[5] - '0') * 10 + (raw.p[6] - '0');
          d = (raw.p[8] - '0') * 10 + (raw.p[9] - '0');
        }
        if (!okd || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in(y, mo)) {
          *err = LIT("key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
          return 0;
        }
        s->date = raw;
        s->has_date = 1;
        break;
      }
      case 2: {
        if (!STARTS(raw, "[") || raw.p[raw.n - 1] != ']') {
          *err = LIT("key \"tags\": not a list [a, b]");
          return 0;
        }
        Str inner = raw.n >= 2 ? sub(raw, 1, raw.n - 1) : LIT("");
        s->has_tags = 1;
        if (js_trim(inner).n == 0) {
          s->ntags = 0;
          s->tags = 0;
          break;
        }
        long cnt = 1;
        for (long j = 0; j < inner.n; j++) cnt += inner.p[j] == ',';
        Str *tags = alloc(cnt * sizeof(Str));
        long st = 0, ti = 0;
        int bad = 0;
        for (long j = 0; j <= inner.n; j++)
          if (j == inner.n || inner.p[j] == ',') {
            Str tg = js_trim(sub(inner, st, j));
            if (tg.n == 0) bad = 1;
            for (long q = 0; q < tg.n; q++)
              if (tg.p[q] == '"' || tg.p[q] == '[' || tg.p[q] == ']') bad = 1;
            tags[ti++] = tg;
            st = j + 1;
          }
        if (bad) {
          *err = LIT("key \"tags\": an empty tag, or one holding \" [ or ]");
          return 0;
        }
        s->tags = tags;
        s->ntags = cnt;
        break;
      }
      case 3:
        if (EQL(raw, "true")) s->draft = 1;
        else if (EQL(raw, "false")) s->draft = 0;
        else {
          *err = LIT("key \"draft\": true or false");
          return 0;
        }
        break;
      case 4: {
        int okn = 1;
        for (long j = 0; j < raw.n; j++) {
          char c = raw.p[j];
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) okn = 0;
        }
        if (!okn) {
          *err = LIT("key \"layout\": not a template name");
          return 0;
        }
        s->layout = raw;
        s->has_layout = 1;
        break;
      }
      case 5: {
        // ^[a-z0-9]+(?:-[a-z0-9]+)*$ and at most 64
        int oks = raw.n <= 64 && raw.p[0] != '-' && raw.p[raw.n - 1] != '-';
        for (long j = 0; oks && j < raw.n; j++) {
          char c = raw.p[j];
          if (c == '-') {
            if (raw.p[j + 1] == '-') oks = 0;
          } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            oks = 0;
        }
        if (!oks) {
          *err = LIT("key \"slug\": not a slug");
          return 0;
        }
        s->slugfm = raw;
        s->has_slug = 1;
        break;
      }
    }
    seen |= 1 << ki;
  }
  if (!(seen & 1)) {
    *err = LIT("no title");
    return 0;
  }
  s->body = close_end == -1 ? LIT("") : sub(t, close_end + 1, t.n);
  return 1;
}

// ---------------------------------------------------------------- Markdown

static Str base;
static Map by_source, static_paths, broken;
static long nbroken;
static Str *broken_list;
static long broken_cap;

typedef struct { Src *s; Str dir; } Env;
static Env *env;

static int resolve_path(Str dir, Str rel, char *out, long cap, long *outn) {
  // segments of dir, then rel's
  Str parts[512];
  int np = 0;
  if (dir.n > 0) {
    long st = 0;
    for (long j = 0; j <= dir.n; j++)
      if (j == dir.n || dir.p[j] == '/') {
        if (np == 512) unsupported();
        parts[np++] = sub(dir, st, j);
        st = j + 1;
      }
  }
  long st = 0;
  for (long j = 0; j <= rel.n; j++)
    if (j == rel.n || rel.p[j] == '/') {
      Str seg = sub(rel, st, j);
      st = j + 1;
      if (seg.n == 0 || EQL(seg, ".")) continue;
      if (EQL(seg, "..")) {
        if (np == 0) return 0;
        np--;
      } else {
        if (np == 512) unsupported();
        parts[np++] = seg;
      }
    }
  long n = 0;
  for (int k = 0; k < np; k++) {
    if (n + parts[k].n + 1 > cap) unsupported();
    if (k > 0) out[n++] = '/';
    memcpy(out + n, parts[k].p, parts[k].n);
    n += parts[k].n;
  }
  *outn = n;
  return 1;
}
/** A link's href (before the escape), written HTML-escaped into B. */
static void emit_link(Str target) {
  // ^[A-Za-z][A-Za-z0-9+.-]*:
  int scheme = 0;
  if (target.n > 0 && ((target.p[0] >= 'A' && target.p[0] <= 'Z') || (target.p[0] >= 'a' && target.p[0] <= 'z'))) {
    long i = 1;
    while (i < target.n) {
      char c = target.p[i];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-') i++;
      else break;
    }
    scheme = i < target.n && target.p[i] == ':';
  }
  if (scheme || STARTS(target, "/") || STARTS(target, "#")) {
    emit_hj(&B, target);
    return;
  }
  long hash = find_ch(target, '#', 0);
  Str rel = hash == -1 ? target : sub(target, 0, hash);
  Str frag = hash == -1 ? LIT("") : sub(target, hash, target.n);
  if (rel.n > 0) {
    char buf[4096];
    long pn;
    if (resolve_path(env->dir, rel, buf, sizeof buf, &pn)) {
      Str path = { buf, pn };
      void **src = map_get(&by_source, path);
      if (src) {
        emit_hj(&B, ((Src *)*src)->url);
        emit_hj(&B, frag);
        return;
      }
      if (map_has(&static_paths, path)) {
        emit_hj(&B, base);
        PUTL(&B, "/");
        emit_hj(&B, path);
        emit_hj(&B, frag);
        return;
      }
    }
  }
  Str line = CAT(LIT("broken-link "), env->s->path, LIT(" -> "), target);
  int nw;
  map_put(&broken, line, &nw);
  if (nw) {
    if (nbroken == broken_cap) {
      long nc = broken_cap ? broken_cap * 2 : 64;
      Str *nb = alloc(nc * sizeof(Str));
      if (nbroken) memcpy(nb, broken_list, nbroken * sizeof(Str));
      broken_list = nb;
      broken_cap = nc;
    }
    broken_list[nbroken++] = line;
  }
  emit_hj(&B, target);
}

static u8 ESCAPABLE[256];
static inline int escapable_at(Str s, long j) { return s.p[j] == '\\' && j + 1 < s.n && ESCAPABLE[(u8)s.p[j + 1]]; }
static long find_unescaped(Str s, const char *w, long wn, long from) {
  for (long j = from; j <= s.n - wn; j++) {
    if (escapable_at(s, j)) {
      j++;
      continue;
    }
    if (s.p[j] == w[0] && (wn == 1 || memcmp(s.p + j, w, wn) == 0)) return j;
  }
  return -1;
}
static long find_single_star(Str s, long from) {
  for (long j = from; j < s.n; j++) {
    char c = s.p[j];
    if (escapable_at(s, j)) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}
static u8 INLINE_SPECIAL[256];
/** The length of the run from p holding none of \\ ` * [ & < > " ' (the bytes inline treats). */
static inline long run_inline(const u8 *p, long n) {
  long i = 0;
  const __m128i a = _mm_set1_epi8('\\'), b = _mm_set1_epi8('`'), c = _mm_set1_epi8('*'), d = _mm_set1_epi8('['),
                e = _mm_set1_epi8('&'), f = _mm_set1_epi8('<'), g = _mm_set1_epi8('>'), h = _mm_set1_epi8('"'), k = _mm_set1_epi8('\'');
  for (; i + 16 <= n; i += 16) {
    __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
    __m128i m = _mm_or_si128(_mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, a), _mm_cmpeq_epi8(x, b)), _mm_or_si128(_mm_cmpeq_epi8(x, c), _mm_cmpeq_epi8(x, d))),
                             _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, e), _mm_cmpeq_epi8(x, f)), _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, g), _mm_cmpeq_epi8(x, h)), _mm_cmpeq_epi8(x, k))));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_min_epu8(x, _mm_set1_epi8(0x1F)), x));
    int mm = _mm_movemask_epi8(m);
    if (mm) return i + __builtin_ctz(mm);
  }
  while (i < n && !INLINE_SPECIAL[p[i]]) i++;
  return i;
}
/** s rendered inline (makefun-site's inline), into B. */
static void inline_md(Str s) {
  long i = 0;
  while (i < s.n) {
    // a run of plain characters (none of \ ` * [ and nothing to escape) goes at once
    long r = i + run_inline((const u8 *)s.p + i, s.n - i);
    if (r > i) {
      put(&B, s.p + i, r - i);
      i = r;
      if (i >= s.n) break;
    }
    char c = s.p[i];
#ifdef PROF
    prof_t[15]++;
#endif
    switch (c) {
      // the characters that are only escaped, at once
      case '\n': PUTL(&B, "\\n"); i++; continue;
      case '"': PUTL(&B, "&quot;"); i++; continue;
      case '\'': PUTL(&B, "&#39;"); i++; continue;
      case '&': PUTL(&B, "&amp;"); i++; continue;
      case '<': PUTL(&B, "&lt;"); i++; continue;
      case '>': PUTL(&B, "&gt;"); i++; continue;
    }
    if (c == '\\' && i + 1 < s.n && ESCAPABLE[(u8)s.p[i + 1]]) {
      emit_hj(&B, sub(s, i + 1, i + 2));
      i += 2;
      continue;
    }
    if (c == '`') {
      long j = find_ch(s, '`', i + 1);
      if (j > i + 1) {
        PUTL(&B, "<code>");
        emit_hj(&B, sub(s, i + 1, j));
        PUTL(&B, "</code>");
        i = j + 1;
      } else {
        putch(&B, '`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.n && s.p[i + 1] == '*') {
        long j = find_unescaped(s, "**", 2, i + 2);
        if (j > i + 2) {
          PUTL(&B, "<strong>");
          inline_md(sub(s, i + 2, j));
          PUTL(&B, "</strong>");
          i = j + 2;
        } else {
          PUTL(&B, "**");
          i += 2;
        }
        continue;
      }
      long j = find_single_star(s, i + 1);
      if (j > i + 1) {
        PUTL(&B, "<em>");
        inline_md(sub(s, i + 1, j));
        PUTL(&B, "</em>");
        i = j + 1;
      } else {
        putch(&B, '*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, "]", 1, i + 1);
      long m = k != -1 && k + 1 < s.n && s.p[k + 1] == '(' ? find_ch(s, ')', k + 2) : -1;
      Str target = m == -1 ? LIT("") : sub(s, k + 2, m);
      if (target.n > 0 && !has_js_ws(target)) {
        PUTL(&B, "<a href=\\\"");
#ifdef PROF
        unsigned long long _l0 = rdtsc();
#endif
        emit_link(target);
#ifdef PROF
        prof_t[12] += rdtsc() - _l0;
#endif
        PUTL(&B, "\\\">");
        inline_md(sub(s, i + 1, k));
        PUTL(&B, "</a>");
        i = m + 1;
        continue;
      }
      putch(&B, '[');
      i++;
      continue;
    }
    emit_hj(&B, sub(s, i, i + 1));
    i++;
  }
}

typedef struct { Str *ls; long n; } Lines;
static inline int is_blank(Str l) { return all_sp_tab(l); }
/** ^```([A-Za-z0-9_+-]*)[ \t]*$ : the language, or -1 */
static int fence_open(Str l, Str *lang) {
  if (l.n < 3 || l.p[0] != '`' || l.p[1] != '`' || l.p[2] != '`') return 0;
  long i = 3;
  while (i < l.n) {
    char c = l.p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') i++;
    else break;
  }
  if (!all_sp_tab(sub(l, i, l.n))) return 0;
  *lang = sub(l, 3, i);
  return 1;
}
static int fence_close(Str l) { return l.n >= 3 && l.p[0] == '`' && l.p[1] == '`' && l.p[2] == '`' && all_sp_tab(sub(l, 3, l.n)); }
/** ^(#{1,6})[ \t]+(.*?)[ \t]*$ with a text that is not empty: the depth, or 0 */
static int heading_of(Str l, Str *text) {
  long h = 0;
  while (h < l.n && l.p[h] == '#') h++;
  if (h < 1 || h > 6) return 0;
  if (h >= l.n || !sp_tab(l.p[h])) return 0;
  Str t = trim_sp_tab(sub(l, h, l.n));
  if (t.n == 0) return 0;
  *text = t;
  return (int)h;
}
static int is_hr(Str l) {
  if (l.n < 3) return 0;
  if (!((l.p[0] == '-' && l.p[1] == '-' && l.p[2] == '-') || (l.p[0] == '*' && l.p[1] == '*' && l.p[2] == '*'))) return 0;
  return all_sp_tab(sub(l, 3, l.n));
}
static int ends_paragraph(Str l) {
  Str x;
  if (is_blank(l)) return 1;
  if (l.n == 0) return 1;
  char c = l.p[0];
  if (c == '`' && fence_open(l, &x)) return 1;
  if (c == '#' && heading_of(l, &x)) return 1;
  if ((c == '-' || c == '*') && is_hr(l)) return 1;
  return 0;
}
/** ^[-*] (.*)$ */
static int bullet_of(Str l, Str *rest) {
  if (l.n >= 2 && (l.p[0] == '-' || l.p[0] == '*') && l.p[1] == ' ') {
    *rest = sub(l, 2, l.n);
    return 1;
  }
  return 0;
}
/** ^(\d{1,9})\. (.*)$ */
static int ordered_of(Str l, Str *rest, long *num) {
  long d = 0;
  while (d < l.n && l.p[d] >= '0' && l.p[d] <= '9') d++;
  if (d < 1 || d > 9) return 0;
  if (d + 1 >= l.n || l.p[d] != '.' || l.p[d + 1] != ' ') return 0;
  long v = 0;
  for (long k = 0; k < d; k++) v = v * 10 + (l.p[k] - '0');
  *num = v;
  *rest = sub(l, d + 2, l.n);
  return 1;
}
/** ^ {2,}\S */
static int continued(Str l) {
  long k = 0;
  while (k < l.n && l.p[k] == ' ') k++;
  return k >= 2 && k < l.n && !js_ws(l.p[k]);
}
/** lines (trimmed by the caller or not) joined with "\n", in A */
static Str join_lines(Str *ls, long n) {
  long tot = n > 0 ? n - 1 : 0;
  for (long k = 0; k < n; k++) tot += ls[k].n;
  char *p = alloc(tot + 1);
  long at = 0;
  for (long k = 0; k < n; k++) {
    if (k) p[at++] = '\n';
    memcpy(p + at, ls[k].p, ls[k].n);
    at += ls[k].n;
  }
  return (Str){ p, tot };
}

/** s's body rendered: html, toc and excerpt (makefun-site's renderMarkdown). */
#ifdef PROF
static void inline_top(Str x) { unsigned long long t = rdtsc(); inline_md(x); prof_t[13] += rdtsc() - t; prof_t[14] += x.n; }
#else
#define inline_top inline_md
#endif
static void render_md(Src *s) {
  Str src = s->body;
  // the lines (a bound first: one per 1 + newline, counted 16 bytes at a time), and the words
  long nl = 1, words = 0;
  {
    const __m128i NL = _mm_set1_epi8('\n'), SP = _mm_set1_epi8(' '), LO = _mm_set1_epi8(8), HI = _mm_set1_epi8(14);
    unsigned prev_ws = 1;
    long k = 0;
    for (; k + 16 <= src.n; k += 16) {
      __m128i x = _mm_loadu_si128((const __m128i *)(src.p + k));
      nl += __builtin_popcount(_mm_movemask_epi8(_mm_cmpeq_epi8(x, NL)));
      // whitespace: space or 9..13
      __m128i ws = _mm_or_si128(_mm_cmpeq_epi8(x, SP), _mm_and_si128(_mm_cmpgt_epi8(x, LO), _mm_cmplt_epi8(x, HI)));
      unsigned w = (unsigned)_mm_movemask_epi8(ws);
      unsigned nonws = ~w & 0xFFFF;
      unsigned shifted = ((w << 1) | prev_ws) & 0xFFFF; // was the byte before whitespace (or the start)
      words += __builtin_popcount(nonws & shifted);
      prev_ws = (w >> 15) & 1;
    }
    for (; k < src.n; k++) {
      nl += src.p[k] == '\n';
      int w = js_ws(src.p[k]);
      if (!w && prev_ws) words++;
      prev_ws = w;
    }
  }
  long m = (words + 199) / 200;
  s->minutes = m < 1 ? 1 : m;
  Str *L = alloc(nl * sizeof(Str));
  {
    long st = 0, li = 0;
    for (;;) {
      long e = find_ch(src, '\n', st);
      if (e == -1) {
        L[li++] = sub(src, st, src.n);
        break;
      }
      L[li++] = sub(src, st, e);
      st = e + 1;
    }
  }
  Toc *toc = alloc(nl * sizeof(Toc));
  long ntoc = 0;
  Str *ids = alloc(nl * sizeof(Str));
  long nids = 0;
  Str *tmp = alloc(nl * sizeof(Str));
  int have_excerpt = 0;
  Str excerpt = LIT("");
  char *start = B.cur;
  long i = 0;
  while (i < nl) {
    Str line = L[i];
    if (is_blank(line)) {
      i++;
      continue;
    }
    char c0 = line.p[0];
    Str lang;
    if (c0 == '`' && fence_open(line, &lang)) {
      i++;
      PUTL(&B, "<pre><code");
      if (lang.n > 0) {
        PUTL(&B, " class=\\\"language-");
        emit_hj(&B, lang);
        PUTL(&B, "\\\"");
      }
      putch(&B, '>');
      while (i < nl && !fence_close(L[i])) {
        emit_hj(&B, L[i]);
        PUTL(&B, "\\n");
        i++;
      }
      i++;
      PUTL(&B, "</code></pre>\\n");
      continue;
    }
    Str htext;
    int depth = c0 == '#' ? heading_of(line, &htext) : 0;
    if (depth) {
      Str slug = slugify(htext);
      Str id = slug;
      for (long n = 2;; n++) {
        int taken = 0;
        for (long q = 0; q < nids; q++)
          if (str_eq(ids[q], id)) {
            taken = 1;
            break;
          }
        if (!taken) break;
        id = CAT(slug, LIT("-"), num_str(n));
      }
      ids[nids++] = id;
      PUTL(&B, "<h");
      putch(&B, '0' + depth);
      PUTL(&B, " id=\\\"");
      put(&B, id.p, id.n);
      PUTL(&B, "\\\">");
      char *h0 = B.cur;
      inline_top(htext);
      Str html = { h0, B.cur - h0 };
      PUTL(&B, "</h");
      putch(&B, '0' + depth);
      PUTL(&B, ">\\n");
      if (depth == 2 || depth == 3) toc[ntoc++] = (Toc){ depth, id, html };
      i++;
      continue;
    }
    if ((c0 == '-' || c0 == '*') && is_hr(line)) {
      PUTL(&B, "<hr>\\n");
      i++;
      continue;
    }
    if (c0 == '>') {
      long n = 0;
      while (i < nl && L[i].n > 0 && L[i].p[0] == '>') {
        Str l = L[i++];
        l = sub(l, (l.n > 1 && l.p[1] == ' ') ? 2 : 1, l.n);
        l = trim_sp_tab(l);
        if (l.n > 0) tmp[n++] = l;
      }
      if (n == 0) PUTL(&B, "<blockquote>\\n</blockquote>\\n");
      else {
        PUTL(&B, "<blockquote>\\n<p>");
        inline_top(join_lines(tmp, n));
        PUTL(&B, "</p>\\n</blockquote>\\n");
      }
      continue;
    }
    Str rest;
    long num = 1;
    int bullet = bullet_of(line, &rest);
    int ordered = !bullet && ordered_of(line, &rest, &num);
    if (bullet || ordered) {
      long start_num = num;
      if (ordered) {
        if (start_num != 1) {
          PUTL(&B, "<ol start=\\\"");
          putnum(&B, start_num);
          PUTL(&B, "\\\">\\n");
        } else
          PUTL(&B, "<ol>\\n");
      } else
        PUTL(&B, "<ul>\\n");
      long n = 0; // parts of the current item
      int open_item = 0;
      while (i < nl) {
        Str r;
        long nn;
        int m = ordered ? ordered_of(L[i], &r, &nn) : bullet_of(L[i], &r);
        if (m) {
          if (open_item) {
            PUTL(&B, "<li>");
            inline_top(join_lines(tmp, n));
            PUTL(&B, "</li>\\n");
          }
          n = 0;
          tmp[n++] = trim_sp_tab(r);
          open_item = 1;
          i++;
        } else if (continued(L[i])) {
          tmp[n++] = trim_sp_tab(L[i]);
          i++;
        } else
          break;
      }
      if (open_item) {
        PUTL(&B, "<li>");
        inline_top(join_lines(tmp, n));
        PUTL(&B, "</li>\\n");
      }
      if (ordered) PUTL(&B, "</ol>\\n");
      else PUTL(&B, "</ul>\\n");
      continue;
    }
    long n = 0;
    tmp[n++] = trim_sp_tab(line);
    i++;
    while (i < nl && !ends_paragraph(L[i])) tmp[n++] = trim_sp_tab(L[i++]);
    PUTL(&B, "<p>");
    char *p0 = B.cur;
    inline_top(join_lines(tmp, n));
    if (!have_excerpt) {
      excerpt = (Str){ p0, B.cur - p0 };
      have_excerpt = 1;
    }
    PUTL(&B, "</p>\\n");
  }
  s->html = (Str){ start, B.cur - start };
  s->toc = toc;
  s->ntoc = ntoc;
  s->excerpt = excerpt;
}

// ---------------------------------------------------------------- the build

typedef struct { Str path, content; } File;
static int cmp_file(const void *a, const void *b) { return str_cmp(((const File *)a)->path, ((const File *)b)->path); }
static int cmp_str(const void *a, const void *b) { return str_cmp(*(const Str *)a, *(const Str *)b); }
static int cmp_tpl(const void *a, const void *b) { return str_cmp(((const Tpl *)a)->name, ((const Tpl *)b)->name); }
static int cmp_post(const void *a, const void *b) {
  const Src *x = a, *y = b;
  int c = str_cmp(y->date, x->date);
  return c != 0 ? c : str_cmp(x->slug, y->slug);
}
static int cmp_tag(const void *a, const void *b) {
  const Tag *x = a, *y = b;
  if (x->count != y->count) return y->count > x->count ? 1 : -1;
  return str_cmp(x->slug, y->slug);
}

static Map outkeys;
static int first_entry;
static void entry_begin(Str key) {
  set_add(&outkeys, key);
  if (!first_entry) putch(&O, ',');
  first_entry = 0;
  putch(&O, '"');
  emit_j(&O, key);
  PUTL(&O, "\":\"");
}
static inline void entry_end(void) { putch(&O, '"'); }

static Str dir_of(Str p) {
  long k = p.n - 1;
  while (k >= 0 && p.p[k] != '/') k--;
  return k < 0 ? LIT("") : sub(p, 0, k);
}

static Str index_url(long k) { return k == 1 ? CAT(base, LIT("/")) : CAT(base, LIT("/page/"), num_str(k), LIT("/")); }

static long tpl_index(Map *tplmap, Str name) {
  void **v = map_get(tplmap, name);
  return v ? (long)(intptr_t)*v : -1;
}

/** One case: its answer, written into O as a JSON object. */
static void build_case(JV *input) {
  PHASE(0);
  JV *site = jget(input, "site"), *templates = jget(input, "templates"), *files = jget(input, "files");
  if (!site || !templates || !files || templates->t != J_OBJ || files->t != J_ARR) unsupported();
  JV *title = jget(site, "title"), *base_url = jget(site, "base_url"), *per_page = jget(site, "per_page"), *recent_n = jget(site, "recent");
  if (!title || title->t != J_STR || !base_url || base_url->t != J_STR) unsupported();
  if (!per_page || per_page->t != J_NUM || !per_page->isint || per_page->num < 1) unsupported();
  if (!recent_n || recent_n->t != J_NUM || !recent_n->isint || recent_n->num < 0) unsupported();

  map_init(&names, 128);
  nnames = 0;
  for (int f = 0; f < F_COUNT; f++) {
    long n = 0;
    while (FIELD_NAMES[f][n]) n++;
    intern((Str){ FIELD_NAMES[f], n });
  }

  // the report's lines (refusals, drafts, template errors, shadowed files)
  long rcap = 64, nrep = 0;
  Str *rep = alloc(rcap * sizeof(Str));
#define REPORT(s)                                       \
  do {                                                  \
    if (nrep == rcap) {                                 \
      Str *nr = alloc(rcap * 2 * sizeof(Str));          \
      memcpy(nr, rep, nrep * sizeof(Str));              \
      rep = nr;                                         \
      rcap *= 2;                                        \
    }                                                   \
    rep[nrep++] = (s);                                  \
  } while (0)

  // templates, in code-unit order of their names (a repeated name: JSON.parse keeps the last, at the first's place)
  ntpls = templates->n;
  tpls = alloc((ntpls ? ntpls : 1) * sizeof(Tpl));
  Map tplmap;
  map_init(&tplmap, ntpls + 1);
  {
    long n = 0;
    for (long k = 0; k < templates->n; k++) {
      if (templates->kids[k].t != J_STR) unsupported();
      int nw;
      void **slot = map_put(&tplmap, templates->keys[k], &nw);
      if (!nw) unsupported();
      *slot = 0;
      tpls[n].name = templates->keys[k];
      tpls[n].nodes = (Node *)&templates->kids[k]; // the source, for now
      n++;
    }
    void **order = alloc((ntpls ? ntpls : 1) * sizeof(void *));
    for (long k = 0; k < ntpls; k++) order[k] = &tpls[k];
    msort(order, ntpls, cmp_tpl);
    Tpl *sorted = alloc((ntpls ? ntpls : 1) * sizeof(Tpl));
    for (long k = 0; k < ntpls; k++) sorted[k] = *(Tpl *)order[k];
    tpls = sorted;
    map_init(&tplmap, ntpls + 1);
    for (long k = 0; k < ntpls; k++) {
      Str src = ((JV *)tpls[k].nodes)->s;
      int ok;
      terr = LIT("");
      Node *nodes = parse_template(tpls[k].name, src, &ok);
      tpls[k].ok = ok;
      tpls[k].nodes = ok ? nodes : 0;
      if (ok) {
        int nw;
        *map_put(&tplmap, tpls[k].name, &nw) = (void *)(intptr_t)k;
      } else
        REPORT(terr);
    }
    // includes: the template each names, where it parsed
    for (long k = 0; k < ntpls; k++) {
      if (!tpls[k].ok) continue;
      // walk every node
      Node *stack[512];
      int sp = 0;
      if (tpls[k].nodes) stack[sp++] = tpls[k].nodes;
      while (sp > 0) {
        Node *n = stack[--sp];
        for (; n; n = n->next) {
          if (n->kind == N_INC) n->tpl = (int)tpl_index(&tplmap, n->text);
          if (n->body) {
            if (sp >= 511) unsupported();
            stack[sp++] = n->body;
          }
          if (n->els) {
            if (sp >= 511) unsupported();
            stack[sp++] = n->els;
          }
        }
      }
    }
    chunk_n = 0;
    for (long k = 0; k < ntpls; k++)
      if (tpls[k].ok) tpls[k].nodes = chunkify(tpls[k].nodes);
    chunk_text = allocz((chunk_n * 9 + 1) * sizeof(Str));
  }

  PHASE(1);
  base = base_url->s;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;
  Site *st = alloc(sizeof(Site));
  st->title = title->s;
  st->base = base;

  // 1. the sources, in code-unit order of their paths
  long nf = files->n;
  File *fl = alloc((nf ? nf : 1) * sizeof(File));
  void **fo = alloc((nf ? nf : 1) * sizeof(void *));
  for (long k = 0; k < nf; k++) {
    JV *f = &files->kids[k];
    JV *p = jget(f, "path"), *c = jget(f, "content");
    if (!p || p->t != J_STR || !c || c->t != J_STR) unsupported();
    fl[k].path = p->s;
    fl[k].content = c->s;
    fo[k] = &fl[k];
  }
  msort(fo, nf, cmp_file);
  Src **srcs = alloc((nf ? nf : 1) * sizeof(Src *));
  File **statics = alloc((nf ? nf : 1) * sizeof(File *));
  long nsrc = 0, nstat = 0;
  for (long k = 0; k < nf; k++) {
    File *f = fo[k];
    if (ends(f->path, ".md", 3)) {
      Src *s = allocz(sizeof(Src));
      s->path = f->path;
      Str t = f->content;
      if (find_ch(t, '\r', 0) != -1) {
        char *b = alloc(t.n + 1);
        long n = 0;
        for (long j = 0; j < t.n; j++) {
          if (t.p[j] == '\r') {
            b[n++] = '\n';
            if (j + 1 < t.n && t.p[j + 1] == '\n') j++;
          } else
            b[n++] = t.p[j];
        }
        t = (Str){ b, n };
      }
      s->text = t;
      srcs[nsrc++] = s;
    } else
      statics[nstat++] = f;
  }

  // 2. front matter
  long nkept = 0;
  for (long k = 0; k < nsrc; k++) {
    Src *s = srcs[k];
    Str err;
    if (!front_matter(s, &err)) {
      REPORT(CAT(LIT("refused "), s->path, LIT(": "), err));
      continue;
    }
    s->post = STARTS(s->path, "posts/");
    if (STARTS(s->path, "tags/")) {
      REPORT(CAT(LIT("refused "), s->path, LIT(": tags/ is the generator's own")));
      continue;
    }
    if (STARTS(s->path, "page/")) {
      REPORT(CAT(LIT("refused "), s->path, LIT(": page/ is the generator's own")));
      continue;
    }
    if (s->post && !s->has_date) {
      REPORT(CAT(LIT("refused "), s->path, LIT(": a post needs a date")));
      continue;
    }
    Str layout = s->has_layout ? s->layout : s->post ? LIT("post") : LIT("page");
    long ti = tpl_index(&tplmap, layout);
    if (ti < 0) {
      REPORT(CAT(LIT("refused "), s->path, LIT(": layout \""), layout, LIT("\" is no template that parses")));
      continue;
    }
    s->tpl = (int)ti;
    if (s->draft) {
      REPORT(CAT(LIT("draft "), s->path));
      continue;
    }
    srcs[nkept++] = s;
  }
  nsrc = nkept;

  PHASE(2);
  // 3. permalinks
  Map taken;
  map_init(&taken, nsrc + 8);
  set_add(&taken, LIT("\x01posts"));
  set_add(&taken, LIT("\x01tags"));
  set_add(&taken, LIT("\x01page"));
  map_init(&by_source, nsrc + 1);
  for (long k = 0; k < nsrc; k++) {
    Src *s = srcs[k];
    Str dir = s->post ? LIT("posts") : dir_of(s->path);
    Str slug = s->has_slug ? s->slugfm : slugify(s->title);
    s->slug = unique_slug(slug, &taken, CAT(dir, LIT("\x01")));
    s->outdir = dir.n == 0 ? s->slug : CAT(dir, LIT("/"), s->slug);
    s->url = CAT(base, LIT("/"), s->outdir, LIT("/"));
    int nw;
    *map_put(&by_source, s->path, &nw) = s;
  }

  PHASE(3);
  // 4. Markdown
  map_init(&static_paths, nstat + 1);
  for (long k = 0; k < nstat; k++) set_add(&static_paths, statics[k]->path);
  map_init(&broken, 256);
  nbroken = 0;
  broken_cap = 0;
  broken_list = 0;
  Env e;
  env = &e;
  for (long k = 0; k < nsrc; k++) {
    Src *s = srcs[k];
    e.s = s;
    e.dir = dir_of(s->path);
    render_md(s);
    if (s->has_date) s->date_long = long_date(s->date);
  }

  PHASE(4);
  // 5. collections
  void **posts = alloc((nsrc ? nsrc : 1) * sizeof(void *));
  long nposts = 0;
  for (long k = 0; k < nsrc; k++)
    if (srcs[k]->post) posts[nposts++] = srcs[k];
  msort(posts, nposts, cmp_post);
  for (long k = 0; k < nposts; k++) {
    Src *p = posts[k];
    p->newer = k == 0 ? 0 : posts[k - 1];
    p->older = k == nposts - 1 ? 0 : posts[k + 1];
  }

  // 6. tags
  Map by_slug;
  map_init(&by_slug, 64);
  long ntags = 0;
  Tag **tag_list = alloc(8 * sizeof(Tag *));
  long tag_cap = 8;
  for (int pass = 0; pass < 2; pass++) {
    for (long k = 0; k < nsrc; k++) {
      Src *s = pass == 0 ? (k < nposts ? posts[k] : 0) : srcs[k];
      if (s == 0) continue;
      if (pass == 1 && s->post) continue;
      s->trefs = alloc((s->ntags ? s->ntags : 1) * sizeof(Tag *));
      s->ntrefs = 0;
      Str *seen = alloc((s->ntags ? s->ntags : 1) * sizeof(Str));
      long nseen = 0;
      for (long j = 0; j < s->ntags; j++) {
        Str name = s->tags[j];
        Str slug = slugify(name);
        int dup = 0;
        for (long q = 0; q < nseen; q++)
          if (str_eq(seen[q], slug)) dup = 1;
        if (dup) continue;
        seen[nseen++] = slug;
        void **tv = map_get(&by_slug, slug);
        Tag *t;
        if (pass == 0) {
          if (tv == 0) {
            t = allocz(sizeof(Tag));
            t->name = name;
            t->slug = slug;
            t->url = CAT(base, LIT("/tags/"), slug, LIT("/"));
            int nw;
            *map_put(&by_slug, slug, &nw) = t;
            if (ntags == tag_cap) {
              Tag **nt = alloc(tag_cap * 2 * sizeof(Tag *));
              memcpy(nt, tag_list, ntags * sizeof(Tag *));
              tag_list = nt;
              tag_cap *= 2;
            }
            tag_list[ntags++] = t;
          } else
            t = *tv;
          t->count++;
        } else if (tv)
          t = *tv;
        else {
          t = allocz(sizeof(Tag));
          t->name = name;
          t->slug = slug;
          t->url = LIT("");
        }
        s->trefs[s->ntrefs++] = t;
      }
    }
  }
  for (long k = 0; k < ntags; k++) tag_list[k]->posts = alloc((tag_list[k]->count ? tag_list[k]->count : 1) * sizeof(Src *));
  for (long k = 0; k < nposts; k++) {
    Src *p = posts[k];
    for (long j = 0; j < p->ntrefs; j++) {
      Tag *t = p->trefs[j];
      t->posts[t->fill++] = p;
    }
  }
  msort((void **)tag_list, ntags, cmp_tag);

  PHASE(5);
  // 7. layouts
  map_init(&outkeys, nsrc + ntags + nposts / 1 + 16);
  first_entry = 1;
  putch(&O, '{');
  Scope root;
  memset(&root, 0, sizeof root);
  root.root = 1;
  root.site = vobj(K_SITE, st);
  root.tags = varr(K_TALL, tag_list, ntags);
  long nrecent = recent_n->num < nposts ? recent_n->num : nposts;
  root.recent = varr(K_CARD, posts, nrecent);
  root.e1 = root.e2 = -1;
  for (long k = 0; k < nsrc; k++) {
    Src *s = srcs[k];
    entry_begin(CAT(s->outdir, LIT("/index.html")));
    root.e1 = F_page;
    root.v1 = vobj(K_PAGE, s);
    render(tpls[s->tpl].nodes, &root, 0);
    entry_end();
  }
  PHASE(6);
  long per = per_page->num;
  long pages = nposts == 0 ? 1 : (nposts + per - 1) / per;
  long ti_index = tpl_index(&tplmap, LIT("index"));
  if (ti_index >= 0)
    for (long k = 1; k <= pages; k++) {
      Pager *pg = alloc(sizeof(Pager));
      pg->number = k;
      pg->total = pages;
      pg->url = index_url(k);
      pg->newer_url = k == 1 ? LIT("") : index_url(k - 1);
      pg->older_url = k == pages ? LIT("") : index_url(k + 1);
      long lo = (k - 1) * per, hi = k * per < nposts ? k * per : nposts;
      if (lo > nposts) lo = nposts;
      entry_begin(k == 1 ? LIT("index.html") : CAT(LIT("page/"), num_str(k), LIT("/index.html")));
      root.e1 = F_posts;
      root.v1 = varr(K_CARD, posts + lo, hi - lo);
      root.e2 = F_pager;
      root.v2 = vobj(K_PAGER, pg);
      render(tpls[ti_index].nodes, &root, 0);
      entry_end();
    }
  root.e2 = -1;
  long ti_tag = tpl_index(&tplmap, LIT("tag"));
  if (ti_tag >= 0)
    for (long k = 0; k < ntags; k++) {
      Tag *t = tag_list[k];
      entry_begin(CAT(LIT("tags/"), t->slug, LIT("/index.html")));
      root.e1 = F_tag;
      root.v1 = vobj(K_TFULL, t);
      render(tpls[ti_tag].nodes, &root, 0);
      entry_end();
    }
  long ti_tags = tpl_index(&tplmap, LIT("tags"));
  if (ti_tags >= 0) {
    entry_begin(LIT("tags/index.html"));
    root.e1 = -1;
    render(tpls[ti_tags].nodes, &root, 0);
    entry_end();
  }
  for (long k = 0; k < nstat; k++) {
    File *f = statics[k];
    if (map_has(&outkeys, f->path)) {
      REPORT(CAT(LIT("shadowed "), f->path, LIT(": a built page has its path")));
      continue;
    }
    if (EQL(f->path, "build-report.txt")) continue; // the report takes its place
    entry_begin(f->path);
    emit_j(&O, f->content);
    entry_end();
  }

  PHASE(7);
  // 8. the report
  long nl = nrep + nbroken;
  void **lines = alloc((nl ? nl : 1) * sizeof(void *));
  for (long k = 0; k < nrep; k++) lines[k] = &rep[k];
  for (long k = 0; k < nbroken; k++) lines[nrep + k] = &broken_list[k];
  msort(lines, nl, cmp_str);
  long npages = 0;
  for (long k = 0; k < nsrc; k++) npages += !srcs[k]->post;
  entry_begin(LIT("build-report.txt"));
  for (long k = 0; k < nl; k++) {
    emit_j(&O, *(Str *)lines[k]);
    PUTL(&O, "\\n");
  }
  PUTL(&O, "built ");
  putnum(&O, npages);
  PUTL(&O, " pages, ");
  putnum(&O, nposts);
  PUTL(&O, " posts, ");
  putnum(&O, ntags);
  PUTL(&O, " tags and ");
  putnum(&O, nstat);
  PUTL(&O, " static files\\n");
  entry_end();
  putch(&O, '}');
  PHASE(8);
#undef REPORT
}

// ---------------------------------------------------------------- Node-API

static int ready;
static int own_fd;
static long out_fd = -1;
static napi_value build_inner(napi_env envp, napi_callback_info info);
/** build: the cases (bytes or path), slugs' plainOf, and the answers' fd or path (see build_inner). */
static napi_value build(napi_env envp, napi_callback_info info) {
  own_fd = 0;
  napi_value r = build_inner(envp, info);
  if (own_fd) sys3(3, out_fd, 0, 0); // close
  return r;
}
static napi_value build_inner(napi_env envp, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3], result, nul;
  napi_get_cb_info(envp, info, &argc, argv, 0, 0);
  napi_get_null(envp, &nul);
  if (argc < 2) return nul;
  void *data;
  size_t len;
  // the cases: a Buffer of cases.json's bytes, or cases.json's path (mapped, never copied)
  napi_valuetype t0;
  napi_typeof(envp, argv[0], &t0);
  if (t0 == napi_string) {
    char path[4096];
    size_t pn;
    if (napi_get_value_string_utf8(envp, argv[0], path, sizeof path, &pn) != napi_ok || pn >= sizeof path - 1) return nul;
    long in = sys3(2, (long)path, 0, 0); // open(O_RDONLY)
    if (in < 0) return nul;
    long st[18];
    if (sys3(5, in, (long)st, 0) != 0) return nul; // fstat
    len = (size_t)st[6]; // st_size
    if (len == 0) return nul;
    register long r10 __asm__("r10") = 0x02 | 0x8000; // MAP_PRIVATE | MAP_POPULATE
    register long r8 __asm__("r8") = in;
    register long r9 __asm__("r9") = 0;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(9), "D"(0), "S"(len), "d"(1), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    sys3(3, in, 0, 0);
    if (r < 0 && r > -4096) return nul;
    data = (void *)r;
  } else if (napi_get_buffer_info(envp, argv[0], &data, &len) != napi_ok)
    return nul;
  if (!ready) {
    if (!arena_init(&P, (size_t)1 << 26) || !arena_init(&A, (size_t)1 << 30) || !arena_init(&B, (size_t)1 << 29) || !arena_init(&O, (size_t)1 << 31)) return nul;
    tables_init();
    const char *esc = "\\`*_[]()#+-.!>";
    for (const char *c = esc; *c; c++) ESCAPABLE[(u8)*c] = 1;
    const char *sp = "\\`*[&<>\"'";
    for (const char *c = sp; *c; c++) INLINE_SPECIAL[(u8)*c] = 1;
    for (int c = 0; c < 0x20; c++) INLINE_SPECIAL[c] = 1;
    ready = 1;
  }
  genv = envp;
  gslug = argv[1];
  // With an fd (the third argument): the answers array written straight to it, each case built in the output arena
  // from its start; true when every case was, else false (the file then holds a part: the caller writes it again).
  int64_t fd = -1;
  if (argc >= 3) {
    napi_valuetype t;
    napi_typeof(envp, argv[2], &t);
    if (t == napi_number) napi_get_value_int64(envp, argv[2], &fd);
    else if (t == napi_string) {
      // the answers' path: opened (created, truncated) here; the caller reopens it to write it again if need be
      char path[4096];
      size_t pn;
      if (napi_get_value_string_utf8(envp, argv[2], path, sizeof path, &pn) != napi_ok || pn >= sizeof path - 1) return nul;
      fd = sys3(2, (long)path, 01 | 0100 | 01000, 0644); // O_WRONLY | O_CREAT | O_TRUNC
      if (fd < 0) return nul;
      own_fd = 1;
      out_fd = fd;
    }
  }
  napi_value vfalse, vtrue;
  napi_get_boolean(envp, 0, &vfalse);
  napi_get_boolean(envp, 1, &vtrue);
  // the cases' bytes: [ {id, input}, ... ]
  const u8 *p0 = data, *pe = p0 + len;
  jp = p0;
  je = pe;
  // the cases in order: each parsed and built; one that fails is answered null and skipped by its bytes
  jp = p0;
  je = pe;
  jws();
  if (jp >= je || *jp != '[') return fd >= 0 ? vfalse : nul;
  jp++;
  jws();
  if (fd >= 0) {
    if (jp < je && *jp == ']') return write_all(fd, "[]", 2) ? vtrue : vfalse;
    for (long volatile k = 0;; k++) {
      jws();
      A.cur = A.base;
      B.cur = B.base;
      O.cur = O.base;
      if (__builtin_setjmp(jb) != 0) return vfalse;
      putch(&O, k == 0 ? '[' : ',');
      je = pe;
      JV *c = jval(0);
      const u8 *cend = jp;
      JV *input = jget(c, "input");
      if (!input || input->t != J_OBJ) unsupported();
      build_case(input);
#ifdef PROF
      if ((unsigned long)(A.cur - A.base) > prof_t[6 + 0 * 0] * 0 + 0 && 0) {}
      { unsigned long a = A.cur - A.base, b = B.cur - B.base, o = O.cur - O.base;
        if (a > peak[0]) peak[0] = a; if (b > peak[1]) peak[1] = b; if (o > peak[2]) peak[2] = o; }
#endif
      jp = cend;
      je = pe;
      jws();
      int last = jp < je && *jp == ']';
      if (last) putch(&O, ']');
      if (!write_all(fd, O.base, O.cur - O.base)) return vfalse;
      if (last) return vtrue;
      if (jp < je && *jp == ',') {
        jp++;
        continue;
      }
      return vfalse;
    }
  }
  napi_create_array(envp, &result);
  if (jp < je && *jp == ']') return result;
  for (long volatile k = 0;; k++) {
    jws();
    const u8 *volatile cstart = jp;
    const u8 *volatile cend = 0;
    A.cur = A.base;
    B.cur = B.base;
    char *volatile o0 = O.cur;
    napi_value volatile item = nul;
    if (__builtin_setjmp(jb) == 0) {
#ifdef PROF
      prof_last = rdtsc();
#endif
      je = pe;
      JV *c = jval(0);
      cend = jp;
      JV *input = jget(c, "input");
      if (!input || input->t != J_OBJ) unsupported();
      build_case(input);
      napi_value buf;
      if (napi_create_external_buffer(envp, O.cur - o0, o0, 0, 0, &buf) == napi_ok) item = buf;
      else if (napi_create_buffer_copy(envp, O.cur - o0, o0, 0, &buf) == napi_ok) item = buf;
    } else {
      O.cur = o0;
      if (__builtin_setjmp(jb) != 0) return nul; // a case whose bytes do not even skip: every case to run()
      A.cur = A.base;
      jp = cstart;
      je = pe;
      jskip();
      cend = jp;
    }
    napi_set_element(envp, result, k, item);
    jp = cend;
    je = pe;
    jws();
    if (jp < je && *jp == ',') {
      jp++;
      continue;
    }
    if (jp < je && *jp == ']') break;
    return nul;
  }
  return result;
}

#ifdef PROF
// A sampling profiler: SIGPROF every 100 µs of CPU, each sample the interrupted instruction's address.
static unsigned long samples[1 << 20];
static long nsamples;
static long sys6(long n, long a, long b, long c, long d) {
  long r;
  register long r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return r;
}
static void on_prof(int sig, void *info, void *uc) {
  (void)sig; (void)info;
  unsigned long rip = *(unsigned long *)((char *)uc + 168);
  if (nsamples < (1 << 20)) samples[nsamples++] = rip;
}
__attribute__((naked)) static void restorer(void) { __asm__ volatile("mov $15, %eax\n syscall"); }
struct ksa { void *handler; unsigned long flags; void *restorer; unsigned long mask; };
static napi_value prof_start(napi_env envp, napi_callback_info info) {
  struct ksa sa = { (void *)on_prof, 0x04000000 | 4 | 0x10000000, (void *)restorer, 0 }; // SA_RESTORER | SA_SIGINFO | SA_RESTART
  sys6(13, 27, (long)&sa, 0, 8); // rt_sigaction(SIGPROF)
  long it[4] = { 0, 100, 0, 100 }; // interval 100 µs
  sys6(38, 2, (long)it, 0, 0); // setitimer(ITIMER_PROF)
  napi_value u; napi_get_undefined(envp, &u); return u;
}
static napi_value prof_stop(napi_env envp, napi_callback_info info) {
  long it[4] = { 0, 0, 0, 0 };
  sys6(38, 2, (long)it, 0, 0);
  napi_value arr; napi_create_array(envp, &arr);
  // the first element: the address of build, to find the library's base
  napi_value v; napi_create_double(envp, (double)(unsigned long)&prof_stop, &v); napi_set_element(envp, arr, 0, v);
  for (long k = 0; k < nsamples; k++) { napi_create_double(envp, (double)samples[k], &v); napi_set_element(envp, arr, k + 1, v); }
  nsamples = 0;
  return arr;
}
static napi_value stats(napi_env envp, napi_callback_info info) {
  napi_value arr; napi_create_array(envp, &arr);
  { napi_value v; for (int k = 0; k < 3; k++) { napi_create_double(envp, (double)peak[k], &v); napi_set_element(envp, arr, 40 + k, v); } }
  for (int k = 0; k < 16; k++) { napi_value v; napi_create_double(envp, (double)prof_t[k], &v); napi_set_element(envp, arr, k, v); prof_t[k] = 0; }
  return arr;
}
#endif
NAPI_MODULE_INIT() {
  napi_value f;
#ifdef PROF
  napi_create_function(env, "profStart", NAPI_AUTO_LENGTH, prof_start, 0, &f);
  napi_set_named_property(env, exports, "profStart", f);
  napi_create_function(env, "profStop", NAPI_AUTO_LENGTH, prof_stop, 0, &f);
  napi_set_named_property(env, exports, "profStop", f);
  napi_create_function(env, "stats", NAPI_AUTO_LENGTH, stats, 0, &f);
  napi_set_named_property(env, exports, "stats", f);
#endif
  napi_create_function(env, "build", NAPI_AUTO_LENGTH, build, 0, &f);
  napi_set_named_property(env, exports, "build", f);
  return exports;
}
