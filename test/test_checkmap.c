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

#include "test_util.h"
#include "jit/jit.h"
#include "rt/checkmap.h"

#include <check.h>
#include <string.h>

#define NL JIT_NO_LABEL
#define NEXT JIT_FLOW_NEXT
#define JUMP JIT_FLOW_JUMP
#define BRANCH JIT_FLOW_BRANCH
#define END JIT_FLOW_END

// Blocks [0,2) [2,4) [4,5) [5,6): the second instruction branches to 4 and
// the fourth jumps to 5.  The branch was never taken so block 4 never ran.
static const uint64_t old_hash[] = { 1, 2, 3, 4, 5, 6 };
static const uint32_t old_label[] = { NL, 4, NL, 5, NL, NL };
static const uint8_t old_kind[] = { NEXT, BRANCH, NEXT, JUMP, NEXT, END };
static const uint8_t old_exec[] = { 1, 1, 1, 1, 0, 1 };

static code_view_t old_view(void)
{
   return (code_view_t) {
      .n = 6, .hash = old_hash, .label = old_label, .kind = old_kind,
      .exec = old_exec,
   };
}

START_TEST(test_identity)
{
   const code_view_t o = old_view();
   const int32_t image[] = { 0, -1, 2, -1, -1, 5 };
   ck_assert_ptr_null(checkmap_confirm(&o, &o, image));

   // The proposal finds it
   int32_t found[6];
   checkmap_propose(&o, &o, found);
   ck_assert_ptr_null(checkmap_confirm(&o, &o, found));
   ck_assert_int_eq(found[5], 5);
   ck_assert_int_eq(found[4], -1);
}
END_TEST

START_TEST(test_unrun_block_may_differ)
{
   // The block that never ran is different, longer and elsewhere
   const uint64_t hash[] = { 1, 2, 3, 4, 77, 78, 6 };
   const uint32_t label[] = { NL, 4, NL, 6, NL, NL, NL };
   const uint8_t kind[] = { NEXT, BRANCH, NEXT, JUMP, NEXT, NEXT, END };
   const code_view_t n = { .n = 7, .hash = hash, .label = label,
                           .kind = kind };
   const code_view_t o = old_view();

   int32_t image[6];
   checkmap_propose(&o, &n, image);
   ck_assert_ptr_null(checkmap_confirm(&o, &n, image));
   ck_assert_int_eq(image[5], 6);
}
END_TEST

START_TEST(test_changed_block_that_ran)
{
   const uint64_t hash[] = { 1, 2, 33, 4, 5, 6 };   // Block 2 changed
   const code_view_t n = { .n = 6, .hash = hash, .label = old_label,
                           .kind = old_kind };
   const code_view_t o = old_view();
   const int32_t image[] = { 0, -1, 2, -1, -1, 5 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &n, image));

   int32_t found[6];
   checkmap_propose(&o, &n, found);
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &n, found));
}
END_TEST

START_TEST(test_wrong_mappings)
{
   const code_view_t o = old_view();

   // The entry must map to the entry
   const int32_t bad_entry[] = { 2, -1, 0, -1, -1, 5 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, bad_entry));

   // A block that ran has to be mapped
   const int32_t missing[] = { 0, -1, 2, -1, -1, -1 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, missing));

   // Two blocks cannot share an image
   const int32_t shared[] = { 0, -1, 2, -1, -1, 2 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, shared));

   // The image must be the start of a block
   const int32_t middle[] = { 0, -1, 2, -1, -1, 3 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, middle));

   // Contents that differ: mapping the last block onto the first
   const int32_t swapped[] = { 0, -1, 5, -1, -1, 2 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, swapped));
}
END_TEST

START_TEST(test_branch_target)
{
   // The jump that ran now goes to block 4 instead of 5
   const uint32_t label[] = { NL, 4, NL, 4, NL, NL };
   const code_view_t n = { .n = 6, .hash = old_hash, .label = label,
                           .kind = old_kind };
   const code_view_t o = old_view();
   const int32_t image[] = { 0, -1, 2, -1, -1, 5 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &n, image));
}
END_TEST

START_TEST(test_fall_through)
{
   // A block is inserted between two that ran one after the other
   const uint64_t hash[] = { 1, 2, 9, 3, 4, 5, 6 };
   const uint32_t label[] = { NL, 5, NL, NL, 6, NL, NL };
   const uint8_t kind[] = { NEXT, BRANCH, NEXT, NEXT, JUMP, NEXT, END };
   const code_view_t n = { .n = 7, .hash = hash, .label = label,
                           .kind = kind };
   const code_view_t o = old_view();

   // Mapping block 2 to where it now is does not make up for the new
   // block in the way
   const int32_t image[] = { 0, -1, 3, -1, -1, 6 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &n, image));
}
END_TEST

START_TEST(test_unknown_marks)
{
   const code_view_t o = { .n = 6, .hash = old_hash, .label = old_label,
                           .kind = old_kind };   // No marks
   const int32_t image[] = { 0, -1, 2, -1, -1, 5 };
   ck_assert_ptr_nonnull(checkmap_confirm(&o, &o, image));

   code_view_t u = old_view();
   u.unsupported = true;
   ck_assert_ptr_nonnull(checkmap_confirm(&u, &u, image));
}
END_TEST

Suite *get_checkmap_tests(void)
{
   Suite *s = suite_create("checkmap");

   TCase *tc = tcase_create("core");
   tcase_add_test(tc, test_identity);
   tcase_add_test(tc, test_unrun_block_may_differ);
   tcase_add_test(tc, test_changed_block_that_ran);
   tcase_add_test(tc, test_wrong_mappings);
   tcase_add_test(tc, test_branch_target);
   tcase_add_test(tc, test_fall_through);
   tcase_add_test(tc, test_unknown_marks);
   suite_add_tcase(s, tc);

   return s;
}
