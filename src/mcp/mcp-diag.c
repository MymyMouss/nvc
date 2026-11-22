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
#include "diag.h"
#include "mcp/mcp-child.h"
#include "mcp/mcp-json.h"

#include <errno.h>
#include <unistd.h>

static int diag_fd = -1;

static const char *severity_name(diag_level_t level)
{
   switch (level) {
   case DIAG_DEBUG:   return "debug";
   case DIAG_NOTE:    return "note";
   case DIAG_WARN:    return "warning";
   case DIAG_ERROR:   return "error";
   case DIAG_FAILURE: return "failure";
   case DIAG_FATAL:   return "fatal";
   }

   return "error";
}

static void write_all(int fd, const char *buf, size_t len)
{
   while (len > 0) {
      const ssize_t n = write(fd, buf, len);
      if (n < 0 && errno == EINTR)
         continue;
      else if (n <= 0)
         return;   // The supervisor went away

      buf += n;
      len -= n;
   }
}

static void diag_to_json(diag_t *d, void *context)
{
   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t w;
   jw_init(&w, tb);

   jw_begin_object(&w);

   jw_key(&w, "severity");
   jw_string(&w, severity_name(diag_level(d, NULL)));

   const loc_t *loc = diag_get_loc(d);
   if (loc != NULL && !loc_invalid_p(loc)) {
      const char *file = loc_file_str(loc);
      if (file != NULL) {
         jw_key(&w, "file");
         jw_string(&w, file);
      }

      jw_key(&w, "line");
      jw_int(&w, loc->first_line);

      jw_key(&w, "column");
      jw_int(&w, loc->first_column + 1);

      if (loc->line_delta != DELTA_INVALID) {
         jw_key(&w, "end_line");
         jw_int(&w, loc->first_line + loc->line_delta);

         if (loc->line_delta == 0 && loc->column_delta != DELTA_INVALID) {
            jw_key(&w, "end_column");
            jw_int(&w, loc->first_column + loc->column_delta + 1);
         }
      }
   }

   jw_key(&w, "message");
   jw_string(&w, diag_get_text(d));

   const int nhints = diag_hints(d);
   if (nhints > 0) {
      jw_key(&w, "hints");
      jw_begin_array(&w);
      for (int i = 0; i < nhints; i++) {
         const char *hint = diag_get_hint(d, i);
         if (hint != NULL)
            jw_string(&w, hint);
      }
      jw_end_array(&w);
   }

   jw_end_object(&w);
   tb_append(tb, '\n');

   write_all(diag_fd, tb_get(tb), tb_len(tb));
}

void mcp_diag_install(int fd)
{
   diag_fd = fd;
   diag_set_consumer(diag_to_json, NULL);
}
