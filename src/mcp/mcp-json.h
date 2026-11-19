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

#ifndef _MCP_JSON_H
#define _MCP_JSON_H

#include "prim.h"
#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//
// Bounded JSON parser
//
// The parser is meant for untrusted input: it enforces a maximum input
// size and nesting depth, validates UTF-8, and rejects duplicate object
// keys.  All nodes live in a memory pool owned by the caller.
//

typedef enum {
   JSON_NULL,
   JSON_BOOL,
   JSON_INT,
   JSON_REAL,
   JSON_STRING,
   JSON_ARRAY,
   JSON_OBJECT,
} json_kind_t;

typedef struct _json json_t;

struct _json {
   json_kind_t kind;
   union {
      bool    boolean;
      int64_t integer;
      double  real;
      struct {
         const char *chars;   // NUL terminated
         size_t      len;
      } string;
      struct {
         json_t **items;
         size_t   count;
      } array;
      struct {
         const char **keys;
         json_t     **values;
         size_t       count;
      } object;
   };
};

typedef struct {
   size_t   max_bytes;
   unsigned max_depth;
} json_limits_t;

#define JSON_DEFAULT_MAX_BYTES  (1024 * 1024)
#define JSON_DEFAULT_MAX_DEPTH  32

// Returns NULL and sets *err to a static message on failure
json_t *json_parse(mem_pool_t *mp, const char *text, size_t len,
                   const json_limits_t *limits, const char **err);

// Accessors that are safe to call on NULL or a node of the wrong kind
json_t *json_get(const json_t *obj, const char *key);
json_t *json_at(const json_t *arr, size_t index);
const char *json_string(const json_t *j);
bool json_is(const json_t *j, json_kind_t kind);
bool json_as_int(const json_t *j, int64_t *value);
bool json_as_bool(const json_t *j, bool *value);

//
// Streaming JSON writer
//
// Output is always a single line: strings are escaped and invalid UTF-8
// is replaced with U+FFFD so the result is safe to send over a
// newline-delimited transport.
//

#define JSON_WRITER_MAX_DEPTH 32

typedef struct {
   text_buf_t *tb;
   unsigned    depth;
   bool        after_key;
   bool        first[JSON_WRITER_MAX_DEPTH + 1];
} json_writer_t;

void jw_init(json_writer_t *w, text_buf_t *tb);
void jw_begin_object(json_writer_t *w);
void jw_end_object(json_writer_t *w);
void jw_begin_array(json_writer_t *w);
void jw_end_array(json_writer_t *w);
void jw_key(json_writer_t *w, const char *key);
void jw_string(json_writer_t *w, const char *str);
void jw_stringn(json_writer_t *w, const char *str, size_t len);
void jw_int(json_writer_t *w, int64_t value);
void jw_real(json_writer_t *w, double value);
void jw_bool(json_writer_t *w, bool value);
void jw_null(json_writer_t *w);
void jw_raw(json_writer_t *w, const char *json);   // Already serialised

// Append already serialised object members such as "a":1,"b":2 to the
// object being written
void jw_raw_members(json_writer_t *w, const char *members);

#endif   // _MCP_JSON_H
