/*
 * regex.c — in-house ERE subset compiled to a Thompson NFA and executed
 * with set-simulation, no backtracking (D13, SPEC §A1.6).
 *
 * Supported: literals, '.', '[...]' / '[^...]' with ranges and literal ']'
 * first, '^' (matches only offset 0), '$' (matches only end of buffer),
 * '*', '+', '?', '{m}', '{m,}', '{m,n}', '|', '(...)'.
 * Escapes: backslash before any metacharacter or backslash yields the
 * literal; \n \t \r are recognized; \d \w \s, digit backreferences and
 * lookaround are rejected at compile time with a message naming the
 * construct. Inside a bracket class a backslash is an ordinary byte
 * (POSIX bracket semantics). Patterns and input are raw bytes, not
 * codepoints; '.' matches any byte; case folding is ASCII-only.
 *
 * Bounded repetition is expanded by node duplication under a total
 * compiled-size cap of RE_MAX_INSTS instructions ("pattern too large").
 *
 * Match rule: leftmost start, then longest match for that start. The
 * simulation makes a single left-to-right pass and injects a fresh
 * start-thread at every input position until a match is found; at most
 * one thread per NFA state is kept per step, so total work is
 * O(len * insts) — linear time, adversarial patterns included.
 */

#include "sdk.h"
#include <stdlib.h>
#include <string.h>

#define RE_MAX_INSTS 4096
#define RE_MAX_NODES 8192
#define RE_MAX_DEPTH 100
#define RE_MAX_COUNT 4096

/* ---- compiled program --------------------------------------------------- */

enum {
  RI_CHAR,  /* consume one byte equal to ch (folded) */
  RI_CLASS, /* consume one byte in class bitmap cls */
  RI_ANY,   /* consume any byte */
  RI_BOL,   /* zero-width: only at offset 0 */
  RI_EOL,   /* zero-width: only at end of buffer */
  RI_SPLIT, /* epsilon to x and y */
  RI_JMP,   /* epsilon to x */
  RI_MATCH
};

typedef struct {
  uint8_t op;
  unsigned char ch;
  uint16_t cls;
  int32_t x;
  int32_t y;
} re_inst;

struct astd_regex {
  re_inst *prog;
  size_t nprog;
  uint8_t (*cls)[32];
  size_t ncls;
  int fold;
  /* Simulation scratch, allocated once at compile time and mutated
   * through these pointers even when the struct itself is accessed via a
   * const pointer. A compiled regex is therefore not shareable across
   * threads; astools-std tools are single-threaded processes. */
  uint32_t *marka;
  uint32_t *markb;
  size_t *starta;
  size_t *startb;
  uint32_t *lista;
  uint32_t *listb;
  uint32_t *stack; /* closure DFS stack, 2*nprog+2 slots */
  uint32_t *genp;  /* generation counter */
};

/* ---- parse tree --------------------------------------------------------- */

enum {
  RN_EMPTY,
  RN_CHAR,
  RN_CLASS,
  RN_ANY,
  RN_BOL,
  RN_EOL,
  RN_CAT, /* a then b (b chains further CATs; may be NULL) */
  RN_ALT, /* a or b (b chains further ALTs; never NULL) */
  RN_REP  /* a repeated m..n times; n < 0 = unbounded */
};

typedef struct rnode {
  uint8_t kind;
  unsigned char ch;
  uint16_t cls;
  int32_t m;
  int32_t n;
  struct rnode *a;
  struct rnode *b;
} rnode;

typedef struct {
  const char *p;
  int fold;
  const char *err; /* static-lifetime message */
  size_t nnodes;
  int depth;
  uint8_t (*cls)[32];
  size_t ncls;
  size_t ccap;
} rparse;

static unsigned char fold_ch(int fold, unsigned char c) {
  if (fold && c >= 'A' && c <= 'Z') return (unsigned char)(c + 32);
  return c;
}

/* Free a tree. Iterative on the b-spine so CAT/ALT chains of any length
 * never deepen the stack; recursion depth is bounded by group nesting. */
static void rnode_free(rnode *n) {
  while (n) {
    rnode *b = n->b;
    rnode_free(n->a);
    free(n);
    n = b;
  }
}

static rnode *node_new(rparse *ps, uint8_t kind) {
  rnode *n;
  if (ps->nnodes >= RE_MAX_NODES) {
    ps->err = "pattern too large";
    return NULL;
  }
  n = calloc(1, sizeof *n);
  if (!n) {
    ps->err = "out of memory";
    return NULL;
  }
  ps->nnodes++;
  n->kind = kind;
  return n;
}

static void cls_set_range(uint8_t *map, unsigned lo, unsigned hi, int fold) {
  unsigned v;
  for (v = lo; v <= hi; v++) {
    map[v >> 3] |= (uint8_t)(1u << (v & 7));
    if (fold) {
      if (v >= 'A' && v <= 'Z')
        map[(v + 32) >> 3] |= (uint8_t)(1u << ((v + 32) & 7));
      else if (v >= 'a' && v <= 'z')
        map[(v - 32) >> 3] |= (uint8_t)(1u << ((v - 32) & 7));
    }
  }
}

static int cls_add(rparse *ps, const uint8_t map[32]) {
  if (ps->ncls >= 65535) {
    ps->err = "pattern too large";
    return -1;
  }
  if (ps->ncls == ps->ccap) {
    size_t nc = ps->ccap ? ps->ccap * 2 : 8;
    uint8_t(*p)[32] = realloc(ps->cls, nc * 32);
    if (!p) {
      ps->err = "out of memory";
      return -1;
    }
    ps->cls = p;
    ps->ccap = nc;
  }
  memcpy(ps->cls[ps->ncls], map, 32);
  return (int)ps->ncls++;
}

static rnode *parse_class(rparse *ps) {
  uint8_t map[32];
  int neg = 0, first = 1, idx;
  rnode *n;
  memset(map, 0, sizeof map);
  ps->p++; /* '[' */
  if (*ps->p == '^') {
    neg = 1;
    ps->p++;
  }
  for (;;) {
    unsigned char lo, hi;
    char c = *ps->p;
    if (c == '\0') {
      ps->err = "unterminated character class (missing ']')";
      return NULL;
    }
    if (c == ']' && !first) {
      ps->p++;
      break;
    }
    first = 0;
    if (c == '[' && ps->p[1] == ':') {
      ps->err = "POSIX [:class:] classes are not supported";
      return NULL;
    }
    lo = (unsigned char)c;
    ps->p++;
    if (ps->p[0] == '-' && ps->p[1] != ']' && ps->p[1] != '\0') {
      hi = (unsigned char)ps->p[1];
      ps->p += 2;
      if (lo > hi) {
        ps->err = "invalid range in character class";
        return NULL;
      }
      cls_set_range(map, lo, hi, ps->fold);
    } else {
      cls_set_range(map, lo, lo, ps->fold);
    }
  }
  if (neg) {
    size_t i;
    for (i = 0; i < 32; i++) map[i] = (uint8_t)~map[i];
  }
  idx = cls_add(ps, map);
  if (idx < 0) return NULL;
  n = node_new(ps, RN_CLASS);
  if (!n) return NULL;
  n->cls = (uint16_t)idx;
  return n;
}

static rnode *parse_alt(rparse *ps); /* fwd */

static rnode *parse_atom(rparse *ps) {
  rnode *n;
  char c = *ps->p;
  switch (c) {
    case '(': {
      rnode *inner;
      if (ps->depth >= RE_MAX_DEPTH) {
        ps->err = "groups nested too deeply";
        return NULL;
      }
      ps->depth++;
      ps->p++;
      inner = parse_alt(ps);
      if (!inner) return NULL;
      if (*ps->p != ')') {
        rnode_free(inner);
        ps->err = "unterminated group (missing ')')";
        return NULL;
      }
      ps->p++;
      ps->depth--;
      return inner;
    }
    case '.':
      ps->p++;
      return node_new(ps, RN_ANY);
    case '^':
      ps->p++;
      return node_new(ps, RN_BOL);
    case '$':
      ps->p++;
      return node_new(ps, RN_EOL);
    case '[':
      return parse_class(ps);
    case '*':
    case '+':
    case '?':
    case '{':
      ps->err = "quantifier has nothing to repeat";
      return NULL;
    case '\\': {
      char e = ps->p[1];
      unsigned char lit;
      if (e == '\0') {
        ps->err = "trailing backslash";
        return NULL;
      }
      if (e == 'n') {
        lit = '\n';
      } else if (e == 't') {
        lit = '\t';
      } else if (e == 'r') {
        lit = '\r';
      } else if (e == 'd') {
        ps->err = "escape \\d is not supported (use [0-9])";
        return NULL;
      } else if (e == 'w') {
        ps->err = "escape \\w is not supported (use [A-Za-z0-9_])";
        return NULL;
      } else if (e == 's') {
        ps->err = "escape \\s is not supported (use a bracket class)";
        return NULL;
      } else if (e >= '0' && e <= '9') {
        ps->err = "backreferences are not supported";
        return NULL;
      } else if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z')) {
        ps->err = "unsupported escape sequence";
        return NULL;
      } else {
        lit = (unsigned char)e;
      }
      ps->p += 2;
      n = node_new(ps, RN_CHAR);
      if (n) n->ch = fold_ch(ps->fold, lit);
      return n;
    }
    default:
      ps->p++;
      n = node_new(ps, RN_CHAR);
      if (n) n->ch = fold_ch(ps->fold, (unsigned char)c);
      return n;
  }
}

static int parse_uint(rparse *ps, const char **pp, int32_t *out) {
  const char *q = *pp;
  int64_t v = 0;
  if (*q < '0' || *q > '9') {
    ps->err = "malformed {m,n} repetition";
    return -1;
  }
  while (*q >= '0' && *q <= '9') {
    v = v * 10 + (*q - '0');
    if (v > RE_MAX_COUNT) {
      ps->err = "repetition count too large";
      return -1;
    }
    q++;
  }
  *out = (int32_t)v;
  *pp = q;
  return 0;
}

static rnode *parse_piece(rparse *ps) {
  rnode *atom = parse_atom(ps);
  if (!atom) return NULL;
  for (;;) {
    char c = *ps->p;
    int32_t m, n;
    rnode *rep;
    if (c == '*') {
      m = 0;
      n = -1;
      ps->p++;
    } else if (c == '+') {
      m = 1;
      n = -1;
      ps->p++;
    } else if (c == '?') {
      m = 0;
      n = 1;
      ps->p++;
    } else if (c == '{') {
      const char *q = ps->p + 1;
      if (parse_uint(ps, &q, &m) != 0) {
        rnode_free(atom);
        return NULL;
      }
      if (*q == '}') {
        n = m;
        q++;
      } else if (*q == ',') {
        q++;
        if (*q == '}') {
          n = -1;
          q++;
        } else {
          if (parse_uint(ps, &q, &n) != 0 || *q != '}') {
            if (!ps->err) ps->err = "malformed {m,n} repetition";
            rnode_free(atom);
            return NULL;
          }
          q++;
          if (n < m) {
            ps->err = "invalid repetition range (m greater than n)";
            rnode_free(atom);
            return NULL;
          }
        }
      } else {
        ps->err = "malformed {m,n} repetition";
        rnode_free(atom);
        return NULL;
      }
      ps->p = q;
    } else {
      break;
    }
    rep = node_new(ps, RN_REP);
    if (!rep) {
      rnode_free(atom);
      return NULL;
    }
    rep->a = atom;
    rep->m = m;
    rep->n = n;
    atom = rep;
  }
  return atom;
}

static rnode *parse_concat(rparse *ps) {
  rnode *head = NULL;
  rnode **slot = &head;
  while (*ps->p != '\0' && *ps->p != '|' && *ps->p != ')') {
    rnode *piece = parse_piece(ps);
    rnode *cat;
    if (!piece) {
      rnode_free(head);
      return NULL;
    }
    cat = node_new(ps, RN_CAT);
    if (!cat) {
      rnode_free(piece);
      rnode_free(head);
      return NULL;
    }
    cat->a = piece;
    *slot = cat;
    slot = &cat->b;
  }
  if (!head) head = node_new(ps, RN_EMPTY);
  return head;
}

static rnode *parse_alt(rparse *ps) {
  rnode *root = NULL;
  rnode **slot = &root;
  rnode *br = parse_concat(ps);
  if (!br) return NULL;
  while (*ps->p == '|') {
    rnode *alt = node_new(ps, RN_ALT);
    if (!alt) {
      rnode_free(br);
      rnode_free(root);
      return NULL;
    }
    alt->a = br;
    *slot = alt;
    slot = &alt->b;
    ps->p++;
    br = parse_concat(ps);
    if (!br) {
      rnode_free(root);
      return NULL;
    }
  }
  *slot = br;
  return root;
}

/* ---- code emission ------------------------------------------------------ */

typedef struct {
  re_inst *prog;
  size_t n;
  const char *err;
} remit;

static int32_t emit1(remit *e, uint8_t op, unsigned char ch, uint16_t cls) {
  re_inst *ip;
  if (e->n >= RE_MAX_INSTS) {
    e->err = "pattern too large";
    return -1;
  }
  ip = &e->prog[e->n];
  ip->op = op;
  ip->ch = ch;
  ip->cls = cls;
  ip->x = 0;
  ip->y = 0;
  return (int32_t)e->n++;
}

/* Recursion depth is bounded by the a-side depth of the tree, which the
 * RE_MAX_NODES cap keeps finite; CAT and ALT chains iterate on b. */
static int emit_node(remit *e, const rnode *nd) {
  while (nd) {
    switch (nd->kind) {
      case RN_EMPTY:
        return 0;
      case RN_CHAR:
        return emit1(e, RI_CHAR, nd->ch, 0) < 0 ? -1 : 0;
      case RN_CLASS:
        return emit1(e, RI_CLASS, 0, nd->cls) < 0 ? -1 : 0;
      case RN_ANY:
        return emit1(e, RI_ANY, 0, 0) < 0 ? -1 : 0;
      case RN_BOL:
        return emit1(e, RI_BOL, 0, 0) < 0 ? -1 : 0;
      case RN_EOL:
        return emit1(e, RI_EOL, 0, 0) < 0 ? -1 : 0;
      case RN_CAT:
        if (nd->a && emit_node(e, nd->a)) return -1;
        nd = nd->b;
        continue;
      case RN_ALT: {
        /* SPLIT into each branch; branch ends JMP to the common end.
         * The pending JMPs are chained through their own x fields. */
        int32_t plist = -1;
        const rnode *cur = nd;
        while (cur && cur->kind == RN_ALT) {
          int32_t s = emit1(e, RI_SPLIT, 0, 0);
          int32_t j;
          if (s < 0) return -1;
          e->prog[s].x = s + 1;
          if (emit_node(e, cur->a)) return -1;
          j = emit1(e, RI_JMP, 0, 0);
          if (j < 0) return -1;
          e->prog[j].x = plist;
          plist = j;
          e->prog[s].y = (int32_t)e->n;
          cur = cur->b;
        }
        if (cur && emit_node(e, cur)) return -1;
        while (plist >= 0) {
          int32_t nx = e->prog[plist].x;
          e->prog[plist].x = (int32_t)e->n;
          plist = nx;
        }
        return 0;
      }
      case RN_REP: {
        int32_t i;
        if (nd->n < 0) {
          if (nd->m == 0) { /* star */
            int32_t l1 = (int32_t)e->n;
            int32_t s = emit1(e, RI_SPLIT, 0, 0);
            int32_t j;
            if (s < 0) return -1;
            e->prog[s].x = s + 1;
            if (emit_node(e, nd->a)) return -1;
            j = emit1(e, RI_JMP, 0, 0);
            if (j < 0) return -1;
            e->prog[j].x = l1;
            e->prog[s].y = (int32_t)e->n;
          } else { /* {m,} = m copies, last one loops */
            int32_t l, s;
            for (i = 0; i < nd->m - 1; i++)
              if (emit_node(e, nd->a)) return -1;
            l = (int32_t)e->n;
            if (emit_node(e, nd->a)) return -1;
            s = emit1(e, RI_SPLIT, 0, 0);
            if (s < 0) return -1;
            e->prog[s].x = l;
            e->prog[s].y = s + 1;
          }
        } else {
          /* {m,n}: m required copies, then n-m optional copies each
           * guarded by a SPLIT to the common end; the pending SPLIT
           * ends are chained through their y fields. */
          int32_t plist = -1;
          for (i = 0; i < nd->m; i++)
            if (emit_node(e, nd->a)) return -1;
          for (i = nd->m; i < nd->n; i++) {
            int32_t s = emit1(e, RI_SPLIT, 0, 0);
            if (s < 0) return -1;
            e->prog[s].x = s + 1;
            e->prog[s].y = plist;
            plist = s;
            if (emit_node(e, nd->a)) return -1;
          }
          while (plist >= 0) {
            int32_t nx = e->prog[plist].y;
            e->prog[plist].y = (int32_t)e->n;
            plist = nx;
          }
        }
        return 0;
      }
      default:
        return 0;
    }
  }
  return 0;
}

/* ---- public API --------------------------------------------------------- */

void astd_regex_free(astd_regex *re) {
  if (!re) return;
  free(re->prog);
  free(re->cls);
  free(re->marka);
  free(re->markb);
  free(re->starta);
  free(re->startb);
  free(re->lista);
  free(re->listb);
  free(re->stack);
  free(re->genp);
  free(re);
}

astd_regex *astd_regex_compile(const char *pattern, int case_sensitive,
                               const char **err) {
  rparse ps;
  remit em;
  rnode *root;
  astd_regex *re;
  const char *edummy;
  int ok;
  if (!err) err = &edummy;
  *err = NULL;
  if (!pattern) {
    *err = "null pattern";
    return NULL;
  }
  memset(&ps, 0, sizeof ps);
  ps.p = pattern;
  ps.fold = !case_sensitive;
  root = parse_alt(&ps);
  if (root && *ps.p != '\0') {
    /* parse_alt stops at a ')' it did not open */
    rnode_free(root);
    root = NULL;
    ps.err = "unmatched )";
  }
  if (!root) {
    free(ps.cls);
    *err = ps.err ? ps.err : "out of memory";
    return NULL;
  }
  em.prog = malloc(sizeof(re_inst) * RE_MAX_INSTS);
  em.n = 0;
  em.err = NULL;
  ok = em.prog != NULL;
  if (ok) ok = emit_node(&em, root) == 0;
  if (ok) ok = emit1(&em, RI_MATCH, 0, 0) >= 0;
  rnode_free(root);
  if (!ok) {
    *err = em.err ? em.err : "out of memory";
    free(em.prog);
    free(ps.cls);
    return NULL;
  }
  re = calloc(1, sizeof *re);
  if (!re) {
    *err = "out of memory";
    free(em.prog);
    free(ps.cls);
    return NULL;
  }
  re->prog = em.prog;
  re->nprog = em.n;
  re->cls = ps.cls;
  re->ncls = ps.ncls;
  re->fold = ps.fold;
  re->marka = calloc(em.n, sizeof(uint32_t));
  re->markb = calloc(em.n, sizeof(uint32_t));
  re->starta = malloc(em.n * sizeof(size_t));
  re->startb = malloc(em.n * sizeof(size_t));
  re->lista = malloc(em.n * sizeof(uint32_t));
  re->listb = malloc(em.n * sizeof(uint32_t));
  re->stack = malloc((2 * em.n + 2) * sizeof(uint32_t));
  re->genp = calloc(1, sizeof(uint32_t));
  if (!re->marka || !re->markb || !re->starta || !re->startb || !re->lista ||
      !re->listb || !re->stack || !re->genp) {
    *err = "out of memory";
    astd_regex_free(re);
    return NULL;
  }
  return re;
}

/* Epsilon-closure insertion of pc into a thread list. First arrival wins:
 * lists are always built in non-decreasing match-start order, so the kept
 * start is the leftmost. Iterative DFS; the stack cannot exceed
 * 2*nprog+2 entries per generation because only a newly marked state
 * pushes (at most two) successors. */
static void re_add(const astd_regex *re, uint32_t *mark, size_t *sarr,
                   uint32_t *list, size_t *ln, uint32_t gen, uint32_t pc0,
                   size_t s, size_t pos, size_t len) {
  uint32_t *stk = re->stack;
  size_t sp = 0;
  stk[sp++] = pc0;
  while (sp > 0) {
    uint32_t pc = stk[--sp];
    const re_inst *ip;
    if (mark[pc] == gen) continue;
    mark[pc] = gen;
    sarr[pc] = s;
    ip = &re->prog[pc];
    switch (ip->op) {
      case RI_JMP:
        stk[sp++] = (uint32_t)ip->x;
        break;
      case RI_SPLIT:
        stk[sp++] = (uint32_t)ip->y;
        stk[sp++] = (uint32_t)ip->x;
        break;
      case RI_BOL:
        if (pos == 0) stk[sp++] = pc + 1;
        break;
      case RI_EOL:
        if (pos == len) stk[sp++] = pc + 1;
        break;
      default: /* consuming state or MATCH */
        list[(*ln)++] = pc;
        break;
    }
  }
}

static int cls_has(const uint8_t map[32], unsigned char c) {
  return (map[c >> 3] >> (c & 7)) & 1;
}

int astd_regex_search(const astd_regex *re, const char *line, size_t len,
                      size_t *start, size_t *end) {
  uint32_t *cmark, *nmark, *clist, *nlist;
  size_t *cstart, *nstart;
  size_t cn = 0, nn, pos, i;
  size_t bs = 0, be = 0;
  int found = 0;
  uint32_t gen;
  if (!re || !start || !end) return 0;
  if (!line && len > 0) return 0;
  /* Generation counter wrap safety: reset marks with ample headroom
   * (a single search consumes at most 2*(len+1) generations). */
  if (*re->genp >= UINT32_C(0xc0000000)) {
    memset(re->marka, 0, re->nprog * sizeof(uint32_t));
    memset(re->markb, 0, re->nprog * sizeof(uint32_t));
    *re->genp = 0;
  }
  cmark = re->marka;
  nmark = re->markb;
  cstart = re->starta;
  nstart = re->startb;
  clist = re->lista;
  nlist = re->listb;
  gen = ++*re->genp;
  re_add(re, cmark, cstart, clist, &cn, gen, 0, 0, 0, len);
  pos = 0;
  for (;;) {
    unsigned char c;
    /* harvest matches at this position */
    for (i = 0; i < cn; i++) {
      uint32_t pc = clist[i];
      if (re->prog[pc].op == RI_MATCH) {
        size_t s = cstart[pc];
        if (!found || s < bs) {
          found = 1;
          bs = s;
          be = pos;
        } else if (s == bs && pos > be) {
          be = pos;
        }
      }
    }
    if (pos == len) break;
    c = (unsigned char)line[pos];
    gen = ++*re->genp;
    nn = 0;
    for (i = 0; i < cn; i++) {
      uint32_t pc = clist[i];
      size_t s = cstart[pc];
      const re_inst *ip = &re->prog[pc];
      int take = 0;
      if (found && s > bs) continue; /* leftmost pruning */
      switch (ip->op) {
        case RI_CHAR:
          take = fold_ch(re->fold, c) == ip->ch;
          break;
        case RI_CLASS:
          take = cls_has(re->cls[ip->cls], c);
          break;
        case RI_ANY:
          take = 1;
          break;
        default:
          break;
      }
      if (take)
        re_add(re, nmark, nstart, nlist, &nn, gen, pc + 1, s, pos + 1, len);
    }
    if (!found)
      re_add(re, nmark, nstart, nlist, &nn, gen, 0, pos + 1, pos + 1, len);
    {
      uint32_t *tm = cmark, *tl = clist;
      size_t *ts = cstart;
      cmark = nmark;
      nmark = tm;
      cstart = nstart;
      nstart = ts;
      clist = nlist;
      nlist = tl;
      cn = nn;
    }
    pos++;
    /* Once a match exists no new starts are injected, so an empty list
     * is final. Before a match the list may be legitimately empty (for
     * example "^x" or "$" mid-buffer) while later injections still have
     * to happen, so keep scanning. */
    if (cn == 0 && found) break;
  }
  if (found) {
    *start = bs;
    *end = be;
    return 1;
  }
  return 0;
}
