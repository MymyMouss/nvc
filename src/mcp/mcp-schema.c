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
#include "mcp/mcp-schema.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#define MAX_SCHEMA_DEPTH 16

typedef struct {
   text_buf_t *err;
   text_buf_t *path;
} check_t;

static bool check_value(check_t *c, const json_t *schema, const json_t *value,
                        unsigned depth);

static const char *kind_name(const json_t *j)
{
   switch (j->kind) {
   case JSON_NULL:   return "null";
   case JSON_BOOL:   return "boolean";
   case JSON_INT:    return "integer";
   case JSON_REAL:   return "number";
   case JSON_STRING: return "string";
   case JSON_ARRAY:  return "array";
   case JSON_OBJECT: return "object";
   }

   return "value";
}

static bool fail(check_t *c, const char *fmt, ...)
   __attribute__((format(printf, 2, 3)));

static bool fail(check_t *c, const char *fmt, ...)
{
   // Only the first failure is reported
   if (tb_len(c->err) == 0) {
      tb_cat(c->err, tb_get(c->path));
      tb_cat(c->err, ": ");

      va_list ap;
      va_start(ap, fmt);
      tb_vprintf(c->err, fmt, ap);
      va_end(ap);
   }

   return false;
}

static bool type_matches(const char *type, const json_t *value)
{
   if (strcmp(type, "string") == 0)
      return value->kind == JSON_STRING;
   else if (strcmp(type, "boolean") == 0)
      return value->kind == JSON_BOOL;
   else if (strcmp(type, "null") == 0)
      return value->kind == JSON_NULL;
   else if (strcmp(type, "array") == 0)
      return value->kind == JSON_ARRAY;
   else if (strcmp(type, "object") == 0)
      return value->kind == JSON_OBJECT;
   else if (strcmp(type, "number") == 0)
      return value->kind == JSON_INT || value->kind == JSON_REAL;
   else if (strcmp(type, "integer") == 0) {
      int64_t tmp;
      return json_as_int(value, &tmp);
   }

   return false;
}

static bool equal_scalar(const json_t *a, const json_t *b)
{
   if (a->kind != b->kind)
      return false;

   switch (a->kind) {
   case JSON_NULL:   return true;
   case JSON_BOOL:   return a->boolean == b->boolean;
   case JSON_INT:    return a->integer == b->integer;
   case JSON_REAL:   return a->real == b->real;
   case JSON_STRING: return strcmp(a->string.chars, b->string.chars) == 0;
   default:          return false;
   }
}

static size_t count_chars(const char *str)
{
   size_t n = 0;
   for (; *str; str++) {
      if ((*str & 0xc0) != 0x80)
         n++;
   }
   return n;
}

static bool check_type(check_t *c, const json_t *type, const json_t *value)
{
   if (json_is(type, JSON_STRING)) {
      if (type_matches(type->string.chars, value))
         return true;

      return fail(c, "expected %s but got %s", type->string.chars,
                  kind_name(value));
   }
   else if (json_is(type, JSON_ARRAY)) {
      for (size_t i = 0; i < type->array.count; i++) {
         const char *name = json_string(type->array.items[i]);
         if (name != NULL && type_matches(name, value))
            return true;
      }

      return fail(c, "unexpected type %s", kind_name(value));
   }

   return true;
}

static bool check_enum(check_t *c, const json_t *allowed, const json_t *value)
{
   if (!json_is(allowed, JSON_ARRAY))
      return true;

   for (size_t i = 0; i < allowed->array.count; i++) {
      if (equal_scalar(allowed->array.items[i], value))
         return true;
   }

   LOCAL_TEXT_BUF list = tb_new();
   for (size_t i = 0; i < allowed->array.count; i++) {
      const json_t *a = allowed->array.items[i];
      if (i > 0)
         tb_cat(list, ", ");
      if (a->kind == JSON_STRING)
         tb_printf(list, "\"%s\"", a->string.chars);
      else if (a->kind == JSON_INT)
         tb_printf(list, "%"PRIi64, a->integer);
      else if (a->kind == JSON_BOOL)
         tb_cat(list, a->boolean ? "true" : "false");
   }

   return fail(c, "must be one of %s", tb_get(list));
}

static bool check_number(check_t *c, const json_t *schema, const json_t *value)
{
   const double v = value->kind == JSON_INT ? (double)value->integer
      : value->real;

   const json_t *min = json_get(schema, "minimum");
   if (json_is(min, JSON_INT) || json_is(min, JSON_REAL)) {
      const double m = min->kind == JSON_INT ? (double)min->integer : min->real;
      if (v < m)
         return fail(c, "must be at least %g (got %g)", m, v);
   }

   const json_t *max = json_get(schema, "maximum");
   if (json_is(max, JSON_INT) || json_is(max, JSON_REAL)) {
      const double m = max->kind == JSON_INT ? (double)max->integer : max->real;
      if (v > m)
         return fail(c, "must be at most %g (got %g)", m, v);
   }

   return true;
}

static bool check_string(check_t *c, const json_t *schema, const json_t *value)
{
   int64_t limit;
   const size_t nchars = count_chars(value->string.chars);

   if (json_as_int(json_get(schema, "minLength"), &limit)
       && (int64_t)nchars < limit)
      return fail(c, "must have at least %"PRIi64" characters", limit);

   if (json_as_int(json_get(schema, "maxLength"), &limit)
       && (int64_t)nchars > limit)
      return fail(c, "must have at most %"PRIi64" characters (got %zu)",
                  limit, nchars);

   return true;
}

static bool check_array(check_t *c, const json_t *schema, const json_t *value,
                        unsigned depth)
{
   int64_t limit;

   if (json_as_int(json_get(schema, "minItems"), &limit)
       && (int64_t)value->array.count < limit)
      return fail(c, "must have at least %"PRIi64" items", limit);

   if (json_as_int(json_get(schema, "maxItems"), &limit)
       && (int64_t)value->array.count > limit)
      return fail(c, "must have at most %"PRIi64" items (got %zu)", limit,
                  value->array.count);

   const json_t *items = json_get(schema, "items");
   if (json_is(items, JSON_OBJECT)) {
      for (size_t i = 0; i < value->array.count; i++) {
         const size_t mark = tb_len(c->path);
         tb_printf(c->path, "[%zu]", i);

         const bool ok = check_value(c, items, value->array.items[i],
                                     depth + 1);

         tb_trim(c->path, mark);

         if (!ok)
            return false;
      }
   }

   return true;
}

static bool check_object(check_t *c, const json_t *schema,
                         const json_t *value, unsigned depth)
{
   const json_t *required = json_get(schema, "required");
   if (json_is(required, JSON_ARRAY)) {
      for (size_t i = 0; i < required->array.count; i++) {
         const char *key = json_string(required->array.items[i]);
         if (key != NULL && json_get(value, key) == NULL)
            return fail(c, "missing required property '%s'", key);
      }
   }

   const json_t *props = json_get(schema, "properties");

   for (size_t i = 0; i < value->object.count; i++) {
      const char *key = value->object.keys[i];
      const json_t *sub = json_get(props, key);

      if (sub == NULL) {
         const json_t *extra = json_get(schema, "additionalProperties");
         bool allowed = true;
         if (json_as_bool(extra, &allowed) && !allowed) {
            LOCAL_TEXT_BUF names = tb_new();
            if (json_is(props, JSON_OBJECT)) {
               for (size_t j = 0; j < props->object.count; j++) {
                  if (j > 0)
                     tb_cat(names, ", ");
                  tb_cat(names, props->object.keys[j]);
               }
            }

            return fail(c, "unexpected property '%s' (allowed: %s)", key,
                        tb_len(names) > 0 ? tb_get(names) : "none");
         }

         continue;
      }

      const size_t mark = tb_len(c->path);
      tb_printf(c->path, ".%s", key);

      const bool ok = check_value(c, sub, value->object.values[i], depth + 1);

      tb_trim(c->path, mark);

      if (!ok)
         return false;
   }

   return true;
}

static bool check_value(check_t *c, const json_t *schema, const json_t *value,
                        unsigned depth)
{
   if (!json_is(schema, JSON_OBJECT))
      return true;   // Boolean or unknown schemas accept anything
   else if (depth > MAX_SCHEMA_DEPTH)
      return fail(c, "nesting too deep");

   const json_t *type = json_get(schema, "type");
   if (type != NULL && !check_type(c, type, value))
      return false;

   if (!check_enum(c, json_get(schema, "enum"), value))
      return false;

   switch (value->kind) {
   case JSON_INT:
   case JSON_REAL:
      return check_number(c, schema, value);
   case JSON_STRING:
      return check_string(c, schema, value);
   case JSON_ARRAY:
      return check_array(c, schema, value, depth);
   case JSON_OBJECT:
      return check_object(c, schema, value, depth);
   default:
      return true;
   }
}

bool mcp_schema_validate(const json_t *schema, const json_t *value,
                         text_buf_t *err)
{
   LOCAL_TEXT_BUF path = tb_new();
   tb_cat(path, "arguments");

   check_t c = { .err = err, .path = path };
   return check_value(&c, schema, value, 0);
}
