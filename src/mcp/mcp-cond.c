//
//  Copyright (C) 2026  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "mcp/mcp-cond.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NODES   64
#define MAX_REFS    16
#define MAX_DEPTH   16

typedef enum {
   N_AND, N_OR, N_NOT, N_CMP, N_CALL, N_TRUTH,
} node_kind_t;

typedef enum {
   OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
} cmp_op_t;

typedef enum {
   F_ROSE, F_FELL, F_CHANGED, F_STABLE,
} func_t;

typedef enum {
   L_CHAR, L_BITS, L_HEX, L_INT, L_NAME,
} lit_kind_t;

typedef struct {
   lit_kind_t kind;
   char      *text;     // Characters of a bit string or name, digits
} literal_t;

typedef struct {
   node_kind_t kind;
   int         lhs, rhs;    // Child nodes of and, or, not
   int         ref;         // Signal read by cmp, call and truth
   cmp_op_t    op;
   func_t      func;
   literal_t   lit;         // Right operand of cmp
} node_t;

struct _cond {
   node_t  nodes[MAX_NODES];
   int     nnodes;
   int     root;
   char   *refs[MAX_REFS];
   int     nrefs;
};

typedef struct {
   cond_t     *c;
   const char *text;
   const char *p;
   char       *err;
   size_t      errlen;
   unsigned    depth;
   bool        failed;
} parser_t;

static void fail(parser_t *ps, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

static void fail(parser_t *ps, const char *fmt, ...)
{
   if (ps->failed)
      return;

   ps->failed = true;

   char what[200];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(what, sizeof(what), fmt, ap);
   va_end(ap);

   snprintf(ps->err, ps->errlen, "Invalid condition at character %d: %s. "
            "Write conditions like /dut/valid = '1' and /dut/state = SEND "
            "or rose(/dut/start): signals are design paths, literals are "
            "'1', \"0101\", x\"1F\", 42 or an enumeration name",
            (int)(ps->p - ps->text) + 1, what);
}

static void skip_space(parser_t *ps)
{
   while (isspace((unsigned char)*ps->p))
      ps->p++;
}

static bool is_word(char c)
{
   return isalnum((unsigned char)c) || c == '_';
}

static bool accept_word(parser_t *ps, const char *word)
{
   skip_space(ps);
   const size_t n = strlen(word);
   if (strncasecmp(ps->p, word, n) == 0 && !is_word(ps->p[n])) {
      ps->p += n;
      return true;
   }

   return false;
}

static int new_node(parser_t *ps, node_kind_t kind)
{
   if (ps->c->nnodes == MAX_NODES) {
      fail(ps, "the condition is too long");
      return 0;
   }

   node_t *n = &(ps->c->nodes[ps->c->nnodes]);
   memset(n, 0, sizeof(*n));
   n->kind = kind;
   return ps->c->nnodes++;
}

static int add_ref(parser_t *ps, const char *path, size_t len)
{
   cond_t *c = ps->c;

   for (int i = 0; i < c->nrefs; i++) {
      if (strlen(c->refs[i]) == len && strncasecmp(c->refs[i], path, len) == 0)
         return i;
   }

   if (c->nrefs == MAX_REFS) {
      fail(ps, "a condition can read at most %d signals", MAX_REFS);
      return 0;
   }

   c->refs[c->nrefs] = strndup(path, len);
   return c->nrefs++;
}

// A signal path: / then names separated by /, a name may end in (N)
static int parse_signal(parser_t *ps)
{
   skip_space(ps);
   const char *start = ps->p;
   if (*ps->p != '/') {
      fail(ps, "expected a signal path starting with /");
      return 0;
   }

   while (*ps->p == '/' || is_word(*ps->p)
          || (*ps->p == '(' && isdigit((unsigned char)ps->p[1]))) {
      if (*ps->p == '(') {
         const char *q = ps->p + 1;
         while (isdigit((unsigned char)*q))
            q++;
         if (*q != ')')
            break;
         ps->p = q;
      }
      ps->p++;
   }

   if (ps->p - start < 2) {
      fail(ps, "expected a signal path such as /dut/valid");
      return 0;
   }

   return add_ref(ps, start, ps->p - start);
}

static bool parse_literal(parser_t *ps, literal_t *lit)
{
   skip_space(ps);
   const char *p = ps->p;

   if (*p == '\'' && p[1] != '\0' && p[2] == '\'') {
      lit->kind = L_CHAR;
      lit->text = strndup(p + 1, 1);
      ps->p += 3;
      return true;
   }
   else if (*p == '"' || ((*p == 'x' || *p == 'X') && p[1] == '"')) {
      const bool hex = *p != '"';
      p += hex ? 2 : 1;
      const char *end = strchr(p, '"');
      if (end == NULL) {
         fail(ps, "unterminated string");
         return false;
      }

      lit->kind = hex ? L_HEX : L_BITS;
      lit->text = strndup(p, end - p);
      for (const char *q = lit->text; *q; q++) {
         const bool good = hex ? isxdigit((unsigned char)*q) || *q == '_'
            : strchr("01UXZWLHuxzwlh-_", *q) != NULL;
         if (!good) {
            fail(ps, hex ? "x\"...\" holds hexadecimal digits"
                 : "\"...\" holds bits such as \"0101\"");
            return false;
         }
      }
      ps->p = end + 1;
      return true;
   }
   else if (isdigit((unsigned char)*p)) {
      const char *q = p;
      while (isdigit((unsigned char)*q) || *q == '_')
         q++;
      lit->kind = L_INT;
      lit->text = strndup(p, q - p);
      ps->p = q;
      return true;
   }
   else if (isalpha((unsigned char)*p)) {
      const char *q = p;
      while (is_word(*q))
         q++;
      lit->kind = L_NAME;
      lit->text = strndup(p, q - p);
      ps->p = q;
      return true;
   }

   fail(ps, "expected a value such as '1', \"0101\", x\"1F\", 42 or a name");
   return false;
}

static int parse_expr(parser_t *ps);

static int parse_primary(parser_t *ps)
{
   skip_space(ps);

   if (*ps->p == '(') {
      if (++ps->depth > MAX_DEPTH) {
         fail(ps, "too many nested parentheses");
         return 0;
      }
      ps->p++;
      const int e = parse_expr(ps);
      skip_space(ps);
      if (*ps->p != ')')
         fail(ps, "expected )");
      else
         ps->p++;
      ps->depth--;
      return e;
   }

   static const struct { const char *name; func_t f; } funcs[] = {
      { "rose", F_ROSE }, { "fell", F_FELL }, { "changed", F_CHANGED },
      { "stable", F_STABLE },
   };

   for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
      const size_t n = strlen(funcs[i].name);
      if (strncasecmp(ps->p, funcs[i].name, n) == 0 && !is_word(ps->p[n])) {
         const char *save = ps->p;
         ps->p += n;
         skip_space(ps);
         if (*ps->p != '(') {
            ps->p = save;
            break;
         }
         ps->p++;
         const int ref = parse_signal(ps);
         skip_space(ps);
         if (*ps->p != ')')
            fail(ps, "expected ) after the signal");
         else
            ps->p++;

         const int n_id = new_node(ps, N_CALL);
         ps->c->nodes[n_id].ref = ref;
         ps->c->nodes[n_id].func = funcs[i].f;
         return n_id;
      }
   }

   const int ref = parse_signal(ps);
   skip_space(ps);

   static const struct { const char *text; cmp_op_t op; } ops[] = {
      { "/=", OP_NE }, { "<=", OP_LE }, { ">=", OP_GE }, { "=", OP_EQ },
      { "<", OP_LT }, { ">", OP_GT },
   };

   for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
      const size_t n = strlen(ops[i].text);
      if (strncmp(ps->p, ops[i].text, n) == 0) {
         ps->p += n;
         literal_t lit = {};
         parse_literal(ps, &lit);

         const int id = new_node(ps, N_CMP);
         node_t *node = &(ps->c->nodes[id]);
         node->ref = ref;
         node->op = ops[i].op;
         node->lit = lit;
         return id;
      }
   }

   const int id = new_node(ps, N_TRUTH);
   ps->c->nodes[id].ref = ref;
   return id;
}

static int parse_not(parser_t *ps)
{
   if (accept_word(ps, "not")) {
      if (++ps->depth > MAX_DEPTH) {
         fail(ps, "too many nested not");
         return 0;
      }
      const int e = parse_not(ps);
      ps->depth--;
      const int id = new_node(ps, N_NOT);
      ps->c->nodes[id].lhs = e;
      return id;
   }

   return parse_primary(ps);
}

static int parse_and(parser_t *ps)
{
   int lhs = parse_not(ps);
   while (!ps->failed && accept_word(ps, "and")) {
      const int rhs = parse_not(ps);
      const int id = new_node(ps, N_AND);
      ps->c->nodes[id].lhs = lhs;
      ps->c->nodes[id].rhs = rhs;
      lhs = id;
   }

   return lhs;
}

static int parse_expr(parser_t *ps)
{
   int lhs = parse_and(ps);
   while (!ps->failed && accept_word(ps, "or")) {
      const int rhs = parse_and(ps);
      const int id = new_node(ps, N_OR);
      ps->c->nodes[id].lhs = lhs;
      ps->c->nodes[id].rhs = rhs;
      lhs = id;
   }

   return lhs;
}

cond_t *cond_parse(const char *text, char *err, size_t errlen)
{
   cond_t *c = calloc(1, sizeof(cond_t));
   parser_t ps = { .c = c, .text = text, .p = text, .err = err,
                   .errlen = errlen };

   c->root = parse_expr(&ps);

   skip_space(&ps);
   if (!ps.failed && *ps.p != '\0')
      fail(&ps, "unexpected '%.20s': use and, or, not and parentheses to "
           "combine tests", ps.p);

   if (ps.failed) {
      cond_free(c);
      return NULL;
   }

   return c;
}

void cond_free(cond_t *c)
{
   if (c == NULL)
      return;

   for (int i = 0; i < c->nnodes; i++)
      free(c->nodes[i].lit.text);

   for (int i = 0; i < c->nrefs; i++)
      free(c->refs[i]);

   free(c);
}

size_t cond_nrefs(const cond_t *c)
{
   return c->nrefs;
}

const char *cond_ref(const cond_t *c, size_t index)
{
   return c->refs[index];
}

////////////////////////////////////////////////////////////////////////////////
// Evaluation

// Unsigned value of a string of 0 and 1, false if it holds anything else
static bool bits_value(const char *s, uint64_t *out)
{
   const size_t len = strlen(s);
   if (len == 0 || len > 64)
      return false;

   uint64_t v = 0;
   for (const char *p = s; *p; p++) {
      if (*p != '0' && *p != '1')
         return false;
      v = v << 1 | (*p == '1');
   }

   *out = v;
   return true;
}

static bool text_number(const char *s, uint64_t *out)
{
   char *end;
   const long long v = strtoll(s, &end, 10);
   if (end == s || *end != '\0' || v < 0)
      return false;

   *out = v;
   return true;
}

// The number a literal stands for
static bool literal_value(const literal_t *lit, uint64_t *out)
{
   uint64_t v = 0;

   switch (lit->kind) {
   case L_INT:
      {
         char clean[64];
         size_t n = 0;
         for (const char *p = lit->text; *p && n < sizeof(clean) - 1; p++) {
            if (*p != '_')
               clean[n++] = *p;
         }
         clean[n] = '\0';
         return text_number(clean, out);
      }
   case L_HEX:
      {
         size_t digits = 0;
         for (const char *p = lit->text; *p; p++) {
            if (*p == '_')
               continue;
            v = v << 4 | (isdigit((unsigned char)*p) ? *p - '0'
                          : (tolower((unsigned char)*p) - 'a' + 10));
            if (++digits > 16)
               return false;
         }
         *out = v;
         return digits > 0;
      }
   case L_BITS:
      return bits_value(lit->text, out);
   case L_CHAR:
      if (lit->text[0] == '0' || lit->text[0] == '1') {
         *out = lit->text[0] - '0';
         return true;
      }
      return false;
   default:
      return false;
   }
}

// Bits of a hexadecimal literal, for exact comparison with a vector
static char *hex_bits(const char *hex)
{
   char *out = malloc(strlen(hex) * 4 + 1);
   size_t n = 0;
   for (const char *p = hex; *p; p++) {
      if (*p == '_')
         continue;
      const int d = isdigit((unsigned char)*p) ? *p - '0'
         : tolower((unsigned char)*p) - 'a' + 10;
      for (int b = 3; b >= 0; b--)
         out[n++] = (d >> b) & 1 ? '1' : '0';
   }
   out[n] = '\0';
   return out;
}

// Equal after dropping leading zeros, so x"0F" matches the 4 bit 1111
static bool bits_equal(const char *a, const char *b)
{
   while (*a == '0' && a[1] != '\0')
      a++;
   while (*b == '0' && b[1] != '\0')
      b++;
   return strcasecmp(a, b) == 0;
}

static bool compare_values(const node_t *n, const cond_val_t *v)
{
   if (v->cur == NULL)
      return false;

   const literal_t *lit = &n->lit;
   bool eq = false;
   int order = 0;
   bool ordered = false;

   if (v->bits) {
      uint64_t sv, lv;
      const bool numeric = bits_value(v->cur, &sv)
         && literal_value(lit, &lv);

      switch (lit->kind) {
      case L_CHAR:
      case L_BITS:
         // Compared as written when the signal is not a plain number
         eq = numeric ? sv == lv : strcasecmp(v->cur, lit->text) == 0;
         break;
      case L_HEX:
         if (numeric)
            eq = sv == lv;
         else {
            char *bits = hex_bits(lit->text);
            eq = strcasecmp(v->cur, bits) == 0;
            free(bits);
         }
         break;
      case L_INT:
         eq = numeric && sv == lv;
         break;
      case L_NAME:
         eq = strcasecmp(v->cur, lit->text) == 0;
         break;
      }

      if (numeric) {
         ordered = true;
         order = sv < lv ? -1 : sv > lv;
      }

      if (lit->kind == L_BITS && !numeric)
         eq = bits_equal(v->cur, lit->text);
   }
   else {
      eq = strcasecmp(v->cur, lit->text) == 0;

      uint64_t sv, lv;
      if (text_number(v->cur, &sv) && literal_value(lit, &lv)) {
         eq = sv == lv;
         ordered = true;
         order = sv < lv ? -1 : sv > lv;
      }
   }

   switch (n->op) {
   case OP_EQ: return eq;
   case OP_NE: return !eq;
   case OP_LT: return ordered && order < 0;
   case OP_LE: return ordered && order <= 0;
   case OP_GT: return ordered && order > 0;
   case OP_GE: return ordered && order >= 0;
   }

   return false;
}

static bool is_one(const char *s)
{
   return s != NULL && (s[0] == '1' || s[0] == 'H' || s[0] == 'h')
      && s[1] == '\0';
}

static bool is_zero(const char *s)
{
   return s != NULL && (s[0] == '0' || s[0] == 'L' || s[0] == 'l')
      && s[1] == '\0';
}

static bool eval_node(const cond_t *c, int id, const cond_val_t *vals)
{
   const node_t *n = &(c->nodes[id]);

   switch (n->kind) {
   case N_AND:
      return eval_node(c, n->lhs, vals) && eval_node(c, n->rhs, vals);
   case N_OR:
      return eval_node(c, n->lhs, vals) || eval_node(c, n->rhs, vals);
   case N_NOT:
      return !eval_node(c, n->lhs, vals);
   case N_CMP:
      return compare_values(n, &(vals[n->ref]));
   case N_TRUTH:
      return is_one(vals[n->ref].cur);
   case N_CALL:
      {
         const cond_val_t *v = &(vals[n->ref]);
         if (v->cur == NULL || v->prev == NULL)
            return false;

         const bool same = strcasecmp(v->cur, v->prev) == 0;
         switch (n->func) {
         case F_ROSE:    return is_zero(v->prev) && is_one(v->cur);
         case F_FELL:    return is_one(v->prev) && is_zero(v->cur);
         case F_CHANGED: return !same;
         case F_STABLE:  return same;
         }
      }
   }

   return false;
}

bool cond_eval(const cond_t *c, const cond_val_t *vals)
{
   return eval_node(c, c->root, vals);
}
