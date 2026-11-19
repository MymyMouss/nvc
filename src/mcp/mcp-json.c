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

#include "util.h"
#include "mcp/mcp-json.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
// UTF-8

// Returns the length of the sequence at p or zero if it is not valid
// UTF-8 (overlong forms, surrogates and values above U+10FFFF are
// rejected).
static int utf8_decode(const unsigned char *p, const unsigned char *end,
                       uint32_t *cp)
{
   assert(p < end);

   const unsigned char b0 = p[0];
   if (b0 < 0x80) {
      *cp = b0;
      return 1;
   }

   int len;
   uint32_t value, min;
   if ((b0 & 0xe0) == 0xc0) {
      len = 2;
      value = b0 & 0x1f;
      min = 0x80;
   }
   else if ((b0 & 0xf0) == 0xe0) {
      len = 3;
      value = b0 & 0x0f;
      min = 0x800;
   }
   else if ((b0 & 0xf8) == 0xf0) {
      len = 4;
      value = b0 & 0x07;
      min = 0x10000;
   }
   else
      return 0;

   if (end - p < len)
      return 0;

   for (int i = 1; i < len; i++) {
      if ((p[i] & 0xc0) != 0x80)
         return 0;
      value = (value << 6) | (p[i] & 0x3f);
   }

   if (value < min || value > 0x10ffff || (value >= 0xd800 && value < 0xe000))
      return 0;

   *cp = value;
   return len;
}

static int utf8_encode(uint32_t cp, char *out)
{
   if (cp < 0x80) {
      out[0] = cp;
      return 1;
   }
   else if (cp < 0x800) {
      out[0] = 0xc0 | (cp >> 6);
      out[1] = 0x80 | (cp & 0x3f);
      return 2;
   }
   else if (cp < 0x10000) {
      out[0] = 0xe0 | (cp >> 12);
      out[1] = 0x80 | ((cp >> 6) & 0x3f);
      out[2] = 0x80 | (cp & 0x3f);
      return 3;
   }
   else {
      out[0] = 0xf0 | (cp >> 18);
      out[1] = 0x80 | ((cp >> 12) & 0x3f);
      out[2] = 0x80 | ((cp >> 6) & 0x3f);
      out[3] = 0x80 | (cp & 0x3f);
      return 4;
   }
}

////////////////////////////////////////////////////////////////////////////////
// Parser

typedef struct {
   mem_pool_t    *mp;
   const char    *p;
   const char    *end;
   unsigned       depth;
   unsigned       max_depth;
   const char    *err;
} parser_t;

static json_t *parse_value(parser_t *ps);

static json_t *parse_fail(parser_t *ps, const char *msg)
{
   if (ps->err == NULL)
      ps->err = msg;
   return NULL;
}

static void skip_space(parser_t *ps)
{
   while (ps->p < ps->end
          && (*ps->p == ' ' || *ps->p == '\t'
              || *ps->p == '\n' || *ps->p == '\r'))
      ps->p++;
}

static json_t *new_node(parser_t *ps, json_kind_t kind)
{
   json_t *j = pool_calloc(ps->mp, sizeof(json_t));
   j->kind = kind;
   return j;
}

static bool parse_hex4(parser_t *ps, uint32_t *value)
{
   if (ps->end - ps->p < 4)
      return false;

   uint32_t v = 0;
   for (int i = 0; i < 4; i++) {
      const char c = ps->p[i];
      v <<= 4;
      if (c >= '0' && c <= '9')
         v |= c - '0';
      else if (c >= 'a' && c <= 'f')
         v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
         v |= c - 'A' + 10;
      else
         return false;
   }

   ps->p += 4;
   *value = v;
   return true;
}

// Parses a string whose opening quote has been consumed and returns a
// pool allocated NUL terminated copy.
static char *parse_string_body(parser_t *ps, size_t *outlen)
{
   // Find the closing quote first so the decoded string (which is never
   // longer than its encoded form) can be allocated exactly once
   const char *q = ps->p;
   while (q < ps->end && *q != '"') {
      if (*q == '\\') {
         q++;
         if (q == ps->end)
            break;
      }
      q++;
   }

   if (q >= ps->end) {
      parse_fail(ps, "unterminated string");
      return NULL;
   }

   char *buf = pool_malloc(ps->mp, (q - ps->p) + 1), *wp = buf;

   while (ps->p < q) {
      const unsigned char c = *ps->p;

      if (c < 0x20) {
         parse_fail(ps, "control character in string");
         return NULL;
      }
      else if (c == '\\') {
         ps->p++;
         switch (*ps->p++) {
         case '"':  *wp++ = '"'; break;
         case '\\': *wp++ = '\\'; break;
         case '/':  *wp++ = '/'; break;
         case 'b':  *wp++ = '\b'; break;
         case 'f':  *wp++ = '\f'; break;
         case 'n':  *wp++ = '\n'; break;
         case 'r':  *wp++ = '\r'; break;
         case 't':  *wp++ = '\t'; break;
         case 'u':
            {
               uint32_t cp;
               if (!parse_hex4(ps, &cp)) {
                  parse_fail(ps, "invalid \\u escape");
                  return NULL;
               }

               if (cp >= 0xd800 && cp < 0xdc00) {
                  uint32_t lo;
                  if (ps->end - ps->p < 2 || ps->p[0] != '\\'
                      || ps->p[1] != 'u') {
                     parse_fail(ps, "unpaired surrogate in string");
                     return NULL;
                  }
                  ps->p += 2;
                  if (!parse_hex4(ps, &lo) || lo < 0xdc00 || lo >= 0xe000) {
                     parse_fail(ps, "unpaired surrogate in string");
                     return NULL;
                  }
                  cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
               }
               else if (cp >= 0xdc00 && cp < 0xe000) {
                  parse_fail(ps, "unpaired surrogate in string");
                  return NULL;
               }

               if (cp == 0) {
                  parse_fail(ps, "NUL character in string");
                  return NULL;
               }

               wp += utf8_encode(cp, wp);
            }
            break;
         default:
            parse_fail(ps, "invalid escape in string");
            return NULL;
         }
      }
      else if (c < 0x80)
         *wp++ = *ps->p++;
      else {
         uint32_t cp;
         const int len = utf8_decode((const unsigned char *)ps->p,
                                     (const unsigned char *)q, &cp);
         if (len == 0) {
            parse_fail(ps, "invalid UTF-8 in string");
            return NULL;
         }
         memcpy(wp, ps->p, len);
         wp += len;
         ps->p += len;
      }
   }

   assert(ps->p == q);
   ps->p++;   // Closing quote

   *wp = '\0';
   *outlen = wp - buf;
   return buf;
}

static json_t *parse_string(parser_t *ps)
{
   ps->p++;   // Opening quote

   size_t len;
   char *chars = parse_string_body(ps, &len);
   if (chars == NULL)
      return NULL;

   json_t *j = new_node(ps, JSON_STRING);
   j->string.chars = chars;
   j->string.len = len;
   return j;
}

static json_t *parse_number(parser_t *ps)
{
   const char *start = ps->p;
   const char *p = start;
   bool is_int = true;

   if (p < ps->end && *p == '-')
      p++;

   if (p >= ps->end)
      return parse_fail(ps, "invalid number");
   else if (*p == '0')
      p++;
   else if (*p >= '1' && *p <= '9') {
      while (p < ps->end && *p >= '0' && *p <= '9')
         p++;
   }
   else
      return parse_fail(ps, "invalid number");

   if (p < ps->end && *p == '.') {
      is_int = false;
      p++;
      if (p >= ps->end || *p < '0' || *p > '9')
         return parse_fail(ps, "invalid number");
      while (p < ps->end && *p >= '0' && *p <= '9')
         p++;
   }

   if (p < ps->end && (*p == 'e' || *p == 'E')) {
      is_int = false;
      p++;
      if (p < ps->end && (*p == '+' || *p == '-'))
         p++;
      if (p >= ps->end || *p < '0' || *p > '9')
         return parse_fail(ps, "invalid number");
      while (p < ps->end && *p >= '0' && *p <= '9')
         p++;
   }

   const size_t len = p - start;
   if (len >= 64)
      return parse_fail(ps, "number too long");

   char buf[64];
   memcpy(buf, start, len);
   buf[len] = '\0';

   ps->p = p;

   if (is_int) {
      errno = 0;
      char *endp;
      const long long v = strtoll(buf, &endp, 10);
      if (errno == 0 && *endp == '\0') {
         json_t *j = new_node(ps, JSON_INT);
         j->integer = v;
         return j;
      }
   }

   errno = 0;
   const double d = strtod(buf, NULL);
   if (!isfinite(d))
      return parse_fail(ps, "number out of range");

   json_t *j = new_node(ps, JSON_REAL);
   j->real = d;
   return j;
}

static bool parse_literal(parser_t *ps, const char *word)
{
   const size_t len = strlen(word);
   if ((size_t)(ps->end - ps->p) < len || memcmp(ps->p, word, len) != 0)
      return false;

   ps->p += len;
   return true;
}

static json_t *parse_array(parser_t *ps)
{
   ps->p++;   // '['

   json_t **items = NULL;
   size_t count = 0, alloc = 0;

   skip_space(ps);

   if (ps->p < ps->end && *ps->p == ']')
      ps->p++;
   else {
      for (;;) {
         json_t *elem = parse_value(ps);
         if (elem == NULL) {
            free(items);
            return NULL;
         }

         if (count == alloc) {
            alloc = MAX(alloc * 2, 8);
            items = xrealloc_array(items, alloc, sizeof(json_t *));
         }
         items[count++] = elem;

         skip_space(ps);

         if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
         }
         else if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
         }

         free(items);
         return parse_fail(ps, "expected ',' or ']' in array");
      }
   }

   json_t *j = new_node(ps, JSON_ARRAY);
   j->array.count = count;
   if (count > 0) {
      j->array.items = pool_malloc_array(ps->mp, count, sizeof(json_t *));
      memcpy(j->array.items, items, count * sizeof(json_t *));
   }

   free(items);
   return j;
}

static json_t *parse_object(parser_t *ps)
{
   ps->p++;   // '{'

   const char **keys = NULL;
   json_t **values = NULL;
   size_t count = 0, alloc = 0;

   skip_space(ps);

   if (ps->p < ps->end && *ps->p == '}')
      ps->p++;
   else {
      for (;;) {
         skip_space(ps);

         if (ps->p >= ps->end || *ps->p != '"') {
            free(keys);
            free(values);
            return parse_fail(ps, "expected string key in object");
         }

         ps->p++;

         size_t klen;
         char *key = parse_string_body(ps, &klen);
         if (key == NULL) {
            free(keys);
            free(values);
            return NULL;
         }

         for (size_t i = 0; i < count; i++) {
            if (strcmp(keys[i], key) == 0) {
               free(keys);
               free(values);
               return parse_fail(ps, "duplicate key in object");
            }
         }

         skip_space(ps);

         if (ps->p >= ps->end || *ps->p != ':') {
            free(keys);
            free(values);
            return parse_fail(ps, "expected ':' after object key");
         }

         ps->p++;

         json_t *value = parse_value(ps);
         if (value == NULL) {
            free(keys);
            free(values);
            return NULL;
         }

         if (count == alloc) {
            alloc = MAX(alloc * 2, 8);
            keys = xrealloc_array(keys, alloc, sizeof(char *));
            values = xrealloc_array(values, alloc, sizeof(json_t *));
         }
         keys[count] = key;
         values[count] = value;
         count++;

         skip_space(ps);

         if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
         }
         else if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
         }

         free(keys);
         free(values);
         return parse_fail(ps, "expected ',' or '}' in object");
      }
   }

   json_t *j = new_node(ps, JSON_OBJECT);
   j->object.count = count;
   if (count > 0) {
      j->object.keys = pool_malloc_array(ps->mp, count, sizeof(char *));
      j->object.values = pool_malloc_array(ps->mp, count, sizeof(json_t *));
      memcpy(j->object.keys, keys, count * sizeof(char *));
      memcpy(j->object.values, values, count * sizeof(json_t *));
   }

   free(keys);
   free(values);
   return j;
}

static json_t *parse_value(parser_t *ps)
{
   skip_space(ps);

   if (ps->p >= ps->end)
      return parse_fail(ps, "unexpected end of input");

   json_t *result = NULL;

   switch (*ps->p) {
   case '{':
   case '[':
      // Only containers count towards the nesting limit
      if (ps->depth >= ps->max_depth)
         return parse_fail(ps, "maximum nesting depth exceeded");

      ps->depth++;
      result = *ps->p == '{' ? parse_object(ps) : parse_array(ps);
      ps->depth--;
      break;
   case '"':
      result = parse_string(ps);
      break;
   case 't':
      if (parse_literal(ps, "true")) {
         result = new_node(ps, JSON_BOOL);
         result->boolean = true;
      }
      else
         parse_fail(ps, "invalid literal");
      break;
   case 'f':
      if (parse_literal(ps, "false")) {
         result = new_node(ps, JSON_BOOL);
         result->boolean = false;
      }
      else
         parse_fail(ps, "invalid literal");
      break;
   case 'n':
      if (parse_literal(ps, "null"))
         result = new_node(ps, JSON_NULL);
      else
         parse_fail(ps, "invalid literal");
      break;
   default:
      result = parse_number(ps);
      break;
   }

   return result;
}

json_t *json_parse(mem_pool_t *mp, const char *text, size_t len,
                   const json_limits_t *limits, const char **err)
{
   const size_t max_bytes =
      limits != NULL ? limits->max_bytes : JSON_DEFAULT_MAX_BYTES;
   const unsigned max_depth =
      limits != NULL ? limits->max_depth : JSON_DEFAULT_MAX_DEPTH;

   if (len > max_bytes) {
      *err = "input too large";
      return NULL;
   }

   parser_t ps = {
      .mp        = mp,
      .p         = text,
      .end       = text + len,
      .max_depth = max_depth,
   };

   json_t *j = parse_value(&ps);
   if (j != NULL) {
      skip_space(&ps);
      if (ps.p != ps.end) {
         *err = "unexpected data after JSON value";
         return NULL;
      }
   }

   if (j == NULL)
      *err = ps.err ?: "invalid JSON";

   return j;
}

////////////////////////////////////////////////////////////////////////////////
// Accessors

bool json_is(const json_t *j, json_kind_t kind)
{
   return j != NULL && j->kind == kind;
}

json_t *json_get(const json_t *obj, const char *key)
{
   if (!json_is(obj, JSON_OBJECT))
      return NULL;

   for (size_t i = 0; i < obj->object.count; i++) {
      if (strcmp(obj->object.keys[i], key) == 0)
         return obj->object.values[i];
   }

   return NULL;
}

json_t *json_at(const json_t *arr, size_t index)
{
   if (!json_is(arr, JSON_ARRAY) || index >= arr->array.count)
      return NULL;

   return arr->array.items[index];
}

const char *json_string(const json_t *j)
{
   return json_is(j, JSON_STRING) ? j->string.chars : NULL;
}

bool json_as_int(const json_t *j, int64_t *value)
{
   if (json_is(j, JSON_INT)) {
      *value = j->integer;
      return true;
   }
   else if (json_is(j, JSON_REAL) && j->real == floor(j->real)
            && fabs(j->real) < 9007199254740992.0) {
      *value = (int64_t)j->real;
      return true;
   }
   else
      return false;
}

bool json_as_bool(const json_t *j, bool *value)
{
   if (!json_is(j, JSON_BOOL))
      return false;

   *value = j->boolean;
   return true;
}

////////////////////////////////////////////////////////////////////////////////
// Writer

void jw_init(json_writer_t *w, text_buf_t *tb)
{
   memset(w, 0, sizeof(*w));
   w->tb = tb;
   w->first[0] = true;
}

static void jw_prefix(json_writer_t *w)
{
   if (w->after_key) {
      w->after_key = false;
      return;
   }

   if (!w->first[w->depth])
      tb_append(w->tb, ',');

   w->first[w->depth] = false;
}

static void jw_open(json_writer_t *w, char ch)
{
   jw_prefix(w);
   tb_append(w->tb, ch);

   assert(w->depth < JSON_WRITER_MAX_DEPTH);
   w->depth++;
   w->first[w->depth] = true;
}

static void jw_close(json_writer_t *w, char ch)
{
   assert(w->depth > 0);
   w->depth--;
   tb_append(w->tb, ch);
}

void jw_begin_object(json_writer_t *w)
{
   jw_open(w, '{');
}

void jw_end_object(json_writer_t *w)
{
   jw_close(w, '}');
}

void jw_begin_array(json_writer_t *w)
{
   jw_open(w, '[');
}

void jw_end_array(json_writer_t *w)
{
   jw_close(w, ']');
}

static void jw_quoted(json_writer_t *w, const char *str, size_t len)
{
   static const char hex[] = "0123456789abcdef";

   tb_append(w->tb, '"');

   const unsigned char *p = (const unsigned char *)str;
   const unsigned char *end = p + len;

   while (p < end) {
      const unsigned char c = *p;

      if (c == '"')
         tb_cat(w->tb, "\\\""), p++;
      else if (c == '\\')
         tb_cat(w->tb, "\\\\"), p++;
      else if (c == '\n')
         tb_cat(w->tb, "\\n"), p++;
      else if (c == '\r')
         tb_cat(w->tb, "\\r"), p++;
      else if (c == '\t')
         tb_cat(w->tb, "\\t"), p++;
      else if (c < 0x20 || c == 0x7f) {
         tb_cat(w->tb, "\\u00");
         tb_append(w->tb, hex[c >> 4]);
         tb_append(w->tb, hex[c & 0xf]);
         p++;
      }
      else if (c < 0x80)
         tb_append(w->tb, c), p++;
      else {
         uint32_t cp;
         const int n = utf8_decode(p, end, &cp);
         if (n == 0) {
            tb_cat(w->tb, "\xef\xbf\xbd");   // U+FFFD
            p++;
         }
         else {
            tb_catn(w->tb, (const char *)p, n);
            p += n;
         }
      }
   }

   tb_append(w->tb, '"');
}

void jw_key(json_writer_t *w, const char *key)
{
   assert(!w->after_key);

   if (!w->first[w->depth])
      tb_append(w->tb, ',');

   w->first[w->depth] = false;

   jw_quoted(w, key, strlen(key));
   tb_append(w->tb, ':');

   w->after_key = true;
}

void jw_stringn(json_writer_t *w, const char *str, size_t len)
{
   jw_prefix(w);
   jw_quoted(w, str, len);
}

void jw_string(json_writer_t *w, const char *str)
{
   jw_stringn(w, str, strlen(str));
}

void jw_int(json_writer_t *w, int64_t value)
{
   jw_prefix(w);
   tb_printf(w->tb, "%"PRIi64, value);
}

void jw_real(json_writer_t *w, double value)
{
   jw_prefix(w);

   if (isfinite(value))
      tb_printf(w->tb, "%.17g", value);
   else
      tb_cat(w->tb, "null");
}

void jw_bool(json_writer_t *w, bool value)
{
   jw_prefix(w);
   tb_cat(w->tb, value ? "true" : "false");
}

void jw_null(json_writer_t *w)
{
   jw_prefix(w);
   tb_cat(w->tb, "null");
}

void jw_raw(json_writer_t *w, const char *json)
{
   jw_prefix(w);
   tb_cat(w->tb, json);
}

void jw_raw_members(json_writer_t *w, const char *members)
{
   assert(!w->after_key);

   if (*members == '\0')
      return;

   if (!w->first[w->depth])
      tb_append(w->tb, ',');

   w->first[w->depth] = false;

   tb_cat(w->tb, members);
}
