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

#ifndef _MCP_SCHEMA_H
#define _MCP_SCHEMA_H

#include "mcp/mcp-json.h"

// Validates a value against the subset of JSON Schema 2020-12 used by
// the tool definitions: type, enum, properties, required,
// additionalProperties (false), items, minItems, maxItems, minLength,
// maxLength, minimum and maximum.  Other keywords are ignored.  Remote
// $ref values are never followed.
//
// On failure returns false and appends a message that names the
// offending path (prefixed by "arguments") to err.
bool mcp_schema_validate(const json_t *schema, const json_t *value,
                         text_buf_t *err);

#endif   // _MCP_SCHEMA_H
