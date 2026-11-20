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

#ifndef _MCP_COND_H
#define _MCP_COND_H

#include <stdbool.h>
#include <stddef.h>

//
// Conditions on signal values, written in a small VHDL-like language:
//
//   /dut/valid = '1' and /dut/ready = '0'
//   /dut/state = SEND or /dut/cnt >= 16
//   rose(/dut/start) and /dut/data /= x"00"
//
// A signal is a design path starting with /.  Literals are '1' (a bit or
// character), "0101" (bits), x"1F" (hex), 42 (a number) and a bare name
// such as SEND (an enumeration literal).  Operators are = /= < <= > >=,
// not, and, or and parentheses; rose, fell, changed and stable look at the
// previous sample as well.  A signal on its own is true when it is '1'.
//

typedef struct _cond cond_t;

// Returns NULL after writing a message that says where the problem is
cond_t *cond_parse(const char *text, char *err, size_t errlen);
void cond_free(cond_t *c);

// The distinct signals the condition reads, in order of appearance
size_t cond_nrefs(const cond_t *c);
const char *cond_ref(const cond_t *c, size_t index);

// The value of one signal at a sample.  Bit vectors and single bits are
// strings of 0 1 U X Z W L H - with the most significant bit first; other
// types are their printed text.  prev is the value at the previous sample
// and is NULL for the first.
typedef struct {
   const char *cur;
   const char *prev;
   bool        bits;
} cond_val_t;

// vals has one entry for each reference
bool cond_eval(const cond_t *c, const cond_val_t *vals);

#endif   // _MCP_COND_H
