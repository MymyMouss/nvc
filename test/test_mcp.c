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
#include "mcp/mcp-cond.h"
#include "mcp/mcp-json.h"
#include "mcp/mcp-schema.h"
#include "mcp/mcp-server.h"

#include <ftw.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////
// JSON

static json_t *parse_ok(mem_pool_t *mp, const char *text)
{
   const char *err = NULL;
   json_t *j = json_parse(mp, text, strlen(text), NULL, &err);
   ck_assert_msg(j != NULL, "'%s' should parse but got: %s", text, err);
   return j;
}

static void parse_fails(const char *text, const char *expect)
{
   mem_pool_t *mp = pool_new();
   const char *err = NULL;
   json_t *j = json_parse(mp, text, strlen(text), NULL, &err);
   ck_assert_msg(j == NULL, "'%s' should not parse", text);
   ck_assert_ptr_nonnull(err);
   ck_assert_msg(strstr(err, expect) != NULL,
                 "'%s': expected error containing '%s' but got '%s'",
                 text, expect, err);
   pool_free(mp);
}

START_TEST(test_json_parse)
{
   mem_pool_t *mp = pool_new();

   json_t *j = parse_ok(mp, " { \"a\" : [1, 2.5, -3, true, null, "
                        "\"x\\n\\u00e9\\ud83d\\ude00\"], \"b\": {} } ");
   ck_assert_int_eq(j->kind, JSON_OBJECT);

   json_t *a = json_get(j, "a");
   ck_assert_ptr_nonnull(a);
   ck_assert_int_eq(a->array.count, 6);

   int64_t i;
   ck_assert(json_as_int(json_at(a, 0), &i));
   ck_assert_int_eq(i, 1);
   ck_assert_int_eq(json_at(a, 1)->kind, JSON_REAL);
   ck_assert(json_at(a, 1)->real == 2.5);
   ck_assert(json_as_int(json_at(a, 2), &i));
   ck_assert_int_eq(i, -3);

   bool b;
   ck_assert(json_as_bool(json_at(a, 3), &b));
   ck_assert(b);
   ck_assert_int_eq(json_at(a, 4)->kind, JSON_NULL);
   ck_assert_str_eq(json_string(json_at(a, 5)),
                    "x\n\xc3\xa9\xf0\x9f\x98\x80");

   ck_assert_int_eq(json_get(j, "b")->kind, JSON_OBJECT);
   ck_assert_ptr_null(json_get(j, "missing"));
   ck_assert_ptr_null(json_get(a, "a"));   // Wrong kind
   ck_assert_ptr_null(json_at(a, 99));

   // Integers that do not fit are kept as reals
   j = parse_ok(mp, "123456789012345678901234567890");
   ck_assert_int_eq(j->kind, JSON_REAL);

   // An integral real converts to an integer
   j = parse_ok(mp, "3.0");
   ck_assert(json_as_int(j, &i));
   ck_assert_int_eq(i, 3);
   ck_assert(!json_as_int(parse_ok(mp, "3.5"), &i));

   pool_free(mp);
}
END_TEST

START_TEST(test_json_errors)
{
   parse_fails("", "unexpected end");
   parse_fails("{", "expected string key");
   parse_fails("[1,]", "invalid number");
   parse_fails("[1 2]", "expected ',' or ']'");
   parse_fails("{\"a\":1,}", "expected string key");
   parse_fails("{\"a\" 1}", "expected ':'");
   parse_fails("{\"a\":1,\"a\":2}", "duplicate key");
   parse_fails("01", "unexpected data");
   parse_fails("+1", "invalid number");
   parse_fails("1.", "invalid number");
   parse_fails("1e", "invalid number");
   parse_fails("nul", "invalid literal");
   parse_fails("1 2", "unexpected data");
   parse_fails("\"abc", "unterminated");
   parse_fails("\"\\x\"", "invalid escape");
   parse_fails("\"\\u12\"", "invalid \\u escape");
   parse_fails("\"\\ud800\"", "unpaired surrogate");
   parse_fails("\"\\udc00\"", "unpaired surrogate");
   parse_fails("\"\\ud800\\u0041\"", "unpaired surrogate");
   parse_fails("\"\\u0000\"", "NUL");
   parse_fails("\"a\x01\"", "control character");
   parse_fails("\"\xc0\xaf\"", "invalid UTF-8");           // Overlong
   parse_fails("\"\xed\xa0\x80\"", "invalid UTF-8");       // Surrogate
   parse_fails("\"\xf4\x90\x80\x80\"", "invalid UTF-8");   // > U+10FFFF
   parse_fails("\"\xe2\x82\"", "invalid UTF-8");           // Truncated
   parse_fails("\"\x80\"", "invalid UTF-8");               // Stray continuation
}
END_TEST

START_TEST(test_json_limits)
{
   char deep[128];
   memset(deep, '[', 40);
   memset(deep + 40, ']', 40);
   deep[80] = '\0';
   parse_fails(deep, "nesting depth");

   mem_pool_t *mp = pool_new();
   const char *err;

   const json_limits_t small = { .max_bytes = 8, .max_depth = 4 };
   ck_assert_ptr_null(json_parse(mp, "[1,2,3,4,5]", 11, &small, &err));
   ck_assert_str_eq(err, "input too large");

   // Depth exactly at the limit is accepted
   const json_limits_t shallow = { .max_bytes = 100, .max_depth = 2 };
   ck_assert_ptr_nonnull(json_parse(mp, "[[1]]", 5, &shallow, &err));
   ck_assert_ptr_null(json_parse(mp, "[[[1]]]", 7, &shallow, &err));

   pool_free(mp);
}
END_TEST

START_TEST(test_json_writer)
{
   LOCAL_TEXT_BUF tb = tb_new();
   json_writer_t w;
   jw_init(&w, tb);

   jw_begin_object(&w);
   jw_key(&w, "s");
   jw_string(&w, "a\"b\\c\nd\te\x01\x7f");
   jw_key(&w, "n");
   jw_int(&w, -42);
   jw_key(&w, "r");
   jw_real(&w, 0.5);
   jw_key(&w, "inf");
   jw_real(&w, 1.0 / 0.0);
   jw_key(&w, "list");
   jw_begin_array(&w);
   jw_bool(&w, true);
   jw_null(&w);
   jw_begin_object(&w);
   jw_end_object(&w);
   jw_raw(&w, "[1,2]");
   jw_end_array(&w);
   jw_key(&w, "bad");
   jw_string(&w, "ok\xff\xc3(");   // Invalid UTF-8 is replaced
   jw_end_object(&w);

   ck_assert_str_eq(tb_get(tb),
                    "{\"s\":\"a\\\"b\\\\c\\nd\\te\\u0001\\u007f\","
                    "\"n\":-42,\"r\":0.5,\"inf\":null,"
                    "\"list\":[true,null,{},[1,2]],"
                    "\"bad\":\"ok\xef\xbf\xbd\xef\xbf\xbd(\"}");

   // The output never contains a raw newline and parses back
   ck_assert_ptr_null(strchr(tb_get(tb), '\n'));

   mem_pool_t *mp = pool_new();
   json_t *j = parse_ok(mp, tb_get(tb));
   ck_assert_str_eq(json_string(json_get(j, "s")),
                    "a\"b\\c\nd\te\x01\x7f");
   pool_free(mp);
}
END_TEST

////////////////////////////////////////////////////////////////////////////////
// Schema validation

static const char *validate(const char *schema, const char *value)
{
   static char result[512];

   mem_pool_t *mp = pool_new();
   json_t *s = parse_ok(mp, schema);
   json_t *v = parse_ok(mp, value);

   LOCAL_TEXT_BUF err = tb_new();
   const bool ok = mcp_schema_validate(s, v, err);

   if (ok) {
      ck_assert_int_eq(tb_len(err), 0);
      result[0] = '\0';
   }
   else {
      ck_assert_int_gt(tb_len(err), 0);
      snprintf(result, sizeof(result), "%s", tb_get(err));
   }

   pool_free(mp);
   return ok ? NULL : result;
}

START_TEST(test_schema)
{
   static const char tool[] =
      "{\"type\":\"object\",\"properties\":{"
      "\"files\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":2,"
      "\"items\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":5}},"
      "\"std\":{\"type\":\"string\",\"enum\":[\"1993\",\"2008\"]},"
      "\"n\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10},"
      "\"flag\":{\"type\":\"boolean\"}},"
      "\"required\":[\"files\"],\"additionalProperties\":false}";

   ck_assert_ptr_null(validate(tool, "{\"files\":[\"a.vhd\"]}"));
   ck_assert_ptr_null(validate(tool, "{\"files\":[\"a\"],\"std\":\"2008\","
                               "\"n\":10,\"flag\":false}"));
   ck_assert_ptr_null(validate(tool, "{\"files\":[\"a\"],\"n\":3.0}"));

   const char *e;

   e = validate(tool, "{}");
   ck_assert_ptr_nonnull(e);
   ck_assert_ptr_nonnull(strstr(e, "missing required property 'files'"));

   e = validate(tool, "{\"files\":\"a\"}");
   ck_assert_str_eq(e, "arguments.files: expected array but got string");

   e = validate(tool, "{\"files\":[\"a\",3]}");
   ck_assert_str_eq(e, "arguments.files[1]: expected string but got integer");

   e = validate(tool, "{\"files\":[]}");
   ck_assert_ptr_nonnull(strstr(e, "at least 1 items"));

   e = validate(tool, "{\"files\":[\"a\",\"b\",\"c\"]}");
   ck_assert_ptr_nonnull(strstr(e, "at most 2 items"));

   e = validate(tool, "{\"files\":[\"toolong\"]}");
   ck_assert_ptr_nonnull(strstr(e, "at most 5 characters"));

   e = validate(tool, "{\"files\":[\"\"]}");
   ck_assert_ptr_nonnull(strstr(e, "at least 1 characters"));

   e = validate(tool, "{\"files\":[\"a\"],\"std\":\"1987\"}");
   ck_assert_ptr_nonnull(strstr(e, "must be one of \"1993\", \"2008\""));

   e = validate(tool, "{\"files\":[\"a\"],\"n\":11}");
   ck_assert_ptr_nonnull(strstr(e, "at most 10"));

   e = validate(tool, "{\"files\":[\"a\"],\"n\":0}");
   ck_assert_ptr_nonnull(strstr(e, "at least 1"));

   e = validate(tool, "{\"files\":[\"a\"],\"n\":1.5}");
   ck_assert_ptr_nonnull(strstr(e, "expected integer but got number"));

   e = validate(tool, "{\"files\":[\"a\"],\"bogus\":1}");
   ck_assert_ptr_nonnull(strstr(e, "unexpected property 'bogus'"));
   ck_assert_ptr_nonnull(strstr(e, "allowed: files, std, n, flag"));

   e = validate(tool, "[]");
   ck_assert_ptr_nonnull(strstr(e, "expected object but got array"));
}
END_TEST

////////////////////////////////////////////////////////////////////////////////
// Protocol

#define MODERN_META                                                     \
   "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"," \
   "\"io.modelcontextprotocol/clientCapabilities\":{}}"

typedef struct {
   char   **lines;
   size_t   count;
} sink_t;

static void sink_write(void *ctx, const char *line, size_t len)
{
   sink_t *s = ctx;
   s->lines = xrealloc_array(s->lines, s->count + 1, sizeof(char *));
   s->lines[s->count++] = xstrndup(line, len);
}

static void sink_clear(sink_t *s)
{
   for (size_t i = 0; i < s->count; i++)
      free(s->lines[i]);
   free(s->lines);
   s->lines = NULL;
   s->count = 0;
}

static mcp_server_t *new_server(sink_t *sink, const char *root,
                                const char *exe)
{
   const mcp_options_t opts = {
      .write       = sink_write,
      .write_ctx   = sink,
      .in_fd       = -1,
      .exe         = exe,
      .root        = root,
   };

   return mcp_server_new(&opts);
}

// Send one message and return the single response as parsed JSON
static json_t *call(mem_pool_t *mp, mcp_server_t *srv, sink_t *sink,
                    const char *line)
{
   sink_clear(sink);
   mcp_server_handle_line(srv, line, strlen(line));
   ck_assert_int_eq(sink->count, 1);
   return parse_ok(mp, sink->lines[0]);
}

static int64_t error_code(const json_t *resp)
{
   int64_t code;
   const json_t *err = json_get(resp, "error");
   ck_assert_ptr_nonnull(err);
   ck_assert(json_as_int(json_get(err, "code"), &code));
   return code;
}

static const char *error_message(const json_t *resp)
{
   return json_string(json_get(json_get(resp, "error"), "message"));
}

START_TEST(test_proto_modern)
{
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, "/tmp", "/bin/true");

   json_t *r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":1,"
                    "\"method\":\"server/discover\",\"params\":{"
                    MODERN_META "}}");
   int64_t id;
   ck_assert(json_as_int(json_get(r, "id"), &id));
   ck_assert_int_eq(id, 1);
   const json_t *res = json_get(r, "result");
   ck_assert_str_eq(json_string(json_get(res, "resultType")), "complete");
   ck_assert_str_eq(json_string(json_at(json_get(res, "supportedVersions"), 0)),
                    "2026-07-28");
   ck_assert_ptr_nonnull(json_get(json_get(res, "capabilities"), "tools"));
   ck_assert_str_eq(json_string(json_get(json_get(json_get(res, "_meta"),
                                      "io.modelcontextprotocol/serverInfo"),
                                      "name")), "nvc");
   ck_assert_ptr_nonnull(strstr(json_string(json_get(res, "instructions")),
                                "untrusted"));
   ck_assert_ptr_nonnull(json_get(res, "ttlMs"));
   ck_assert_str_eq(json_string(json_get(res, "cacheScope")), "public");

   // The tool list is the same on every call
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":\"abc\","
            "\"method\":\"tools/list\",\"params\":{" MODERN_META "}}");
   ck_assert_str_eq(json_string(json_get(r, "id")), "abc");
   res = json_get(r, "result");
   ck_assert_str_eq(json_string(json_get(res, "resultType")), "complete");
   const json_t *tools = json_get(res, "tools");
   ck_assert_int_ge(tools->array.count, 1);

   const json_t *t0 = json_at(tools, 0);
   ck_assert_str_eq(json_string(json_get(t0, "name")), "nvc_analyse");
   ck_assert_ptr_nonnull(json_get(t0, "title"));
   ck_assert_ptr_nonnull(json_get(t0, "description"));
   ck_assert_ptr_nonnull(json_get(t0, "outputSchema"));
   ck_assert_str_eq(json_string(json_get(json_get(t0, "inputSchema"),
                                         "type")), "object");

   const json_t *ann = json_get(t0, "annotations");
   bool flag;
   ck_assert(json_as_bool(json_get(ann, "readOnlyHint"), &flag));
   ck_assert(!flag);
   ck_assert(json_as_bool(json_get(ann, "destructiveHint"), &flag));
   ck_assert(!flag);
   ck_assert(json_as_bool(json_get(ann, "openWorldHint"), &flag));
   ck_assert(!flag);

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
}
END_TEST

START_TEST(test_proto_errors)
{
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, "/tmp", "/bin/true");

   // Not JSON
   json_t *r = call(mp, srv, &sink, "{not json");
   ck_assert_int_eq(error_code(r), -32700);
   ck_assert_int_eq(json_get(r, "id")->kind, JSON_NULL);

   // Batches are not supported
   r = call(mp, srv, &sink, "[{\"jsonrpc\":\"2.0\",\"id\":1,"
            "\"method\":\"ping\"}]");
   ck_assert_int_eq(error_code(r), -32600);

   // Request ids must be strings or integers
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":null,"
            "\"method\":\"ping\"}");
   ck_assert_int_eq(error_code(r), -32600);
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":1.5,"
            "\"method\":\"ping\"}");
   ck_assert_int_eq(error_code(r), -32600);

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":2}");
   ck_assert_int_eq(error_code(r), -32600);

   // params must be an object
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":2,"
            "\"method\":\"tools/list\",\"params\":[]}");
   ck_assert_int_eq(error_code(r), -32602);

   // No protocol version and no initialize
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":3,"
            "\"method\":\"tools/list\"}");
   ck_assert_int_eq(error_code(r), -32602);
   ck_assert_ptr_nonnull(strstr(error_message(r), "protocolVersion"));

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":3,"
            "\"method\":\"server/discover\",\"params\":{}}");
   ck_assert_int_eq(error_code(r), -32602);

   // Unsupported version reports what is supported
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":4,"
            "\"method\":\"tools/list\",\"params\":{\"_meta\":{"
            "\"io.modelcontextprotocol/protocolVersion\":\"1900-01-01\","
            "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
   ck_assert_int_eq(error_code(r), -32022);
   const json_t *data = json_get(json_get(r, "error"), "data");
   ck_assert_str_eq(json_string(json_at(json_get(data, "supported"), 0)),
                    "2026-07-28");
   ck_assert_str_eq(json_string(json_get(data, "requested")), "1900-01-01");

   // Capabilities are required on every modern request
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":5,"
            "\"method\":\"tools/list\",\"params\":{\"_meta\":{"
            "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}");
   ck_assert_int_eq(error_code(r), -32602);
   ck_assert_ptr_nonnull(strstr(error_message(r), "clientCapabilities"));

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":6,"
            "\"method\":\"nope\",\"params\":{" MODERN_META "}}");
   ck_assert_int_eq(error_code(r), -32601);

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":7,"
            "\"method\":\"tools/call\",\"params\":{\"name\":\"nope\","
            MODERN_META "}}");
   ck_assert_int_eq(error_code(r), -32602);
   ck_assert_ptr_nonnull(strstr(error_message(r), "Unknown tool: nope"));

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":8,"
            "\"method\":\"tools/call\",\"params\":{" MODERN_META "}}");
   ck_assert_int_eq(error_code(r), -32602);

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":9,"
            "\"method\":\"tools/call\",\"params\":{\"name\":\"nvc_analyse\","
            "\"arguments\":[]," MODERN_META "}}");
   ck_assert_int_eq(error_code(r), -32602);

   // Notifications and unknown responses never produce output
   sink_clear(&sink);
   static const char *const silent[] = {
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/whatever\"}",
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
      "\"params\":{\"requestId\":99}}",
      "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{}}",
   };
   for (size_t i = 0; i < ARRAY_LEN(silent); i++)
      mcp_server_handle_line(srv, silent[i], strlen(silent[i]));
   ck_assert_int_eq(sink.count, 0);

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
}
END_TEST

START_TEST(test_proto_legacy)
{
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, "/tmp", "/bin/true");

   json_t *r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":0,"
                    "\"method\":\"initialize\",\"params\":{"
                    "\"protocolVersion\":\"2025-06-18\","
                    "\"capabilities\":{},"
                    "\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");
   const json_t *res = json_get(r, "result");
   ck_assert_str_eq(json_string(json_get(res, "protocolVersion")),
                    "2025-06-18");
   ck_assert_ptr_nonnull(json_get(json_get(res, "capabilities"), "tools"));
   ck_assert_str_eq(json_string(json_get(json_get(res, "serverInfo"), "name")),
                    "nvc");
   ck_assert_ptr_nonnull(json_get(res, "instructions"));
   ck_assert_ptr_null(json_get(res, "resultType"));

   // After initialize no _meta is needed and results omit resultType
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":1,"
            "\"method\":\"tools/list\"}");
   res = json_get(r, "result");
   ck_assert_ptr_null(json_get(res, "resultType"));
   ck_assert_ptr_null(json_get(res, "ttlMs"));
   ck_assert_int_ge(json_get(res, "tools")->array.count, 1);

   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":2,"
            "\"method\":\"ping\"}");
   ck_assert_ptr_nonnull(json_get(r, "result"));

   // A client that asks for an unknown revision is offered the newest
   // legacy one
   sink_t sink2 = {};
   mcp_server_t *srv2 = new_server(&sink2, "/tmp", "/bin/true");
   r = call(mp, srv2, &sink2, "{\"jsonrpc\":\"2.0\",\"id\":0,"
            "\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2024-11-05\"}}");
   ck_assert_str_eq(json_string(json_get(json_get(r, "result"),
                                         "protocolVersion")), "2025-11-25");

   // A client may still use the modern revision on the same process
   r = call(mp, srv, &sink, "{\"jsonrpc\":\"2.0\",\"id\":3,"
            "\"method\":\"tools/list\",\"params\":{" MODERN_META "}}");
   ck_assert_str_eq(json_string(json_get(json_get(r, "result"),
                                         "resultType")), "complete");

   mcp_server_free(srv);
   mcp_server_free(srv2);
   sink_clear(&sink);
   sink_clear(&sink2);
   pool_free(mp);
}
END_TEST

////////////////////////////////////////////////////////////////////////////////
// Tool execution

static int rm_entry(const char *path, const struct stat *sb, int flag,
                    struct FTW *ftw)
{
   remove(path);
   return 0;
}

static void remove_tree(const char *path)
{
   nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static char *make_workspace(void)
{
   char tmpl[] = "/tmp/nvc-mcp-XXXXXX";
   ck_assert_ptr_nonnull(mkdtemp(tmpl));

   char *real = realpath(tmpl, NULL);
   ck_assert_ptr_nonnull(real);
   return real;
}

static void write_file(const char *root, const char *name, const char *text,
                       mode_t mode)
{
   char *path = xasprintf("%s/%s", root, name);
   FILE *f = fopen(path, "w");
   ck_assert_ptr_nonnull(f);
   fputs(text, f);
   fclose(f);
   chmod(path, mode);
   free(path);
}

// The nvc executable and library live next to the unit test binary
static char *build_path(const char *rel)
{
   char self[PATH_MAX];
   const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
   ck_assert_int_gt(n, 0);
   self[n] = '\0';

   char *slash = strrchr(self, '/');
   ck_assert_ptr_nonnull(slash);
   *slash = '\0';   // .../bin

   char *joined = xasprintf("%s/../%s", self, rel);
   char *real = realpath(joined, NULL);
   free(joined);
   ck_assert_msg(real != NULL, "cannot find %s", rel);
   return real;
}

static json_t *analyse(mem_pool_t *mp, mcp_server_t *srv, sink_t *sink,
                       const char *args)
{
   char *req = xasprintf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                         "\"tools/call\",\"params\":{\"name\":\"nvc_analyse\","
                         "\"arguments\":%s," MODERN_META "}}", args);
   json_t *r = call(mp, srv, sink, req);
   free(req);
   return json_get(r, "result");
}

static const json_t *structured(const json_t *result)
{
   bool is_error = true;
   ck_assert(json_as_bool(json_get(result, "isError"), &is_error));
   ck_assert_msg(!is_error, "unexpected tool error: %s",
                 json_string(json_get(json_at(json_get(result, "content"), 0),
                                      "text")));

   const json_t *sc = json_get(result, "structuredContent");
   ck_assert_ptr_nonnull(sc);

   // The text content repeats the structured content
   const char *text = json_string(json_get(json_at(json_get(result,
                                                   "content"), 0), "text"));
   ck_assert_ptr_nonnull(text);
   ck_assert_ptr_nonnull(strchr(text, '{'));
   return sc;
}

static const char *tool_error(const json_t *result)
{
   bool is_error = false;
   ck_assert(json_as_bool(json_get(result, "isError"), &is_error));
   ck_assert(is_error);
   ck_assert_ptr_null(json_get(result, "structuredContent"));
   return json_string(json_get(json_at(json_get(result, "content"), 0),
                               "text"));
}

START_TEST(test_analyse_real)
{
   char *root = make_workspace();
   char *nvc = build_path("bin/nvc");
   char *lib = build_path("lib");
   setenv("NVC_LIBPATH", lib, 1);

   write_file(root, "good.vhd",
              "entity good is end entity;\n"
              "architecture a of good is begin end architecture;\n", 0644);
   write_file(root, "bad.vhd",
              "entity bad is end entity;\n"
              "architecture a of bad is\n"
              "  signal s : integer := nosuch;\n"
              "  signal t : integer := alsonot;\n"
              "begin\nend architecture;\n", 0644);
   write_file(root, "-dash.vhd",
              "entity dash is end entity;\n"
              "architecture a of dash is begin end architecture;\n", 0644);

   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, root, nvc);

   const json_t *sc =
      structured(analyse(mp, srv, &sink, "{\"files\":[\"good.vhd\"]}"));
   bool ok;
   ck_assert(json_as_bool(json_get(sc, "ok"), &ok));
   ck_assert(ok);
   ck_assert_str_eq(json_string(json_get(sc, "status")), "ok");
   ck_assert_int_eq(json_get(sc, "diagnostics")->array.count, 0);

   // File names that look like options are still files
   sc = structured(analyse(mp, srv, &sink, "{\"files\":[\"-dash.vhd\"]}"));
   ck_assert(json_as_bool(json_get(sc, "ok"), &ok));
   ck_assert(ok);

   // Errors are a normal result with positions
   sc = structured(analyse(mp, srv, &sink,
                           "{\"files\":[\"bad.vhd\"],\"std\":\"2008\"}"));
   ck_assert(json_as_bool(json_get(sc, "ok"), &ok));
   ck_assert(!ok);
   ck_assert_str_eq(json_string(json_get(sc, "status")), "errors");

   int64_t n;
   ck_assert(json_as_int(json_get(json_get(sc, "counts"), "error"), &n));
   ck_assert_int_eq(n, 2);

   const json_t *diags = json_get(sc, "diagnostics");
   ck_assert_int_eq(diags->array.count, 2);
   const json_t *d0 = json_at(diags, 0);
   ck_assert_str_eq(json_string(json_get(d0, "severity")), "error");
   ck_assert_str_eq(json_string(json_get(d0, "file")), "bad.vhd");
   ck_assert(json_as_int(json_get(d0, "line"), &n));
   ck_assert_int_eq(n, 3);
   ck_assert(json_as_int(json_get(d0, "column"), &n));
   ck_assert_int_eq(n, 25);
   ck_assert_str_eq(json_string(json_get(d0, "message")),
                    "no visible declaration for NOSUCH");

   // The diagnostic list can be truncated while the counts stay exact
   sc = structured(analyse(mp, srv, &sink,
                           "{\"files\":[\"bad.vhd\"],\"max_diagnostics\":1}"));
   ck_assert_int_eq(json_get(sc, "diagnostics")->array.count, 1);
   bool truncated;
   ck_assert(json_as_bool(json_get(sc, "diagnostics_truncated"), &truncated));
   ck_assert(truncated);
   ck_assert(json_as_int(json_get(json_get(sc, "counts"), "error"), &n));
   ck_assert_int_eq(n, 2);

   // A separate library
   sc = structured(analyse(mp, srv, &sink,
                           "{\"files\":[\"good.vhd\"],\"library\":\"mylib\"}"));
   ck_assert(json_as_bool(json_get(sc, "ok"), &ok));
   ck_assert(ok);

   char *libdir = xasprintf("%s/mylib", root);
   ck_assert_int_eq(access(libdir, F_OK), 0);
   free(libdir);

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
   free(nvc);
   free(lib);
}
END_TEST

START_TEST(test_analyse_bad_arguments)
{
   char *root = make_workspace();
   write_file(root, "a.vhd", "-- empty\n", 0644);
   char *link = xasprintf("%s/escape.vhd", root);
   ck_assert_int_eq(symlink("/etc/passwd", link), 0);
   free(link);
   char *dir = xasprintf("%s/subdir", root);
   ck_assert_int_eq(mkdir(dir, 0755), 0);
   free(dir);

   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, root, "/bin/true");

   const char *e;

   // Schema violations carry an example
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[]}"));
   ck_assert_ptr_nonnull(strstr(e, "arguments.files"));
   ck_assert_ptr_nonnull(strstr(e, "Example arguments for nvc_analyse"));

   e = tool_error(analyse(mp, srv, &sink, "{}"));
   ck_assert_ptr_nonnull(strstr(e, "missing required property 'files'"));

   e = tool_error(analyse(mp, srv, &sink,
                          "{\"files\":[\"a.vhd\"],\"std\":\"1987\"}"));
   ck_assert_ptr_nonnull(strstr(e, "must be one of"));

   e = tool_error(analyse(mp, srv, &sink,
                          "{\"files\":[\"a.vhd\"],\"timeout_ms\":5}"));
   ck_assert_ptr_nonnull(strstr(e, "at least 1000"));

   // Library names are restricted so they cannot become options or paths
   e = tool_error(analyse(mp, srv, &sink,
                          "{\"files\":[\"a.vhd\"],\"library\":\"../x\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid library name"));
   e = tool_error(analyse(mp, srv, &sink,
                          "{\"files\":[\"a.vhd\"],\"library\":\"a:b\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid library name"));

   // Paths must stay inside the workspace, even through symlinks
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"/etc/passwd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "outside the workspace root"));
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"../../etc/passwd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "outside the workspace root")
                         ?: strstr(e, "was not found"));
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"escape.vhd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "outside the workspace root"));

   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"missing.vhd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "was not found"));
   ck_assert_ptr_nonnull(strstr(e, "The root contains: "));
   ck_assert_ptr_nonnull(strstr(e, "a.vhd"));
   ck_assert_ptr_nonnull(strstr(e, "subdir/"));

   // A file with the right name in another directory is suggested
   write_file(root, "subdir/counter.vhd", "-- empty\n", 0644);
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"counter.vhd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "Did you mean: subdir/counter.vhd?"));
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"x/COUNTER.VHD\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "Did you mean: subdir/counter.vhd?"));
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"subdir\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "not a regular file"));

   // The workspace root itself is not a file
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\".\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "outside the workspace root"));

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
}
END_TEST

static mcp_server_t *fake_worker(sink_t *sink, const char *root,
                                 const char *script)
{
   write_file(root, "worker.sh", script, 0755);
   write_file(root, "a.vhd", "-- empty\n", 0644);

   char *exe = xasprintf("%s/worker.sh", root);
   mcp_server_t *srv = new_server(sink, root, exe);
   free(exe);
   return srv;
}

START_TEST(test_analyse_worker_failures)
{
   char *root = make_workspace();
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   const char *e;

   // A fatal error prints text and exits: the server keeps going
   mcp_server_t *srv = fake_worker(
      &sink, root, "#!/bin/sh\necho '** Fatal: library STD not found'\n"
      "echo more >&2\nexit 1\n");
   const json_t *sc =
      structured(analyse(mp, srv, &sink, "{\"files\":[\"a.vhd\"]}"));
   bool ok = true;
   ck_assert(json_as_bool(json_get(sc, "ok"), &ok));
   ck_assert(!ok);
   ck_assert_str_eq(json_string(json_get(sc, "status")), "failed");
   ck_assert_ptr_nonnull(strstr(json_string(json_get(sc, "worker_output")),
                                "Fatal: library STD not found"));
   ck_assert_ptr_nonnull(strstr(json_string(json_get(sc, "worker_output")),
                                "more"));

   // The next request is still served
   sc = structured(analyse(mp, srv, &sink, "{\"files\":[\"a.vhd\"]}"));
   mcp_server_free(srv);

   // A crash is reported as an error with the signal
   srv = fake_worker(&sink, root, "#!/bin/sh\necho partial\nkill -SEGV $$\n");
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"a.vhd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "terminated by SIGSEGV"));
   ck_assert_ptr_nonnull(strstr(e, "partial"));
   mcp_server_free(srv);

   // A hang is stopped at the timeout
   srv = fake_worker(&sink, root, "#!/bin/sh\nexec sleep 60\n");
   e = tool_error(analyse(mp, srv, &sink,
                          "{\"files\":[\"a.vhd\"],\"timeout_ms\":1000}"));
   ck_assert_ptr_nonnull(strstr(e, "did not finish within 1000 ms"));
   mcp_server_free(srv);

   // A missing executable is a start failure, not a crash
   write_file(root, "a.vhd", "-- empty\n", 0644);
   srv = new_server(&sink, root, "/nonexistent/nvc");
   e = tool_error(analyse(mp, srv, &sink, "{\"files\":[\"a.vhd\"]}"));
   ck_assert_ptr_nonnull(strstr(e, "could not be started"));
   mcp_server_free(srv);

   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
}
END_TEST

typedef struct {
   int         fd;
   const char *text;
   unsigned    delay_ms;
} writer_t;

static void *delayed_writer(void *arg)
{
   writer_t *w = arg;

   usleep(w->delay_ms * 1000);
   ck_assert_int_eq(write(w->fd, w->text, strlen(w->text)),
                    (ssize_t)strlen(w->text));
   close(w->fd);
   return NULL;
}

static const char sleep_request[] =
   "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":"
   "{\"name\":\"nvc_analyse\",\"arguments\":{\"files\":[\"a.vhd\"],"
   "\"timeout_ms\":300000}," MODERN_META "}}\n";

static const char ping_request[] =
   "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}\n";

START_TEST(test_cancel_running)
{
   char *root = make_workspace();
   sink_t sink = {};

   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   write_file(root, "worker.sh", "#!/bin/sh\nexec sleep 60\n", 0755);
   write_file(root, "a.vhd", "-- empty\n", 0644);

   char *exe = xasprintf("%s/worker.sh", root);
   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = exe,
      .root      = root,
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   // The request is already running when the cancellation arrives
   ck_assert_int_eq(write(fds[1], sleep_request, strlen(sleep_request)),
                    (ssize_t)strlen(sleep_request));

   static const char later[] =
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
      "\"params\":{\"requestId\":7}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}\n";

   writer_t w = { fds[1], later, 500 };
   pthread_t thread;
   ck_assert_int_eq(pthread_create(&thread, NULL, delayed_writer, &w), 0);

   const uint64_t start = get_timestamp_us();
   mcp_server_run(srv);
   const uint64_t elapsed_ms = (get_timestamp_us() - start) / 1000;

   pthread_join(thread, NULL);

   // The cancelled request gets no response and the worker was killed
   // long before its sixty second sleep ended
   ck_assert_int_lt(elapsed_ms, 20000);
   ck_assert_int_eq(sink.count, 1);

   mem_pool_t *mp = pool_new();
   json_t *r = parse_ok(mp, sink.lines[0]);
   int64_t id;
   ck_assert(json_as_int(json_get(r, "id"), &id));
   ck_assert_int_eq(id, 8);
   ck_assert_ptr_nonnull(json_get(r, "result"));

   pool_free(mp);
   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
   free(exe);
   remove_tree(root);
   free(root);
}
END_TEST

START_TEST(test_cancel_queued)
{
   char *root = make_workspace();
   sink_t sink = {};

   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   write_file(root, "worker.sh", "#!/bin/sh\nexec sleep 60\n", 0755);
   write_file(root, "a.vhd", "-- empty\n", 0644);

   char *exe = xasprintf("%s/worker.sh", root);
   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = exe,
      .root      = root,
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   // Cancel arrives while the request is still queued behind others
   static const char batch[] =
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
      "\"params\":{\"requestId\":7}}\n";

   FILE *f = fdopen(fds[1], "w");
   fputs(sleep_request, f);
   fputs(batch, f);
   fputs(ping_request, f);
   fclose(f);

   mcp_server_run(srv);

   ck_assert_int_eq(sink.count, 1);
   mem_pool_t *mp = pool_new();
   json_t *r = parse_ok(mp, sink.lines[0]);
   int64_t id;
   ck_assert(json_as_int(json_get(r, "id"), &id));
   ck_assert_int_eq(id, 8);

   pool_free(mp);
   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
   free(exe);
   remove_tree(root);
   free(root);
}
END_TEST

START_TEST(test_input_framing)
{
   sink_t sink = {};

   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = "/bin/true",
      .root      = "/tmp",
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   // CRLF line endings, blank lines, and a last message without a newline
   FILE *f = fdopen(fds[1], "w");
   fputs("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n\n", f);
   fputs("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n", f);
   fputs("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}", f);
   fclose(f);

   mcp_server_run(srv);

   ck_assert_int_eq(sink.count, 3);
   for (size_t i = 0; i < sink.count; i++)
      ck_assert_ptr_nonnull(strstr(sink.lines[i], "\"result\""));

   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
}
END_TEST

START_TEST(test_input_too_large)
{
   sink_t sink = {};

   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = "/bin/true",
      .root      = "/tmp",
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   // The writer runs in a thread because the pipe would fill up
   const size_t big = 2 * 1024 * 1024;
   char *text = xmalloc(big + 128);
   memset(text, 'x', big);
   strcpy(text + big, "\n{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n");

   writer_t w = { fds[1], text, 0 };
   pthread_t thread;
   ck_assert_int_eq(pthread_create(&thread, NULL, delayed_writer, &w), 0);

   mcp_server_run(srv);
   pthread_join(thread, NULL);

   // One error for the oversized line then the next message is served
   ck_assert_int_eq(sink.count, 2);
   ck_assert_ptr_nonnull(strstr(sink.lines[0], "too large"));
   ck_assert_ptr_nonnull(strstr(sink.lines[1], "\"id\":9"));

   free(text);
   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
}
END_TEST


////////////////////////////////////////////////////////////////////////////////
// Listing, elaboration and live simulation

static const char counter_src[] =
   "library ieee;\n"
   "use ieee.std_logic_1164.all;\n"
   "use ieee.numeric_std.all;\n"
   "entity counter is\n"
   "  port (clk, rst : in std_logic; q : out unsigned(3 downto 0));\n"
   "end entity;\n"
   "architecture rtl of counter is\n"
   "  signal cnt : unsigned(3 downto 0);\n"
   "begin\n"
   "  process (clk) begin\n"
   "    if rising_edge(clk) then\n"
   "      if rst = '1' then cnt <= (others => '0');\n"
   "      else cnt <= cnt + 1; end if;\n"
   "    end if;\n"
   "  end process;\n"
   "  q <= cnt;\n"
   "end architecture;\n";

// A free running clock, a report at the start and an error when the
// counter reaches five
static const char tb_src[] =
   "library ieee;\n"
   "use ieee.std_logic_1164.all;\n"
   "use ieee.numeric_std.all;\n"
   "use std.textio.all;\n"
   "entity tb is end entity;\n"
   "architecture test of tb is\n"
   "  signal clk : std_logic := '0';\n"
   "  signal rst : std_logic := '1';\n"
   "  signal q   : unsigned(3 downto 0);\n"
   "begin\n"
   "  dut : entity work.counter port map (clk => clk, rst => rst, q => q);\n"
   "  clk <= not clk after 5 ns;\n"
   "  process begin\n"
   "    report \"testbench starting\" severity note;\n"
   "    wait for 12 ns; rst <= '0'; wait;\n"
   "  end process;\n"
   "  process (clk)\n"
   "    variable l : line;\n"
   "  begin\n"
   "    if rising_edge(clk) and rst = '0' then\n"
   "      write(l, string'(\"tick q=\") & integer'image(to_integer(q)));\n"
   "      writeline(output, l);\n"
   "      assert q /= 5 report \"q reached five\" severity error;\n"
   "    end if;\n"
   "  end process;\n"
   "end architecture;\n";

// Finishes by itself: the clock stops after 50 ns
static const char finite_src[] =
   "entity finite is end entity;\n"
   "architecture a of finite is\n"
   "  signal done : boolean := false;\n"
   "  signal clk : bit := '0';\n"
   "begin\n"
   "  clk <= not clk after 5 ns when not done;\n"
   "  process begin wait for 50 ns; done <= true; wait; end process;\n"
   "end architecture;\n";

// Ends the simulation with a failure at 30 ns
static const char fail_src[] =
   "entity failing is end entity;\n"
   "architecture a of failing is\n"
   "  signal x : integer := 0;\n"
   "begin\n"
   "  process begin\n"
   "    wait for 10 ns; x <= 7;\n"
   "    wait for 20 ns;\n"
   "    assert false report \"giving up\" severity failure;\n"
   "    wait;\n"
   "  end process;\n"
   "end architecture;\n";

// More messages than the result can hold, all at time zero
static const char chatty_src[] =
   "entity chatty is end entity;\n"
   "architecture a of chatty is\n"
   "begin\n"
   "  process begin\n"
   "    for i in 1 to 300 loop report \"message\" severity note; end loop;\n"
   "    wait;\n"
   "  end process;\n"
   "end architecture;\n";

// A process that never waits
static const char hang_src[] =
   "entity hang is end entity;\n"
   "architecture a of hang is\n"
   "  signal s : integer := 0;\n"
   "begin\n"
   "  process begin\n"
   "    wait for 10 ns;\n"
   "    loop s <= s + 1; end loop;\n"
   "  end process;\n"
   "end architecture;\n";

static json_t *tool(mem_pool_t *mp, mcp_server_t *srv, sink_t *sink,
                    const char *name, const char *args)
{
   char *req = xasprintf("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                         "\"tools/call\",\"params\":{\"name\":\"%s\","
                         "\"arguments\":%s," MODERN_META "}}", name, args);
   json_t *r = call(mp, srv, sink, req);
   free(req);
   return json_get(r, "result");
}

static int64_t int_field(const json_t *obj, const char *key)
{
   int64_t v;
   ck_assert_msg(json_as_int(json_get(obj, key), &v), "no integer '%s'", key);
   return v;
}

static const char *str_field(const json_t *obj, const char *key)
{
   const char *v = json_string(json_get(obj, key));
   ck_assert_msg(v != NULL, "no string '%s'", key);
   return v;
}

static bool bool_field(const json_t *obj, const char *key)
{
   bool v = false;
   ck_assert_msg(json_as_bool(json_get(obj, key), &v), "no boolean '%s'", key);
   return v;
}

typedef struct {
   char         *root;
   char         *nvc;
   mem_pool_t   *mp;
   sink_t        sink;
   mcp_server_t *srv;
} fixture_t;

// A server over a fresh workspace with the real nvc, the counter and the
// given extra source files analysed and elaborated
static void fixture_open(fixture_t *f, const char *const *names,
                         const char *const *sources, size_t nextra,
                         const char *const *tops, size_t ntops)
{
   memset(f, 0, sizeof(*f));

   f->root = make_workspace();
   f->nvc = build_path("bin/nvc");

   char *lib = build_path("lib");
   setenv("NVC_LIBPATH", lib, 1);
   free(lib);

   write_file(f->root, "counter.vhd", counter_src, 0644);

   LOCAL_TEXT_BUF files = tb_new();
   tb_cat(files, "\"counter.vhd\"");
   for (size_t i = 0; i < nextra; i++) {
      write_file(f->root, names[i], sources[i], 0644);
      tb_printf(files, ",\"%s\"", names[i]);
   }

   f->mp = pool_new();
   f->srv = new_server(&f->sink, f->root, f->nvc);

   char *args = xasprintf("{\"files\":[%s],\"std\":\"2008\"}", tb_get(files));
   const json_t *sc = structured(tool(f->mp, f->srv, &f->sink, "nvc_analyse",
                                      args));
   free(args);
   ck_assert_msg(bool_field(sc, "ok"), "analysis failed: %s",
                 f->sink.lines[0]);

   for (size_t i = 0; i < ntops; i++) {
      args = xasprintf("{\"top\":\"%s\",\"std\":\"2008\"}", tops[i]);
      sc = structured(tool(f->mp, f->srv, &f->sink, "nvc_elaborate", args));
      free(args);
      ck_assert_msg(bool_field(sc, "ok"), "elaboration of %s failed: %s",
                    tops[i], f->sink.lines[0]);
   }
}

static void fixture_close(fixture_t *f)
{
   mcp_server_free(f->srv);
   sink_clear(&f->sink);
   pool_free(f->mp);
   remove_tree(f->root);
   free(f->root);
   free(f->nvc);
}

static const char *open_sim(fixture_t *f, const char *top, char *id)
{
   char *args = xasprintf("{\"top\":\"%s\",\"std\":\"2008\"}", top);
   const json_t *sc = structured(tool(f->mp, f->srv, &f->sink, "nvc_sim_open",
                                      args));
   free(args);

   snprintf(id, 40, "%s", str_field(sc, "sim_id"));
   ck_assert_int_eq(strlen(id), 32);
   return id;
}

static const json_t *sim_tool(fixture_t *f, const char *name, const char *id,
                              const char *extra)
{
   char *args = xasprintf("{\"sim_id\":\"%s\"%s%s}", id,
                          extra[0] ? "," : "", extra);
   const json_t *r = tool(f->mp, f->srv, &f->sink, name, args);
   free(args);
   return r;
}

START_TEST(test_workspace_list)
{
   char *root = make_workspace();
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, root, "/bin/true");

   write_file(root, "top.vhd", "-- vhdl\n", 0644);
   write_file(root, "notes.txt", "text\n", 0644);
   write_file(root, ".hidden.vhd", "-- vhdl\n", 0644);

   char *dir = xasprintf("%s/rtl", root);
   ck_assert_int_eq(mkdir(dir, 0755), 0);
   free(dir);
   write_file(root, "rtl/alu.sv", "module alu; endmodule\n", 0644);
   write_file(root, "rtl/CORE_TB.VHDL", "-- vhdl\n", 0644);
   write_file(root, "rtl/old.v", "module old; endmodule\n", 0644);

   // A library directory is reported but not entered
   dir = xasprintf("%s/work", root);
   ck_assert_int_eq(mkdir(dir, 0755), 0);
   free(dir);
   write_file(root, "work/_NVC_LIB", "", 0644);
   write_file(root, "work/leftover.vhd", "-- vhdl\n", 0644);

   char *link = xasprintf("%s/link.vhd", root);
   ck_assert_int_eq(symlink("top.vhd", link), 0);
   free(link);

   const json_t *sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                                      "{}"));
   const json_t *entries = json_get(sc, "entries");
   ck_assert_int_eq(int_field(sc, "total"), 5);
   ck_assert(!bool_field(sc, "truncated"));
   ck_assert_str_eq(str_field(sc, "path"), ".");

   // Sorted by path; hidden files, links, text and library contents skipped
   static const char *const expect[] = {
      "rtl/CORE_TB.VHDL", "rtl/alu.sv", "rtl/old.v", "top.vhd", "work"
   };
   for (size_t i = 0; i < ARRAY_LEN(expect); i++)
      ck_assert_str_eq(str_field(json_at(entries, i), "path"), expect[i]);

   ck_assert_str_eq(str_field(json_at(entries, 0), "language"), "vhdl");
   ck_assert_str_eq(str_field(json_at(entries, 1), "language"), "systemverilog");
   ck_assert_str_eq(str_field(json_at(entries, 2), "language"), "verilog");
   ck_assert_str_eq(str_field(json_at(entries, 4), "type"), "library");
   ck_assert_int_eq(int_field(json_at(entries, 3), "size_bytes"), 8);

   // Everything, then a glob, then a subdirectory
   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"kind\":\"all\"}"));
   ck_assert_int_eq(int_field(sc, "total"), 6);

   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"pattern\":\"*_tb.vhdl\"}"));
   ck_assert_int_eq(int_field(sc, "total"), 1);
   ck_assert_str_eq(str_field(json_at(json_get(sc, "entries"), 0), "path"),
                    "rtl/CORE_TB.VHDL");

   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"path\":\"rtl\"}"));
   ck_assert_str_eq(str_field(sc, "path"), "rtl");
   ck_assert_int_eq(int_field(sc, "total"), 3);

   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"max_depth\":0}"));
   ck_assert_int_eq(int_field(sc, "total"), 2);   // top.vhd and work

   // Paging
   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"limit\":2}"));
   ck_assert_int_eq(json_get(sc, "entries")->array.count, 2);
   ck_assert(bool_field(sc, "truncated"));
   ck_assert_str_eq(str_field(sc, "next_cursor"), "2");

   sc = structured(tool(mp, srv, &sink, "nvc_workspace_list",
                        "{\"limit\":2,\"cursor\":\"4\"}"));
   ck_assert_int_eq(json_get(sc, "entries")->array.count, 1);
   ck_assert(!bool_field(sc, "truncated"));

   // Misuse is explained
   const char *e = tool_error(tool(mp, srv, &sink, "nvc_workspace_list",
                                   "{\"path\":\"/etc\"}"));
   ck_assert_ptr_nonnull(strstr(e, "outside the workspace root"));
   e = tool_error(tool(mp, srv, &sink, "nvc_workspace_list",
                       "{\"path\":\"top.vhd\"}"));
   ck_assert_ptr_nonnull(strstr(e, "is not a directory"));
   e = tool_error(tool(mp, srv, &sink, "nvc_workspace_list",
                       "{\"max_depth\":99}"));
   ck_assert_ptr_nonnull(strstr(e, "at most 8"));

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
}
END_TEST

START_TEST(test_library_and_elaborate)
{
   char *root = make_workspace();
   char *nvc = build_path("bin/nvc");
   char *lib = build_path("lib");
   setenv("NVC_LIBPATH", lib, 1);
   free(lib);

   write_file(root, "counter.vhd", counter_src, 0644);
   write_file(root, "tb.vhd", tb_src, 0644);

   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   mcp_server_t *srv = new_server(&sink, root, nvc);

   // No library until something has been analysed, and asking does not
   // create one
   const char *e = tool_error(tool(mp, srv, &sink, "nvc_library_list", "{}"));
   ck_assert_ptr_nonnull(strstr(e, "no library 'work'"));
   ck_assert_ptr_nonnull(strstr(e, "none"));
   char *workdir = xasprintf("%s/work", root);
   ck_assert_int_ne(access(workdir, F_OK), 0);
   free(workdir);

   e = tool_error(tool(mp, srv, &sink, "nvc_library_list",
                       "{\"library\":\"../x\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid library name"));

   const json_t *sc = structured(tool(
      mp, srv, &sink, "nvc_analyse",
      "{\"files\":[\"counter.vhd\",\"tb.vhd\"],\"std\":\"2008\"}"));
   ck_assert(bool_field(sc, "ok"));

   sc = structured(tool(mp, srv, &sink, "nvc_library_list", "{}"));
   ck_assert_str_eq(str_field(sc, "library"), "work");
   ck_assert_int_eq(int_field(sc, "total"), 4);
   ck_assert_int_eq(json_get(sc, "elaborated")->array.count, 0);

   const json_t *units = json_get(sc, "units");
   ck_assert_str_eq(str_field(json_at(units, 0), "name"), "WORK.COUNTER");
   ck_assert_str_eq(str_field(json_at(units, 0), "kind"), "entity");
   ck_assert_str_eq(str_field(json_at(units, 1), "kind"), "architecture");
   ck_assert_str_eq(str_field(json_at(units, 1), "entity"), "COUNTER");
   ck_assert_str_eq(str_field(json_at(units, 1), "architecture"), "RTL");

   sc = structured(tool(mp, srv, &sink, "nvc_elaborate",
                        "{\"top\":\"tb\",\"std\":\"2008\"}"));
   ck_assert(bool_field(sc, "ok"));
   ck_assert_str_eq(str_field(sc, "status"), "ok");

   sc = structured(tool(mp, srv, &sink, "nvc_library_list", "{}"));
   const json_t *tops = json_get(sc, "elaborated");
   ck_assert_int_eq(tops->array.count, 1);
   ck_assert_str_eq(json_string(json_at(tops, 0)), "tb");
   ck_assert_int_eq(int_field(sc, "total"), 4);

   // An unknown unit explains what to do
   sc = structured(tool(mp, srv, &sink, "nvc_elaborate",
                        "{\"top\":\"nosuch\",\"std\":\"2008\"}"));
   ck_assert(!bool_field(sc, "ok"));
   ck_assert_str_eq(str_field(sc, "status"), "failed");
   ck_assert_ptr_nonnull(strstr(str_field(sc, "worker_output"),
                                "cannot find unit"));
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), "nvc_analyse"));

   // Generics
   sc = structured(tool(mp, srv, &sink, "nvc_elaborate",
                        "{\"top\":\"tb\",\"std\":\"2008\","
                        "\"generics\":{\"UNUSED\":3,\"FLAG\":true}}"));
   ck_assert(json_get(sc, "ok") != NULL);

   e = tool_error(tool(mp, srv, &sink, "nvc_elaborate",
                       "{\"top\":\"tb\",\"generics\":{\"BAD NAME\":1}}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid generic name"));
   e = tool_error(tool(mp, srv, &sink, "nvc_elaborate",
                       "{\"top\":\"tb\",\"generics\":{\"N\":null}}"));
   ck_assert_ptr_nonnull(strstr(e, "must be a string, number or boolean"));
   e = tool_error(tool(mp, srv, &sink, "nvc_elaborate",
                       "{\"top\":\"tb\",\"generics\":{\"N\":\"a\\nb\"}}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid value for generic"));
   e = tool_error(tool(mp, srv, &sink, "nvc_elaborate",
                       "{\"top\":\"tb\",\"generics\":[1]}"));
   ck_assert_ptr_nonnull(strstr(e, "expected object"));
   e = tool_error(tool(mp, srv, &sink, "nvc_elaborate",
                       "{\"top\":\"bad name!\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid design unit name"));

   mcp_server_free(srv);
   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
   free(nvc);
}
END_TEST

START_TEST(test_sim_lifecycle)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // The open result describes the design
   const json_t *sc = structured(sim_tool(&f, "nvc_design_list", id, ""));
   ck_assert_str_eq(str_field(sc, "path"), "/");
   ck_assert_int_eq(int_field(sc, "total"), 4);
   const json_t *items = json_get(sc, "items");
   ck_assert_str_eq(str_field(json_at(items, 0), "path"), "/clk");
   ck_assert_str_eq(str_field(json_at(items, 0), "type"), "STD_LOGIC");
   ck_assert_str_eq(str_field(json_at(items, 2), "path"), "/q");
   ck_assert_int_eq(int_field(json_at(items, 2), "width"), 4);
   ck_assert_str_eq(str_field(json_at(items, 3), "path"), "/dut");
   ck_assert_str_eq(str_field(json_at(items, 3), "kind"), "region");

   sc = structured(sim_tool(&f, "nvc_design_list", id, "\"path\":\"/dut\""));
   ck_assert_int_eq(int_field(sc, "total"), 4);

   // Search: by leaf, by full path, case insensitive, paged
   sc = structured(sim_tool(&f, "nvc_design_search", id, "\"pattern\":\"cnt\""));
   ck_assert_int_eq(int_field(sc, "total"), 1);
   ck_assert_str_eq(str_field(json_at(json_get(sc, "items"), 0), "path"),
                    "/dut/cnt");

   sc = structured(sim_tool(&f, "nvc_design_search", id,
                            "\"pattern\":\"/dut/*\",\"kind\":\"signal\""));
   ck_assert_int_eq(int_field(sc, "total"), 4);

   sc = structured(sim_tool(&f, "nvc_design_search", id, "\"pattern\":\"CLK\""));
   ck_assert_int_eq(int_field(sc, "total"), 2);

   // Globs: * may match nothing, ? matches one character
   static const char *const globs[] = {
      "cnt*", "*cnt*", "cn?", "CNT*", "/dut/c?t", "*/cnt"
   };
   for (size_t i = 0; i < ARRAY_LEN(globs); i++) {
      char *extra = xasprintf("\"pattern\":\"%s\"", globs[i]);
      sc = structured(sim_tool(&f, "nvc_design_search", id, extra));
      free(extra);
      ck_assert_msg(int_field(sc, "total") == 1, "pattern %s matched %"PRIi64,
                    globs[i], int_field(sc, "total"));
   }

   sc = structured(sim_tool(&f, "nvc_design_search", id,
                            "\"pattern\":\"clk\",\"limit\":1"));
   ck_assert(bool_field(sc, "truncated"));
   ck_assert_str_eq(str_field(sc, "next_cursor"), "1");
   sc = structured(sim_tool(&f, "nvc_design_search", id,
                            "\"pattern\":\"clk\",\"limit\":1,\"cursor\":\"1\""));
   ck_assert(!bool_field(sc, "truncated"));
   ck_assert_str_eq(str_field(json_at(json_get(sc, "items"), 0), "path"),
                    "/dut/clk");

   // The first 20 ns: the testbench reports, the design prints
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"20 ns\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");
   ck_assert_int_eq(int_field(sc, "now_fs"), 20000000);
   ck_assert_str_eq(str_field(sc, "now"), "20 ns");
   const json_t *msg = json_at(json_get(sc, "messages"), 0);
   ck_assert_ptr_nonnull(strstr(str_field(msg, "message"), "testbench starting"));
   ck_assert_str_eq(str_field(msg, "severity"), "note");
   ck_assert_int_eq(int_field(msg, "time_fs"), 0);
   ck_assert_str_eq(str_field(sc, "sim_output"), "tick q=0\n");

   sc = structured(sim_tool(&f, "nvc_signal_get", id,
                            "\"paths\":[\"/q\",\"/dut/cnt\",\"/clk\"]"));
   const json_t *values = json_get(sc, "values");
   ck_assert_str_eq(str_field(json_at(values, 0), "value"), "0001");
   ck_assert_str_eq(str_field(json_at(values, 1), "value"), "0001");
   ck_assert_str_eq(str_field(json_at(values, 2), "value"), "'0'");
   ck_assert_str_eq(str_field(json_at(values, 0), "type"), "UNSIGNED");
   ck_assert(!bool_field(json_at(values, 0), "forced"));
   ck_assert_int_eq(int_field(sc, "now_fs"), 20000000);

   // The default pause on an error stops exactly where it happened
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"200 ns\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "severity");
   ck_assert_int_eq(int_field(sc, "now_fs"), 65000000);
   msg = json_at(json_get(sc, "messages"), 0);
   ck_assert_str_eq(str_field(msg, "severity"), "error");
   ck_assert_ptr_nonnull(strstr(str_field(msg, "message"), "q reached five"));
   ck_assert_int_eq(int_field(msg, "time_fs"), 65000000);
   ck_assert_int_eq(int_field(json_get(sc, "counts"), "error"), 1);

   sc = structured(sim_tool(&f, "nvc_signal_get", id,
                            "\"paths\":[\"/q\"],\"radix\":\"hex\""));
   ck_assert_str_eq(str_field(json_at(json_get(sc, "values"), 0), "value"),
                    "0x5");
   sc = structured(sim_tool(&f, "nvc_signal_get", id,
                            "\"paths\":[\"/q\"],\"radix\":\"dec\""));
   ck_assert_str_eq(str_field(json_at(json_get(sc, "values"), 0), "value"), "5");

   // Continuing counts the duration from where the run stopped, not
   // from the target of the interrupted one
   sc = structured(sim_tool(&f, "nvc_sim_run", id,
                            "\"duration\":\"30 ns\",\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");
   ck_assert_int_eq(int_field(sc, "now_fs"), 95000000);

   // Forcing takes effect at the next cycle and shows as forced
   sc = structured(sim_tool(&f, "nvc_signal_set", id,
                            "\"path\":\"/rst\",\"value\":\"'1'\""));
   ck_assert_str_eq(str_field(sc, "mode"), "force");
   sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"20 ns\",\"pause_on\":\"never\"");

   sc = structured(sim_tool(&f, "nvc_signal_get", id,
                            "\"paths\":[\"/q\",\"/rst\"]"));
   values = json_get(sc, "values");
   ck_assert_str_eq(str_field(json_at(values, 0), "value"), "0000");
   ck_assert(bool_field(json_at(values, 1), "forced"));

   sc = structured(sim_tool(&f, "nvc_signal_set", id,
                            "\"path\":\"/rst\",\"mode\":\"release\""));
   ck_assert_int_eq(int_field(sc, "released"), 1);

   // Forcing a vector needs the right number of elements
   const char *e = tool_error(sim_tool(&f, "nvc_signal_set", id,
                                       "\"path\":\"/q\",\"value\":\"101\""));
   ck_assert_ptr_nonnull(strstr(e, "has 4 elements"));
   e = tool_error(sim_tool(&f, "nvc_signal_set", id,
                           "\"path\":\"/rst\",\"value\":\"banana\""));
   ck_assert_ptr_nonnull(strstr(e, "is not valid for type"));
   e = tool_error(sim_tool(&f, "nvc_signal_set", id,
                           "\"path\":\"/rst\",\"mode\":\"release\""));
   ck_assert_ptr_nonnull(strstr(e, "is not forced"));
   e = tool_error(sim_tool(&f, "nvc_signal_set", id, "\"path\":\"/rst\""));
   ck_assert_ptr_nonnull(strstr(e, "needs a value"));

   // Mistakes say how to recover
   e = tool_error(sim_tool(&f, "nvc_signal_get", id, "\"paths\":[\"/clok\"]"));
   ck_assert_ptr_nonnull(strstr(e, "nvc_design_search"));
   e = tool_error(sim_tool(&f, "nvc_signal_get", id, "\"paths\":[\"/dut\"]"));
   ck_assert_ptr_nonnull(strstr(e, "is a region"));
   e = tool_error(sim_tool(&f, "nvc_design_list", id, "\"path\":\"/nodut\""));
   ck_assert_ptr_nonnull(strstr(e, "was not found"));
   e = tool_error(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"5 parsecs\""));
   ck_assert_ptr_nonnull(strstr(e, "Invalid duration"));
   e = tool_error(sim_tool(&f, "nvc_sim_run", id,
                           "\"duration\":\"5 ns\",\"until\":\"1 us\""));
   ck_assert_ptr_nonnull(strstr(e, "not both"));

   // Closing releases the handle
   sc = structured(sim_tool(&f, "nvc_sim_close", id, ""));
   ck_assert(bool_field(sc, "closed"));
   ck_assert_int_eq(int_field(sc, "open_simulations"), 0);

   e = tool_error(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"5 ns\""));
   ck_assert_ptr_nonnull(strstr(e, "is not open"));
   e = tool_error(sim_tool(&f, "nvc_sim_close", id, ""));
   ck_assert_ptr_nonnull(strstr(e, "is not open"));

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_run_modes)
{
   fixture_t f;
   const char *names[] = { "tb.vhd", "finite.vhd", "failing.vhd", "chatty.vhd" };
   const char *sources[] = { tb_src, finite_src, fail_src, chatty_src };
   const char *tops[] = { "tb", "finite", "failing", "chatty" };
   fixture_open(&f, names, sources, 4, tops, 4);

   char id[40];
   const json_t *sc;

   // A design with nothing left to do finishes and stays finished
   open_sim(&f, "finite", id);
   sc = structured(sim_tool(&f, "nvc_sim_run", id, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "finished");
   ck_assert_int_ge(int_field(sc, "now_fs"), 50000000);
   ck_assert_int_eq(json_get(sc, "next_event_fs")->kind, JSON_NULL);
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), "has finished"));
   const int64_t end_time = int_field(sc, "now_fs");
   sc = structured(sim_tool(&f, "nvc_sim_run", id, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "finished");
   ck_assert_int_eq(int_field(sc, "now_fs"), end_time);
   sim_tool(&f, "nvc_sim_close", id, "");

   // A run is bounded by real time and can be continued
   open_sim(&f, "tb", id);
   sc = structured(sim_tool(&f, "nvc_sim_run", id,
                            "\"max_wall_ms\":300,\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "wall_budget");
   ck_assert_int_ge(int_field(sc, "wall_ms"), 250);
   // The testbench reports an error every few cycles: the hint points at
   // pause_on rather than just saying to call again
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), "real time limit"));
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), "pause_on error"));
   ck_assert_int_gt(int_field(sc, "now_fs"), 65000000);
   const int64_t reached = int_field(sc, "now_fs");
   ck_assert_int_gt(int_field(json_get(sc, "counts"), "error"), 0);

   sc = structured(sim_tool(&f, "nvc_sim_run", id,
                            "\"duration\":\"10 ns\",\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");
   ck_assert_int_gt(int_field(sc, "now_fs"), reached);
   sim_tool(&f, "nvc_sim_close", id, "");

   // A failure ends the simulation but the signals stay readable
   open_sim(&f, "failing", id);
   sc = structured(sim_tool(&f, "nvc_sim_run", id, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "terminated");
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), "cannot continue"));
   ck_assert_int_ne(int_field(sc, "exit_status"), 0);
   ck_assert_int_eq(int_field(sc, "now_fs"), 30000000);
   ck_assert_int_ge(int_field(json_get(sc, "counts"), "failure"), 1);
   const json_t *last = json_at(json_get(sc, "messages"),
                                json_get(sc, "messages")->array.count - 1);
   ck_assert_ptr_nonnull(strstr(str_field(last, "message"), "giving up"));

   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"10 ns\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "terminated");
   sc = structured(sim_tool(&f, "nvc_signal_get", id, "\"paths\":[\"/x\"]"));
   ck_assert_str_eq(str_field(json_at(json_get(sc, "values"), 0), "value"), "7");
   sim_tool(&f, "nvc_sim_close", id, "");

   // Too many messages are counted but not all returned
   open_sim(&f, "chatty", id);
   sc = structured(sim_tool(&f, "nvc_sim_run", id, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "finished");
   ck_assert_int_eq(json_get(sc, "messages")->array.count, 100);
   ck_assert(bool_field(sc, "messages_truncated"));
   ck_assert_int_eq(int_field(json_get(sc, "counts"), "note"), 300);
   sim_tool(&f, "nvc_sim_close", id, "");

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_trace)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // Mistakes first
   const char *e = tool_error(sim_tool(&f, "nvc_wave_start", id,
                                       "\"include\":[\"/nope\"]"));
   ck_assert_ptr_nonnull(strstr(e, "nvc_design_search"));
   e = tool_error(sim_tool(&f, "nvc_wave_start", id,
                           "\"include\":[\"/dut\"]"));
   ck_assert_ptr_nonnull(strstr(e, "is a region"));
   e = tool_error(sim_tool(&f, "nvc_wave_start", id,
                           "\"include\":[\"zzz*\"]"));
   ck_assert_ptr_nonnull(strstr(e, "No signal matches"));
   e = tool_error(sim_tool(&f, "nvc_wave_get", id, "\"paths\":[\"/q\"]"));
   ck_assert_ptr_nonnull(strstr(e, "Nothing is recorded in memory"));

   const json_t *sc = structured(sim_tool(&f, "nvc_wave_start", id,
                                          "\"include\":[\"/rst\",\"/q\"],"
                                          "\"to\":\"memory\""));
   ck_assert_int_eq(int_field(sc, "signals"), 2);

   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"200 ns\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "severity");
   ck_assert_int_eq(int_field(sc, "now_fs"), 65000000);

   // The window before the failure, without running again
   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/q\"],\"from\":\"40 ns\","
                            "\"until\":\"70 ns\",\"radix\":\"binary\""));
   ck_assert_str_eq(str_field(sc, "source"), "memory");
   ck_assert_int_eq(int_field(sc, "changes_total"), 2);
   ck_assert(!bool_field(sc, "truncated"));
   ck_assert_int_eq(int_field(sc, "dropped"), 0);
   ck_assert_int_eq(int_field(sc, "now_fs"), 65000000);
   const json_t *sig = json_at(json_get(sc, "signals"), 0);
   ck_assert_str_eq(str_field(sig, "value_at_from"), "0011");
   const json_t *changes = json_get(sig, "changes");
   ck_assert_int_eq(json_at(json_at(changes, 0), 0)->integer, 45000000);
   ck_assert_str_eq(json_string(json_at(json_at(changes, 0), 1)), "0100");
   ck_assert_int_eq(json_at(json_at(changes, 1), 0)->integer, 55000000);
   ck_assert_str_eq(json_string(json_at(json_at(changes, 1), 1)), "0101");

   // The first entry holds the values when the recording started
   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/rst\",\"/q\"],"
                            "\"until\":\"0 fs\""));
   ck_assert_int_eq(int_field(sc, "oldest_time_fs"), 0);
   ck_assert_str_eq(str_field(json_at(json_get(sc, "signals"), 0),
                              "value_at_from"), "'1'");
   ck_assert_str_eq(str_field(json_at(json_get(sc, "signals"), 1),
                              "value_at_from"), "UUUU");

   // Paged by max_changes, and in decimal
   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/q\"],\"radix\":\"decimal\","
                            "\"max_changes\":3"));
   ck_assert_int_eq(int_field(sc, "changes_total"), 7);
   ck_assert(bool_field(sc, "truncated"));
   ck_assert_int_eq(json_get(json_at(json_get(sc, "signals"), 0),
                             "changes")->array.count, 3);

   e = tool_error(sim_tool(&f, "nvc_wave_get", id, "\"paths\":[\"/clk\"]"));
   ck_assert_ptr_nonnull(strstr(e, "not in the in-memory recording"));
   ck_assert_ptr_nonnull(strstr(e, "/rst, /q"));

   // Stopping ends the recording but what was kept stays readable
   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   const json_t *mem = json_get(json_get(sc, "files"), "memory");
   ck_assert(!bool_field(mem, "active"));
   ck_assert_int_eq(int_field(mem, "changes_kept"), 9);

   sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"40 ns\","
            "\"pause_on\":\"never\"");
   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/q\"],\"from\":\"70 ns\""));
   ck_assert_int_eq(int_field(sc, "changes_total"), 0);

   sim_tool(&f, "nvc_sim_close", id, "");
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_trace_overflow)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // A small buffer keeps only the most recent changes
   const json_t *sc = structured(sim_tool(&f, "nvc_wave_start", id,
                                          "\"include\":[\"/clk\"],"
                                          "\"to\":\"memory\","
                                          "\"max_changes\":100"));
   sc = json_get(json_get(sc, "files"), "memory");
   ck_assert_int_eq(int_field(sc, "capacity"), 100);

   sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"2 us\","
            "\"pause_on\":\"never\"");

   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/clk\"],\"max_changes\":5000"));
   ck_assert_int_eq(int_field(sc, "changes_total"), 100);
   ck_assert_int_gt(int_field(sc, "dropped"), 100);
   ck_assert_int_gt(int_field(sc, "oldest_time_fs"), 0);

   // Changes are in time order
   const json_t *changes = json_get(json_at(json_get(sc, "signals"), 0),
                                    "changes");
   int64_t previous = -1;
   for (size_t i = 0; i < changes->array.count; i++) {
      const int64_t t = json_at(json_at(changes, i), 0)->integer;
      ck_assert_int_gt(t, previous);
      previous = t;
   }

   sim_tool(&f, "nvc_sim_close", id, "");
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_hang)
{
   fixture_t f;
   const char *names[] = { "hang.vhd" }, *sources[] = { hang_src };
   const char *tops[] = { "hang" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "hang", id);

   // The process never waits so the worker cannot give control back: it
   // is stopped after a grace period and the handle is gone
   const char *e = tool_error(sim_tool(&f, "nvc_sim_run", id,
                                       "\"max_wall_ms\":200"));
   ck_assert_ptr_nonnull(strstr(e, "did not respond within 200 ms"));
   ck_assert_ptr_nonnull(strstr(e, "wait statement"));

   e = tool_error(sim_tool(&f, "nvc_signal_get", id, "\"paths\":[\"/s\"]"));
   ck_assert_ptr_nonnull(strstr(e, "is not open"));

   // The server is fine: a new simulation can be opened
   open_sim(&f, "hang", id);
   sim_tool(&f, "nvc_sim_close", id, "");

   fixture_close(&f);
}
END_TEST

#define READY_JSON                                                      \
   "{\"event\":\"ready\",\"signals\":0,\"regions\":1,\"root\":\"/\","   \
   "\"now_fs\":0,\"now\":\"0 fs\",\"delta\":0,\"next_event_fs\":null,"  \
   "\"messages\":[],\"messages_truncated\":false,\"counts\":{}}"

// A stand-in for the worker that reports ready and then does as told
#define FAKE_READY                                                      \
   "#!/bin/sh\n"                                                        \
   "for a in \"$@\"; do case \"$a\" in --fd=*) fd=${a#--fd=};; esac; done\n" \
   "printf '%s\\n' '" READY_JSON "' >&$fd\n"

static mcp_server_t *fake_sim_server(sink_t *sink, const char *root,
                                     const char *script)
{
   write_file(root, "worker.sh", script, 0755);

   char *exe = xasprintf("%s/worker.sh", root);
   mcp_server_t *srv = new_server(sink, root, exe);
   free(exe);
   return srv;
}

START_TEST(test_sim_fake_workers)
{
   char *root = make_workspace();
   mem_pool_t *mp = pool_new();
   sink_t sink = {};
   const char *e;
   const json_t *sc;

   // Four simulations at most
   mcp_server_t *srv = fake_sim_server(&sink, root,
                                       FAKE_READY "exec sleep 60\n");
   char ids[4][40];
   for (int i = 0; i < 4; i++) {
      sc = structured(tool(mp, srv, &sink, "nvc_sim_open",
                           "{\"top\":\"anything\"}"));
      snprintf(ids[i], sizeof(ids[i]), "%s", str_field(sc, "sim_id"));
      ck_assert_int_eq(int_field(sc, "open_simulations"), i + 1);
   }

   e = tool_error(tool(mp, srv, &sink, "nvc_sim_open",
                       "{\"top\":\"anything\"}"));
   ck_assert_ptr_nonnull(strstr(e, "4 simulations are already open"));
   ck_assert_ptr_nonnull(strstr(e, ids[0]));

   char args[128];
   snprintf(args, sizeof(args), "{\"sim_id\":\"%s\"}", ids[2]);
   sc = structured(tool(mp, srv, &sink, "nvc_sim_close", args));
   ck_assert_int_eq(int_field(sc, "open_simulations"), 3);
   sc = structured(tool(mp, srv, &sink, "nvc_sim_open",
                        "{\"top\":\"anything\"}"));
   ck_assert_int_eq(int_field(sc, "open_simulations"), 4);

   // A worker that stops answering is stopped and forgotten
   snprintf(args, sizeof(args), "{\"sim_id\":\"%s\",\"max_wall_ms\":100}",
            ids[0]);
   e = tool_error(tool(mp, srv, &sink, "nvc_sim_run", args));
   ck_assert_ptr_nonnull(strstr(e, "did not respond within 100 ms"));
   snprintf(args, sizeof(args), "{\"sim_id\":\"%s\",\"paths\":[\"/a\"]}",
            ids[0]);
   e = tool_error(tool(mp, srv, &sink, "nvc_signal_get", args));
   ck_assert_ptr_nonnull(strstr(e, "is not open"));

   // Freeing the server ends the workers that are left
   mcp_server_free(srv);

   // A worker that dies after starting is reported with its exit status
   srv = fake_sim_server(&sink, root, FAKE_READY "exit 3\n");
   sc = structured(tool(mp, srv, &sink, "nvc_sim_open", "{\"top\":\"x\"}"));
   snprintf(args, sizeof(args), "{\"sim_id\":\"%s\"}", str_field(sc, "sim_id"));
   usleep(200000);
   e = tool_error(tool(mp, srv, &sink, "nvc_design_list", args));
   ck_assert_ptr_nonnull(strstr(e, "exited with status 3"));
   ck_assert_ptr_nonnull(strstr(e, "Open a new simulation"));
   mcp_server_free(srv);

   // A design that cannot be loaded explains why
   srv = fake_sim_server(&sink, root,
                         "#!/bin/sh\necho '** Fatal: WORK.X not elaborated'\n"
                         "exit 1\n");
   e = tool_error(tool(mp, srv, &sink, "nvc_sim_open", "{\"top\":\"x\"}"));
   ck_assert_ptr_nonnull(strstr(e, "could not be loaded"));
   ck_assert_ptr_nonnull(strstr(e, "not elaborated"));
   ck_assert_ptr_nonnull(strstr(e, "nvc_elaborate"));
   mcp_server_free(srv);

   // ... and one that never starts is given up on
   srv = fake_sim_server(&sink, root, "#!/bin/sh\nexec sleep 60\n");
   e = tool_error(tool(mp, srv, &sink, "nvc_sim_open",
                       "{\"top\":\"x\",\"timeout_ms\":1000}"));
   ck_assert_ptr_nonnull(strstr(e, "took too long"));

   // Bad names never reach a process
   e = tool_error(tool(mp, srv, &sink, "nvc_sim_open", "{\"top\":\"a b\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid design unit name"));
   e = tool_error(tool(mp, srv, &sink, "nvc_sim_open",
                       "{\"top\":\"x\",\"library\":\"a:b\"}"));
   ck_assert_ptr_nonnull(strstr(e, "Invalid library name"));
   mcp_server_free(srv);

   sink_clear(&sink);
   pool_free(mp);
   remove_tree(root);
   free(root);
}
END_TEST

START_TEST(test_sim_not_elaborated)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   fixture_open(&f, names, sources, 1, NULL, 0);   // Analysed only

   const char *e = tool_error(tool(f.mp, f.srv, &f.sink, "nvc_sim_open",
                                   "{\"top\":\"tb\",\"std\":\"2008\"}"));
   ck_assert_ptr_nonnull(strstr(e, "not elaborated"));
   ck_assert_ptr_nonnull(strstr(e, "nvc_elaborate"));

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_cancel_run)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   // Drive a second server over the same workspace from a pipe
   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   sink_t sink = {};
   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = f.nvc,
      .root      = f.root,
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   const json_t *sc = structured(tool(f.mp, srv, &sink, "nvc_sim_open",
                                      "{\"top\":\"tb\",\"std\":\"2008\"}"));
   char id[40];
   snprintf(id, sizeof(id), "%s", str_field(sc, "sim_id"));

   char *run = xasprintf(
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":"
      "{\"name\":\"nvc_sim_run\",\"arguments\":{\"sim_id\":\"%s\","
      "\"pause_on\":\"never\",\"max_wall_ms\":120000}," MODERN_META "}}\n", id);
   char *later = xasprintf(
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
      "\"params\":{\"requestId\":7}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
      "{\"name\":\"nvc_signal_get\",\"arguments\":{\"sim_id\":\"%s\","
      "\"paths\":[\"/q\"]}," MODERN_META "}}\n", id);

   ck_assert_int_eq(write(fds[1], run, strlen(run)), (ssize_t)strlen(run));

   sink_clear(&sink);
   writer_t w = { fds[1], later, 400 };
   pthread_t thread;
   ck_assert_int_eq(pthread_create(&thread, NULL, delayed_writer, &w), 0);

   const uint64_t start = get_timestamp_us();
   mcp_server_run(srv);
   const uint64_t ms = (get_timestamp_us() - start) / 1000;
   pthread_join(thread, NULL);

   // The long run was cut short and got no response; the simulation kept
   // what it had simulated and answers the next request
   ck_assert_int_lt(ms, 30000);
   ck_assert_int_eq(sink.count, 1);
   json_t *r = parse_ok(f.mp, sink.lines[0]);
   ck_assert_int_eq(int_field(r, "id"), 8);
   sc = structured(json_get(r, "result"));
   ck_assert_int_gt(int_field(sc, "now_fs"), 0);

   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
   free(run);
   free(later);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_progress)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   char *req = xasprintf(
      "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
      "{\"name\":\"nvc_sim_run\",\"arguments\":{\"sim_id\":\"%s\","
      "\"pause_on\":\"never\",\"max_wall_ms\":900},\"_meta\":{"
      "\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
      "\"io.modelcontextprotocol/clientCapabilities\":{},"
      "\"progressToken\":\"tok-1\"}}}", id);

   sink_clear(&f.sink);
   mcp_server_handle_line(f.srv, req, strlen(req));
   free(req);

   // Notifications for the token come first, with rising progress, then
   // the one response
   ck_assert_int_ge(f.sink.count, 3);
   int64_t previous = -1;
   for (size_t i = 0; i + 1 < f.sink.count; i++) {
      json_t *n = parse_ok(f.mp, f.sink.lines[i]);
      ck_assert_str_eq(str_field(n, "method"), "notifications/progress");
      const json_t *params = json_get(n, "params");
      ck_assert_str_eq(str_field(params, "progressToken"), "tok-1");
      ck_assert_int_gt(int_field(params, "progress"), previous);
      previous = int_field(params, "progress");
   }

   json_t *last = parse_ok(f.mp, f.sink.lines[f.sink.count - 1]);
   ck_assert_int_eq(int_field(last, "id"), 5);

   // Without a token there are no notifications
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"pause_on\":\"never\","
                                          "\"max_wall_ms\":600"));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "wall_budget");
   ck_assert_int_eq(f.sink.count, 1);

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_background)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"background\":true,"
                                          "\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   // Reading works during the run, changing the simulation does not
   sc = structured(sim_tool(&f, "nvc_signal_get", id, "\"paths\":[\"/clk\"]"));
   ck_assert_ptr_nonnull(sc);

   const char *err = tool_error(sim_tool(&f, "nvc_signal_set", id,
                                         "\"path\":\"/rst\","
                                         "\"value\":\"1\""));
   ck_assert_ptr_nonnull(strstr(err, "nvc_sim_stop"));

   err = tool_error(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"1 ns\""));
   ck_assert_ptr_nonnull(strstr(err, "already in progress"));

   // A short wait times out while the run goes on
   sc = structured(sim_tool(&f, "nvc_sim_wait", id, "\"timeout_ms\":400"));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   // The design asserts repeatedly, so the wait may end early on new errors
   const char *why = str_field(sc, "woke_because");
   ck_assert(!strcmp(why, "timeout") || !strcmp(why, "new_errors"));
   ck_assert_int_gt(int_field(sc, "sim_fs_per_second"), 0);

   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_str_eq(str_field(sc, "state"), "running");
   const int64_t then = int_field(sc, "now_fs");
   ck_assert_int_gt(then, 0);

   // The first message is at least the start-up note, with a sequence
   ck_assert_int_gt(int_field(sc, "next_seq"), 0);

   sc = structured(sim_tool(&f, "nvc_sim_stop", id, ""));
   ck_assert_str_ne(str_field(sc, "state"), "running");
   ck_assert_str_eq(str_field(sc, "stopped_because"), "cancelled");
   ck_assert_int_ge(int_field(sc, "now_fs"), then);

   // The simulation continues from where it stopped
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"10 ns\","
                            "\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");

   // A bounded background run stops by itself and wait returns early
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"background\":true,"
                            "\"duration\":\"1 ns\",\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_wait", id, "\"timeout_ms\":20000"));
   ck_assert_str_eq(str_field(sc, "woke_because"), "stopped");
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");

   // The foreground limit still applies
   err = tool_error(sim_tool(&f, "nvc_sim_run", id,
                             "\"max_wall_ms\":400000"));
   ck_assert_ptr_nonnull(strstr(err, "background"));

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_waves)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // Only the design under test is recorded
   const json_t *sc = structured(sim_tool(&f, "nvc_wave_start", id,
                                          "\"include\":[\"/dut/*\"],"
                                          "\"name\":\"dut\""));
   const char *file = str_field(sc, "file");
   ck_assert_ptr_nonnull(strstr(file, "dut.fst"));
   char *wave_file = xstrdup(file);

   // Only one file at a time
   const char *err = tool_error(sim_tool(&f, "nvc_wave_start", id, ""));
   ck_assert_ptr_nonnull(strstr(err, "already active"));

   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"1 us\","
                            "\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "time");

   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *files = json_get(sc, "files");
   ck_assert_ptr_nonnull(files);
   ck_assert_ptr_nonnull(json_get(files, "output_log"));

   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   ck_assert_int_gt(int_field(sc, "bytes"), 0);

   char *args = xasprintf("\"file\":\"%s\"", wave_file);
   char *extra = xasprintf("%s,\"paths\":[\"/dut/cnt\"],\"from\":\"40 ns\","
                           "\"until\":\"80 ns\",\"radix\":\"decimal\"", args);

   char *call_args = xasprintf("{%s}", args);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_summary", call_args));
   ck_assert_int_eq(int_field(sc, "signals_total"), 2);
   ck_assert_int_eq(int_field(sc, "end_fs"), 1000000000);
   free(call_args);

   call_args = xasprintf("{%s}", extra);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_get", call_args));
   const json_t *sig = json_at(json_get(sc, "signals"), 0);
   ck_assert_str_eq(str_field(sig, "value_at_from"), "3");
   const json_t *changes = json_get(sig, "changes");
   ck_assert_int_eq(changes->array.count, 4);
   ck_assert_int_eq(json_at(json_at(changes, 0), 0)->integer, 45000000);
   ck_assert_str_eq(json_string(json_at(json_at(changes, 0), 1)), "4");
   free(call_args);
   free(extra);

   extra = xasprintf("{%s,\"when\":\"/dut/cnt = x\\\"5\\\"\","
                     "\"max_matches\":2}", args);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_find", extra));
   ck_assert_int_eq(json_get(sc, "matches")->array.count, 2);
   ck_assert_int_eq(int_field(json_at(json_get(sc, "matches"), 0), "start_fs"),
                    55000000);
   free(extra);

   // Unknown signals are explained
   extra = xasprintf("{%s,\"paths\":[\"/dut/count\"]}", args);
   err = tool_error(tool(f.mp, f.srv, &f.sink, "nvc_wave_get", extra));
   ck_assert_ptr_nonnull(strstr(err, "nvc_wave_summary"));
   free(extra);

   // Files outside the simulation directory cannot be read
   err = tool_error(tool(f.mp, f.srv, &f.sink, "nvc_sim_log",
                         "{\"file\":\"../../../etc/passwd\"}"));
   ck_assert_ptr_nonnull(strstr(err, ".nvc-mcp"));

   free(args);

   // The output log keeps the whole text
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *lf = json_get(json_get(json_get(sc, "files"), "output_log"),
                               "file");
   char *log_args = xasprintf("{\"file\":\"%s\",\"grep\":\"tick q=3\"}",
                              json_string(lf));
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_sim_log", log_args));
   ck_assert_int_gt(int_field(sc, "matches_total"), 0);
   free(log_args);

   // A second file can follow the first
   sc = structured(sim_tool(&f, "nvc_wave_start", id, "\"max_bytes\":200"));
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"5 us\","
                            "\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *wave = json_get(json_get(sc, "files"), "wave");
   bool quota = false;
   json_as_bool(json_get(wave, "quota_reached"), &quota);
   ck_assert(quota);

   free(wave_file);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_wait_cancel)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   static const char quiet[] =
      "entity quiet is end entity;\n"
      "architecture test of quiet is\n"
      "  signal clk : bit;\n"
      "begin\n"
      "  clk <= not clk after 5 ns;\n"
      "end architecture;\n";
   names[0] = "quiet.vhd";
   sources[0] = quiet;
   tops[0] = "quiet";
   fixture_open(&f, names, sources, 1, tops, 1);

   int fds[2];
   ck_assert_int_eq(pipe(fds), 0);

   sink_t sink = {};
   const mcp_options_t opts = {
      .write     = sink_write,
      .write_ctx = &sink,
      .in_fd     = fds[0],
      .exe       = f.nvc,
      .root      = f.root,
   };
   mcp_server_t *srv = mcp_server_new(&opts);

   const json_t *sc = structured(tool(f.mp, srv, &sink, "nvc_sim_open",
                                      "{\"top\":\"quiet\",\"std\":\"2008\"}"));
   char id[40];
   snprintf(id, sizeof(id), "%s", str_field(sc, "sim_id"));

   char *args = xasprintf("{\"sim_id\":\"%s\",\"background\":true,"
                          "\"pause_on\":\"never\"}", id);
   sc = structured(tool(f.mp, srv, &sink, "nvc_sim_run", args));
   free(args);
   ck_assert_str_eq(str_field(sc, "state"), "running");

   char *wait = xasprintf(
      "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":"
      "{\"name\":\"nvc_sim_wait\",\"arguments\":{\"sim_id\":\"%s\","
      "\"timeout_ms\":30000}," MODERN_META "}}\n", id);
   char *later = xasprintf(
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
      "\"params\":{\"requestId\":9}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":"
      "{\"name\":\"nvc_sim_status\",\"arguments\":{\"sim_id\":\"%s\"},"
      MODERN_META "}}\n", id);

   ck_assert_int_eq(write(fds[1], wait, strlen(wait)), (ssize_t)strlen(wait));

   sink_clear(&sink);
   writer_t w = { fds[1], later, 400 };
   pthread_t thread;
   ck_assert_int_eq(pthread_create(&thread, NULL, delayed_writer, &w), 0);

   const uint64_t start = get_timestamp_us();
   mcp_server_run(srv);
   const uint64_t ms = (get_timestamp_us() - start) / 1000;
   pthread_join(thread, NULL);

   // The wait ends early and the run itself carries on
   ck_assert_int_lt(ms, 20000);
   ck_assert_int_eq(sink.count, 1);
   json_t *r = parse_ok(f.mp, sink.lines[0]);
   ck_assert_int_eq(int_field(r, "id"), 10);
   sc = structured(json_get(r, "result"));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   mcp_server_free(srv);
   sink_clear(&sink);
   close(fds[0]);
   close(fds[1]);
   free(wait);
   free(later);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_files_misc)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // Bad and repeated names
   const char *err = tool_error(sim_tool(&f, "nvc_wave_start", id,
                                         "\"name\":\"../x\""));
   ck_assert_ptr_nonnull(strstr(err, "Invalid name"));
   err = tool_error(sim_tool(&f, "nvc_wave_stop", id, ""));
   ck_assert_ptr_nonnull(strstr(err, "No recording"));

   // Start recording in the middle of a background run
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"duration\":\"20 ns\","
                                          "\"pause_on\":\"never\""));
   ck_assert_int_gt(int_field(sc, "next_seq"), 0);
   const int64_t seq = int_field(sc, "next_seq");

   sc = structured(sim_tool(&f, "nvc_wave_start", id, "\"name\":\"late\","
                            "\"include\":[\"/dut/cnt\"]"));
   char *file = xstrdup(str_field(sc, "file"));
   err = tool_error(sim_tool(&f, "nvc_wave_start", id, "\"name\":\"late\""));
   ck_assert_ptr_nonnull(strstr(err, "already active"));

   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"500 ns\","
                            "\"pause_on\":\"never\",\"background\":true"));
   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   ck_assert_int_gt(int_field(sc, "bytes"), 0);
   sc = structured(sim_tool(&f, "nvc_sim_wait", id, "\"timeout_ms\":20000"));
   ck_assert_str_eq(str_field(sc, "state"), "idle");

   // Only messages after since_seq come back
   char *extra = xasprintf("\"since_seq\":%"PRIi64, int_field(sc, "next_seq"));
   sc = structured(sim_tool(&f, "nvc_sim_status", id, extra));
   free(extra);
   ck_assert_int_eq(json_get(sc, "messages")->array.count, 0);
   (void)seq;

   // The recording starts at the time it was asked for, in hex on request
   char *call_args = xasprintf("{\"file\":\"%s\",\"paths\":[\"/dut/cnt\"],"
                               "\"radix\":\"hex\",\"max_changes\":3}", file);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_get", call_args));
   free(call_args);
   ck_assert_int_eq(int_field(sc, "from_fs"), 20000000);
   ck_assert_int_eq(json_get(json_at(json_get(sc, "signals"), 0),
                             "changes")->array.count, 3);
   bool truncated = false;
   json_as_bool(json_get(sc, "truncated"), &truncated);
   ck_assert(truncated);
   const char *v = json_string(json_at(json_at(json_get(json_at(
      json_get(sc, "signals"), 0), "changes"), 0), 1));
   ck_assert_ptr_nonnull(v);
   ck_assert_int_eq(v[0], '0');
   ck_assert_int_eq(v[1], 'x');

   // A file that is not a waveform is refused with advice
   char *junk = xasprintf("%s/.nvc-mcp/%s/junk.fst", f.root, id);
   FILE *jf = fopen(junk, "w");
   ck_assert_ptr_nonnull(jf);
   fputs("not a waveform", jf);
   fclose(jf);
   free(junk);
   call_args = xasprintf("{\"file\":\".nvc-mcp/%s/junk.fst\"}", id);
   err = tool_error(tool(f.mp, f.srv, &f.sink, "nvc_wave_summary", call_args));
   ck_assert_ptr_nonnull(strstr(err, "nvc_wave_stop"));
   free(call_args);

   // Closing finishes a recording that is still active
   sc = structured(sim_tool(&f, "nvc_wave_start", id, "\"name\":\"open\""));
   char *open_file = xstrdup(str_field(sc, "file"));
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"50 ns\","
                            "\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_close", id, ""));
   call_args = xasprintf("{\"file\":\"%s\"}", open_file);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_summary", call_args));
   ck_assert_int_gt(int_field(sc, "signals_total"), 0);
   free(call_args);

   free(open_file);
   free(file);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_log_limit)
{
   setenv("NVC_MCP_LOG_MAX", "200", 1);

   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"duration\":\"3 us\","
                                          "\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"3 us\","
                            "\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *log = json_get(json_get(sc, "files"), "output_log");
   bool full = false;
   json_as_bool(json_get(log, "limit_reached"), &full);
   ck_assert(full);

   // The size stops growing once the limit is hit
   const int64_t size = int_field(log, "bytes");
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"3 us\","
                            "\"pause_on\":\"never\""));
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_int_eq(int_field(json_get(json_get(sc, "files"), "output_log"),
                              "bytes"), size);

   unsetenv("NVC_MCP_LOG_MAX");
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_busy)
{
   static const char busy[] =
      "entity busy is end entity;\n"
      "architecture test of busy is\n"
      "begin\n"
      "  process\n"
      "    variable v : natural := 0;\n"
      "  begin\n"
      "    wait for 1 ns;\n"
      "    for i in 1 to 2000000000 loop\n"
      "      v := (v + i) mod 1000;\n"
      "    end loop;\n"
      "    wait;\n"
      "  end process;\n"
      "end architecture;\n";

   fixture_t f;
   const char *names[] = { "busy.vhd" }, *sources[] = { busy };
   const char *tops[] = { "busy" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "busy", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"background\":true"));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   // The process is inside one long step, so it cannot answer, but the
   // call still returns and says what it knows
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_str_eq(str_field(sc, "state"), "running");
   bool responsive = true;
   json_as_bool(json_get(sc, "responsive"), &responsive);
   ck_assert(!responsive);
   ck_assert_ptr_nonnull(json_get(json_get(sc, "worker"), "cpu_seconds"));
   ck_assert_ptr_nonnull(json_get(sc, "hint"));

   // A wait gives up at once instead of sitting through its timeout
   sc = structured(sim_tool(&f, "nvc_sim_wait", id, "\"timeout_ms\":20000"));
   ck_assert_str_eq(str_field(sc, "woke_because"), "unresponsive");

   // The simulation is still open and can be closed
   sc = structured(sim_tool(&f, "nvc_sim_close", id, ""));
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_live_trace)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"background\":true,"
                                          "\"pause_on\":\"never\""));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   // A recording can be started in the middle of the run and read at once
   sc = structured(sim_tool(&f, "nvc_wave_start", id,
                            "\"include\":[\"/dut/*\"],\"to\":\"memory\","
                            "\"max_changes\":1000"));
   const int64_t started = int_field(sc, "now_fs");

   struct timespec ts = { 0, 300 * 1000 * 1000 };
   nanosleep(&ts, NULL);

   sc = structured(sim_tool(&f, "nvc_wave_get", id,
                            "\"paths\":[\"/dut/cnt\"],\"max_changes\":5"));
   ck_assert_int_gt(int_field(sc, "changes_total"), 5);
   ck_assert_int_gt(int_field(sc, "now_fs"), started);

   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   sc = structured(sim_tool(&f, "nvc_sim_stop", id, ""));
   ck_assert_str_ne(str_field(sc, "state"), "running");
   fixture_close(&f);
}
END_TEST

START_TEST(test_cond)
{
   char err[800];

   cond_t *c = cond_parse("/a = '1' and /b = x\"0F\" and not /c = IDLE", err,
                          sizeof(err));
   ck_assert_ptr_nonnull(c);
   ck_assert_int_eq(cond_nrefs(c), 3);
   ck_assert_str_eq(cond_ref(c, 1), "/b");

   cond_val_t v[3] = {
      { .cur = "1", .bits = true },
      { .cur = "00001111", .bits = true },
      { .cur = "SEND", .bits = false },
   };
   ck_assert(cond_eval(c, v));
   v[2].cur = "IDLE";
   ck_assert(!cond_eval(c, v));
   v[2].cur = "SEND";
   v[1].cur = "00001110";
   ck_assert(!cond_eval(c, v));
   cond_free(c);

   // Ordering compares vectors as unsigned numbers
   c = cond_parse("/n >= 16 and /n < 32 or /n = 7", err, sizeof(err));
   ck_assert_ptr_nonnull(c);
   cond_val_t n = { .cur = "00010000", .bits = true };
   ck_assert(cond_eval(c, &n));
   n.cur = "00100000";
   ck_assert(!cond_eval(c, &n));
   n.cur = "0111";
   ck_assert(cond_eval(c, &n));
   n.cur = "01UX";   // Not a number: only equality can hold
   ck_assert(!cond_eval(c, &n));
   cond_free(c);

   // Edges need the previous sample
   c = cond_parse("rose(/s) or fell(/t)", err, sizeof(err));
   ck_assert_ptr_nonnull(c);
   cond_val_t e[2] = { { .cur = "1", .prev = "0", .bits = true },
                       { .cur = "1", .prev = "1", .bits = true } };
   ck_assert(cond_eval(c, e));
   e[0].prev = NULL;
   ck_assert(!cond_eval(c, e));
   e[1].prev = "1";
   e[1].cur = "0";
   ck_assert(cond_eval(c, e));
   cond_free(c);

   // Paths with generate indexes and a signal on its own
   c = cond_parse("/gen(0)/u/c and (/q /= 3)", err, sizeof(err));
   ck_assert_ptr_nonnull(c);
   ck_assert_str_eq(cond_ref(c, 0), "/gen(0)/u/c");
   cond_free(c);

   // Mistakes say where and give an example
   static const char *bad[] = {
      "", "/a =", "/a = '1' and", "(/a = '1'", "/a ! '1'", "a = '1'",
      "/a = '1' /b = '0'", "/a = \"01", "rose(/a",
   };
   for (size_t i = 0; i < ARRAY_LEN(bad); i++) {
      ck_assert_ptr_null(cond_parse(bad[i], err, sizeof(err)));
      ck_assert_ptr_nonnull(strstr(err, "Invalid condition at character"));
      ck_assert_ptr_nonnull(strstr(err, "/dut/valid"));
   }
}
END_TEST

START_TEST(test_sim_queries)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   sim_tool(&f, "nvc_wave_start", id, "\"include\":[\"/clk\",\"/dut/*\"],"
            "\"name\":\"q\"");
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"duration\":\"300 ns\","
                                          "\"pause_on\":\"never\""));

   // Without a clock the condition is tested after every change: cnt
   // becomes 5 at 55 ns.  With the clock it is tested on the values the
   // registers see just before each edge, so the first sample is 65 ns.
   sc = structured(sim_tool(&f, "nvc_wave_find", id,
                            "\"when\":\"/dut/cnt = 5\",\"max_matches\":2"));
   ck_assert_int_eq(json_get(sc, "matches")->array.count, 2);
   ck_assert_int_eq(int_field(json_at(json_get(sc, "matches"), 0), "start_fs"),
                    55000000);

   sc = structured(sim_tool(&f, "nvc_wave_find", id,
                            "\"when\":\"/dut/cnt = 5\",\"clock\":\"/clk\","
                            "\"max_matches\":1"));
   ck_assert_int_eq(int_field(json_at(json_get(sc, "matches"), 0), "start_fs"),
                    65000000);
   ck_assert_int_eq(int_field(json_at(json_get(sc, "matches"), 0), "cycles"), 1);
   ck_assert_int_ge(int_field(sc, "matches_total"), 2);

   // A condition that never holds says so
   sc = structured(sim_tool(&f, "nvc_wave_find", id,
                            "\"when\":\"/dut/cnt = 99\",\"clock\":\"/clk\""));
   ck_assert_int_eq(int_field(sc, "matches_total"), 0);
   ck_assert_ptr_nonnull(json_get(sc, "hint"));

   const char *e = tool_error(sim_tool(&f, "nvc_wave_find", id,
                                       "\"when\":\"/dut/cnt =\""));
   ck_assert_ptr_nonnull(strstr(e, "Invalid condition"));
   e = tool_error(sim_tool(&f, "nvc_wave_find", id,
                           "\"when\":\"/dut/nope = 1\""));
   ck_assert_ptr_nonnull(strstr(e, "in-memory recording"));
   e = tool_error(sim_tool(&f, "nvc_wave_find", id,
                           "\"when\":\"/dut/cnt = 5\",\"at_least_cycles\":2"));
   ck_assert_ptr_nonnull(strstr(e, "clock"));

   // One row per clock edge, around a time, with the assertion on its cycle
   sc = structured(sim_tool(&f, "nvc_wave_table", id,
                            "\"clock\":\"/clk\",\"signals\":[\"/dut/cnt\"],"
                            "\"around\":\"65 ns\",\"cycles\":2"));
   const json_t *rows = json_get(sc, "rows");
   ck_assert_int_eq(rows->array.count, 5);
   ck_assert_int_eq(json_get(sc, "columns")->array.count, 5);
   ck_assert_str_eq(json_string(json_at(json_at(rows, 0), 1)), "45 ns");
   ck_assert_str_eq(json_string(json_at(json_at(rows, 0), 2)), "0x3");
   ck_assert_str_eq(json_string(json_at(json_at(rows, 2), 1)), "65 ns");
   ck_assert_str_eq(json_string(json_at(json_at(rows, 2), 2)), "0x5");
   ck_assert_ptr_nonnull(strstr(json_string(json_at(json_at(rows, 2), 3)),
                                "q reached five"));

   // A finished file answers the same questions
   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   char *file = xstrdup(str_field(sc, "file"));
   char *args = xasprintf("{\"file\":\"%s\",\"when\":\"/dut/cnt = 5\","
                          "\"clock\":\"/clk\",\"max_matches\":1}", file);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_find", args));
   ck_assert_int_eq(int_field(json_at(json_get(sc, "matches"), 0), "start_fs"),
                    65000000);
   free(args);

   args = xasprintf("{\"file\":\"%s\",\"clock\":\"/clk\","
                    "\"signals\":[\"/dut/cnt\"],\"from\":\"100 ns\","
                    "\"until\":\"140 ns\",\"radix\":\"decimal\"}", file);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_table", args));
   ck_assert_int_eq(json_get(sc, "rows")->array.count, 4);
   ck_assert_str_eq(json_string(json_at(json_at(json_get(sc, "rows"), 0), 2)),
                    "9");
   free(args);
   free(file);

   sim_tool(&f, "nvc_sim_close", id, "");
   fixture_close(&f);
}
END_TEST

////////////////////////////////////////////////////////////////////////////////
// Checkpoints

// Reads one number from each line of stim.txt at every clock edge, so the
// state of the design includes the position in an open file
static const char reader_src[] =
   "library std;\n"
   "use std.textio.all;\n"
   "entity reader is end entity;\n"
   "architecture a of reader is\n"
   "  signal clk : bit := '0';\n"
   "  signal total : integer := 0;\n"
   "  signal count : integer := 0;\n"
   "begin\n"
   "  clk <= not clk after 5 ns;\n"
   "  process\n"
   "    file f : text open read_mode is \"stim.txt\";\n"
   "    variable l : line;\n"
   "    variable v, s, c : integer := 0;\n"
   "  begin\n"
   "    while not endfile(f) loop\n"
   "      wait until clk = '1';\n"
   "      readline(f, l);\n"
   "      read(l, v);\n"
   "      s := s + v;\n"
   "      c := c + 1;\n"
   "      total <= s;\n"
   "      count <= c;\n"
   "    end loop;\n"
   "    report \"stimulus read\" severity note;\n"
   "    wait;\n"
   "  end process;\n"
   "end architecture;\n";

#define STIM_LINES 5000

static int64_t stim_value(int i)
{
   return (i * 37) % 1000 + 1;
}

static const char *signal_value(fixture_t *f, const char *id, const char *path)
{
   char *extra = xasprintf("\"paths\":[\"%s\"]", path);
   const json_t *sc = structured(sim_tool(f, "nvc_signal_get", id, extra));
   free(extra);
   return str_field(json_at(json_get(sc, "values"), 0), "value");
}

static void expect_same_messages(const json_t *a, const json_t *b)
{
   ck_assert_int_eq(a->array.count, b->array.count);
   for (size_t i = 0; i < a->array.count; i++) {
      ck_assert_int_eq(int_field(json_at(a, i), "time_fs"),
                       int_field(json_at(b, i), "time_fs"));
      ck_assert_str_eq(str_field(json_at(a, i), "message"),
                       str_field(json_at(b, i), "message"));
      ck_assert_str_eq(str_field(json_at(a, i), "severity"),
                       str_field(json_at(b, i), "severity"));
   }
}

// A process that has gone is no longer there for kill.  When its parent
// has gone too it stays a zombie until init collects it, which can take
// a few seconds.
static bool wait_gone(pid_t pid)
{
   for (int i = 0; i < 500; i++) {
      if (kill(pid, 0) < 0)
         return true;

      usleep(20000);
   }

   return false;
}

static pid_t worker_pid(fixture_t *f, const char *id)
{
   const json_t *sc = structured(sim_tool(f, "nvc_sim_status", id, ""));
   return int_field(json_get(sc, "worker"), "pid");
}

START_TEST(test_sim_checkpoint)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_run", id,
                                          "\"duration\":\"100 ns\","
                                          "\"pause_on\":\"never\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 100000000);

   sc = structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   ck_assert_int_eq(int_field(sc, "checkpoint_id"), 1);
   ck_assert_int_eq(int_field(sc, "now_fs"), 100000000);
   ck_assert_str_eq(str_field(sc, "now"), "100 ns");
   const pid_t snap_pid = int_field(sc, "pid");

   // The original goes on: note what it does
   sc = structured(sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"200 ns\","
                            "\"pause_on\":\"never\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 300000000);
   const json_t *orig_msgs = json_get(sc, "messages");
   char *orig_out = xstrdup(str_field(sc, "sim_output"));
   ck_assert_int_gt(orig_msgs->array.count, 0);
   char *orig_q = xstrdup(signal_value(&f, id, "/q"));
   char *orig_cnt = xstrdup(signal_value(&f, id, "/dut/cnt"));

   // The status lists it
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *list = json_get(sc, "checkpoints");
   ck_assert_int_eq(list->array.count, 1);
   ck_assert_int_eq(int_field(json_at(list, 0), "id"), 1);
   ck_assert_int_eq(int_field(json_at(list, 0), "time_fs"), 100000000);
   ck_assert_str_eq(str_field(json_at(list, 0), "time"), "100 ns");
   ck_assert(!bool_field(json_at(list, 0), "auto"));

   // The restored simulation is a new one at the time of the checkpoint
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":1"));
   char rid[40];
   snprintf(rid, sizeof(rid), "%s", str_field(sc, "sim_id"));
   ck_assert_str_ne(rid, id);
   ck_assert_int_eq(int_field(sc, "now_fs"), 100000000);
   ck_assert_int_eq(int_field(sc, "open_simulations"), 2);
   const json_t *from = json_get(sc, "restored_from");
   ck_assert_str_eq(str_field(from, "sim_id"), id);
   ck_assert_int_eq(int_field(from, "checkpoint_id"), 1);
   ck_assert_int_eq(json_get(sc, "messages")->array.count, 0);
   char *rdir = xstrdup(str_field(sc, "files_directory"));

   ck_assert_str_eq(signal_value(&f, rid, "/dut/cnt"), "1001");

   sc = structured(sim_tool(&f, "nvc_sim_status", rid, ""));
   ck_assert_str_eq(str_field(sc, "state"), "idle");

   sc = structured(sim_tool(&f, "nvc_sim_run", rid, "\"duration\":\"200 ns\","
                            "\"pause_on\":\"never\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 300000000);
   expect_same_messages(orig_msgs, json_get(sc, "messages"));
   ck_assert_str_eq(str_field(sc, "sim_output"), orig_out);
   ck_assert_str_eq(signal_value(&f, rid, "/q"), orig_q);
   ck_assert_str_eq(signal_value(&f, rid, "/dut/cnt"), orig_cnt);

   // The original was not touched by any of that
   ck_assert_str_eq(signal_value(&f, id, "/q"), orig_q);
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 300000000);

   // The log of the new simulation is in its own directory and holds the
   // same text
   char *log = xasprintf("%s/%s/output.log", f.root, rdir);
   char *olog = xasprintf("%s/.nvc-mcp/%s/output.log", f.root, id);
   struct stat a, b;
   ck_assert_int_eq(stat(log, &a), 0);
   ck_assert_int_eq(stat(olog, &b), 0);
   ck_assert_int_gt(a.st_size, 0);
   ck_assert_int_eq(a.st_size, b.st_size);
   free(log);
   free(olog);

   // Either can be closed without the other: the checkpoint belongs to the
   // original and goes with it, the restored simulation does not
   const pid_t rpid = worker_pid(&f, rid);
   sc = structured(sim_tool(&f, "nvc_sim_close", id, ""));
   ck_assert_int_eq(int_field(sc, "open_simulations"), 1);
   ck_assert(wait_gone(snap_pid));
   sc = structured(sim_tool(&f, "nvc_sim_run", rid, "\"duration\":\"10 ns\","
                            "\"pause_on\":\"never\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 310000000);
   sim_tool(&f, "nvc_sim_close", rid, "");
   ck_assert(wait_gone(rpid));

   free(rdir);
   free(orig_q);
   free(orig_cnt);
   free(orig_out);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_restore_files)
{
   fixture_t f;
   const char *names[] = { "reader.vhd" }, *sources[] = { reader_src };
   const char *tops[] = { "reader" };
   fixture_open(&f, names, sources, 1, tops, 1);

   // More than a few buffers of input, so the position in the file matters
   LOCAL_TEXT_BUF stim = tb_new();
   int64_t expect = 0;
   for (int i = 0; i < STIM_LINES; i++) {
      tb_printf(stim, "%"PRIi64"\n", stim_value(i));
      expect += stim_value(i);
   }
   ck_assert_int_gt(tb_len(stim), 4 * 4096);
   write_file(f.root, "stim.txt", tb_get(stim), 0644);

   char id[40];
   open_sim(&f, "reader", id);

   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"1000 ns\","
            "\"pause_on\":\"never\"");
   ck_assert_str_eq(signal_value(&f, id, "/count"), "100");
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));

   // The original reads on past the end of the buffer the copy has, which
   // moves the offset of the file they would share
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"20 us\","
            "\"pause_on\":\"never\"");
   ck_assert_str_eq(signal_value(&f, id, "/count"), "2000");

   char r1[40], r2[40];
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_restore", id,
                                          "\"checkpoint_id\":1"));
   snprintf(r1, sizeof(r1), "%s", str_field(sc, "sim_id"));
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":1"));
   snprintf(r2, sizeof(r2), "%s", str_field(sc, "sim_id"));
   ck_assert_str_eq(signal_value(&f, r1, "/count"), "100");
   ck_assert_str_eq(signal_value(&f, r2, "/count"), "100");

   // The original and both restored simulations read at the same time:
   // each must see every line once, whatever the others do
   const char *all[] = { id, r1, r2 };
   for (int i = 1; i < 3; i++)
      sim_tool(&f, "nvc_sim_run", all[i], "\"background\":true,"
               "\"until\":\"60 us\",\"pause_on\":\"never\"");
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"60 us\","
            "\"pause_on\":\"never\"");

   for (int i = 1; i < 3; i++) {
      sc = structured(sim_tool(&f, "nvc_sim_wait", all[i],
                               "\"timeout_ms\":20000"));
      ck_assert_str_eq(str_field(sc, "state"), "idle");
   }

   char want[32];
   snprintf(want, sizeof(want), "%"PRIi64, expect);
   for (int i = 0; i < 3; i++) {
      ck_assert_str_eq(signal_value(&f, all[i], "/total"), want);
      ck_assert_str_eq(signal_value(&f, all[i], "/count"), "5000");
   }

   // Restoring once more after all that still starts from line 100
   sim_tool(&f, "nvc_sim_close", r2, "");
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":1"));
   snprintf(r2, sizeof(r2), "%s", str_field(sc, "sim_id"));
   ck_assert_str_eq(signal_value(&f, r2, "/count"), "100");
   sim_tool(&f, "nvc_sim_run", r2, "\"until\":\"60 us\","
            "\"pause_on\":\"never\"");
   ck_assert_str_eq(signal_value(&f, r2, "/total"), want);

   // A file that has gone cannot be opened again: say so
   char *path = xasprintf("%s/stim.txt", f.root);
   ck_assert_int_eq(unlink(path), 0);
   free(path);
   const char *err = tool_error(sim_tool(&f, "nvc_sim_restore", id,
                                         "\"checkpoint_id\":1"));
   ck_assert_ptr_nonnull(strstr(err, "stim.txt"));
   ck_assert_ptr_nonnull(strstr(err, "cannot open"));

   // ... and the failed attempt used no slot
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_int_eq(json_get(sc, "checkpoints")->array.count, 1);
   sc = structured(sim_tool(&f, "nvc_sim_close", r2, ""));
   ck_assert_int_eq(int_field(sc, "open_simulations"), 2);

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_restore_errors)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const char *err = tool_error(sim_tool(&f, "nvc_sim_restore", id,
                                         "\"checkpoint_id\":1"));
   ck_assert_ptr_nonnull(strstr(err, "does not exist"));
   err = tool_error(sim_tool(&f, "nvc_sim_restore", id, ""));
   ck_assert_ptr_nonnull(strstr(err, "checkpoint_id or at"));
   err = tool_error(sim_tool(&f, "nvc_sim_restore", id,
                             "\"checkpoint_id\":1,\"at\":\"1 ns\""));
   ck_assert_ptr_nonnull(strstr(err, "checkpoint_id or at"));

   // Checkpoints at 0 (before anything ran), 100 ns and 200 ns
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"100 ns\","
            "\"pause_on\":\"never\"");
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"200 ns\","
            "\"pause_on\":\"never\"");
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"250 ns\","
            "\"pause_on\":\"never\"");

   // The latest checkpoint at or before the time
   struct { const char *at; int64_t want_id; int64_t want_fs; } cases[] = {
      { "150 ns", 2, 100000000 }, { "200 ns", 3, 200000000 },
      { "99 ns", 1, 0 }, { "0.2 us", 3, 200000000 }, { "1 ms", 3, 200000000 },
   };

   for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
      char *extra = xasprintf("\"at\":\"%s\"", cases[i].at);
      const json_t *sc = structured(sim_tool(&f, "nvc_sim_restore", id, extra));
      free(extra);
      ck_assert_int_eq(int_field(json_get(sc, "restored_from"),
                                 "checkpoint_id"), cases[i].want_id);
      ck_assert_int_eq(int_field(sc, "now_fs"), cases[i].want_fs);
      sim_tool(&f, "nvc_sim_close", str_field(sc, "sim_id"), "");
   }

   err = tool_error(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":9"));
   ck_assert_ptr_nonnull(strstr(err, "Checkpoints: 1 (0 fs), 2 (100 ns), "
                                "3 (200 ns)"));
   err = tool_error(sim_tool(&f, "nvc_sim_restore", id, "\"at\":\"soon\""));
   ck_assert_ptr_nonnull(strstr(err, "Invalid at"));

   // Nothing before the time asked for
   sim_tool(&f, "nvc_sim_checkpoint_delete", id, "\"checkpoint_id\":1");
   err = tool_error(sim_tool(&f, "nvc_sim_restore", id, "\"at\":\"50 ns\""));
   ck_assert_ptr_nonnull(strstr(err, "No checkpoint at or before 50 ns"));

   err = tool_error(sim_tool(&f, "nvc_sim_checkpoint_delete", id,
                             "\"checkpoint_id\":1"));
   ck_assert_ptr_nonnull(strstr(err, "does not exist"));

   // At most 16 on request
   for (int i = 0; i < 14; i++)
      structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_int_eq(json_get(sc, "checkpoints")->array.count, 16);

   err = tool_error(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   ck_assert_ptr_nonnull(strstr(err, "16 checkpoints"));
   ck_assert_ptr_nonnull(strstr(err, "nvc_sim_checkpoint_delete"));

   sc = structured(sim_tool(&f, "nvc_sim_checkpoint_delete", id,
                            "\"checkpoint_id\":2"));
   ck_assert_int_eq(int_field(sc, "checkpoints"), 15);
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));

   // A restore is a simulation like any other: four at most
   char ids[4][40];
   for (int i = 0; i < 3; i++) {
      sc = structured(sim_tool(&f, "nvc_sim_restore", id,
                               "\"checkpoint_id\":3"));
      snprintf(ids[i], sizeof(ids[i]), "%s", str_field(sc, "sim_id"));
   }

   err = tool_error(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":3"));
   ck_assert_ptr_nonnull(strstr(err, "4 simulations are already open"));
   ck_assert_ptr_nonnull(strstr(err, "nvc_sim_close"));

   // Closing one makes room
   sim_tool(&f, "nvc_sim_close", ids[0], "");
   structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":3"));

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_restore_failed)
{
   fixture_t f;
   const char *names[] = { "fail.vhd" }, *sources[] = { fail_src };
   const char *tops[] = { "failing" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "failing", id);

   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"15 ns\"");
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   const int64_t ck_fs = int_field(sc, "now_fs");
   ck_assert_int_gt(ck_fs, 0);
   ck_assert_int_le(ck_fs, 15000000);

   sc = structured(sim_tool(&f, "nvc_sim_run", id, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "terminated");
   ck_assert_int_eq(int_field(sc, "now_fs"), 30000000);

   // A copy of the ended simulation could not run
   const char *err = tool_error(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   ck_assert_ptr_nonnull(strstr(err, "has ended"));
   ck_assert_ptr_nonnull(strstr(err, "nvc_sim_restore"));

   // The earlier one still works and fails in the same way
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"at\":\"20 ns\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), ck_fs);
   char rid[40];
   snprintf(rid, sizeof(rid), "%s", str_field(sc, "sim_id"));

   sc = structured(sim_tool(&f, "nvc_sim_status", rid, ""));
   ck_assert_str_eq(str_field(sc, "state"), "idle");
   ck_assert_str_eq(signal_value(&f, rid, "/x"), "7");

   sc = structured(sim_tool(&f, "nvc_sim_run", rid, ""));
   ck_assert_str_eq(str_field(sc, "stopped_because"), "terminated");
   ck_assert_int_eq(int_field(sc, "now_fs"), 30000000);
   const json_t *msgs = json_get(sc, "messages");
   ck_assert_int_eq(msgs->array.count, 1);
   ck_assert_ptr_nonnull(strstr(str_field(json_at(msgs, 0), "message"),
                                "giving up"));

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_checkpoint_wave)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   const json_t *sc = structured(sim_tool(&f, "nvc_wave_start", id,
                                          "\"include\":[\"/dut/*\"],"
                                          "\"name\":\"orig\""));
   char *wave_file = xstrdup(str_field(sc, "file"));

   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"100 ns\","
            "\"pause_on\":\"never\"");
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   sim_tool(&f, "nvc_sim_run", id, "\"until\":\"300 ns\","
            "\"pause_on\":\"never\"");

   // The restored simulation has no file recording of its own, and says so
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":1"));
   ck_assert_ptr_nonnull(strstr(str_field(sc, "hint"), wave_file));
   char rid[40];
   snprintf(rid, sizeof(rid), "%s", str_field(sc, "sim_id"));

   sc = structured(sim_tool(&f, "nvc_sim_status", rid, ""));
   const json_t *files = json_get(sc, "files");
   ck_assert(!bool_field(json_get(files, "wave"), "active"));
   ck_assert(bool_field(json_get(files, "memory"), "active"));

   // The in-memory recording carries on from the checkpoint
   sim_tool(&f, "nvc_sim_run", rid, "\"until\":\"300 ns\","
            "\"pause_on\":\"never\"");
   sc = structured(sim_tool(&f, "nvc_wave_get", rid,
                            "\"paths\":[\"/dut/cnt\"],\"radix\":\"decimal\","
                            "\"from\":\"50 ns\",\"until\":\"300 ns\""));
   const json_t *sig = json_at(json_get(sc, "signals"), 0);
   ck_assert_int_gt(json_get(sig, "changes")->array.count, 10);

   // A file recording can be started in the new simulation
   sim_tool(&f, "nvc_wave_stop", rid, "");
   sc = structured(sim_tool(&f, "nvc_wave_start", rid, "\"name\":\"again\""));
   ck_assert_ptr_nonnull(strstr(str_field(sc, "file"), rid));
   sim_tool(&f, "nvc_wave_stop", rid, "");

   // The original recording is untouched, even by the end of the other
   // simulation, and finishes into a readable file
   sim_tool(&f, "nvc_sim_close", rid, "");
   sc = structured(sim_tool(&f, "nvc_wave_stop", id, ""));
   ck_assert_int_gt(int_field(sc, "bytes"), 0);

   char *args = xasprintf("{\"file\":\"%s\"}", wave_file);
   sc = structured(tool(f.mp, f.srv, &f.sink, "nvc_wave_summary", args));
   ck_assert_int_eq(int_field(sc, "end_fs"), 300000000);
   free(args);

   free(wave_file);
   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_checkpoint_auto)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);

   // A foreground run takes none whatever the setting
   sim_tool(&f, "nvc_sim_run", id, "\"duration\":\"10 ns\","
            "\"auto_checkpoint_s\":0.01,\"pause_on\":\"never\"");
   const json_t *sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_ptr_null(json_get(sc, "checkpoints"));

   // A checkpoint every 50 ms for two seconds is far more than the eight
   // that are kept
   sim_tool(&f, "nvc_sim_run", id, "\"background\":true,"
            "\"auto_checkpoint_s\":0.05,\"pause_on\":\"never\"");

   size_t most = 0;
   for (int i = 0; i < 5; i++) {
      sc = structured(sim_tool(&f, "nvc_sim_wait", id, "\"timeout_ms\":400"));
      ck_assert_str_eq(str_field(sc, "state"), "running");
      const json_t *list = json_get(sc, "checkpoints");
      ck_assert_ptr_nonnull(list);
      most = MAX(most, list->array.count);
      ck_assert_int_le(list->array.count, 8);
   }

   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const json_t *list = json_get(sc, "checkpoints");
   ck_assert_int_eq(list->array.count, 8);
   ck_assert_int_eq(most, 8);

   int64_t last = -1;
   for (size_t i = 0; i < list->array.count; i++) {
      ck_assert(bool_field(json_at(list, i), "auto"));
      const int64_t t = int_field(json_at(list, i), "time_fs");
      ck_assert_int_ge(t, last);
      last = t;
   }

   // Thinning keeps the first one taken
   ck_assert_int_eq(int_field(json_at(list, 0), "id"), 1);

   // One of them can be restored while the original keeps running
   const int64_t picked = int_field(json_at(list, 3), "id");
   char *extra = xasprintf("\"checkpoint_id\":%"PRIi64, picked);
   sc = structured(sim_tool(&f, "nvc_sim_restore", id, extra));
   free(extra);
   ck_assert_int_eq(int_field(sc, "now_fs"),
                    int_field(json_at(list, 3), "time_fs"));

   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_str_eq(str_field(sc, "state"), "running");

   // One taken by hand in the middle of the run is never thinned away
   sc = structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   const int64_t manual = int_field(sc, "checkpoint_id");
   ck_assert_int_gt(manual, 8);
   sleep(1);
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   list = json_get(sc, "checkpoints");
   bool found = false;
   for (size_t i = 0; i < list->array.count; i++)
      found |= int_field(json_at(list, i), "id") == manual;
   ck_assert(found);
   ck_assert_int_le(list->array.count, 9);

   // With them switched off there are no more
   sim_tool(&f, "nvc_sim_stop", id, "");
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   const size_t before = json_get(sc, "checkpoints")->array.count;
   sim_tool(&f, "nvc_sim_run", id, "\"background\":true,"
            "\"auto_checkpoint_s\":0,\"pause_on\":\"never\"");
   usleep(300000);
   sim_tool(&f, "nvc_sim_stop", id, "");
   sc = structured(sim_tool(&f, "nvc_sim_status", id, ""));
   ck_assert_int_eq(json_get(sc, "checkpoints")->array.count, before);

   fixture_close(&f);
}
END_TEST

START_TEST(test_sim_checkpoint_cleanup)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);
   const pid_t orig = worker_pid(&f, id);

   const json_t *sc = structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   const pid_t snap1 = int_field(sc, "pid");
   sc = structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));
   const pid_t snap2 = int_field(sc, "pid");
   ck_assert_int_ne(snap1, snap2);
   ck_assert_int_eq(kill(snap1, 0), 0);
   ck_assert_int_eq(kill(snap2, 0), 0);

   // Deleting one ends its process and no other
   sim_tool(&f, "nvc_sim_checkpoint_delete", id, "\"checkpoint_id\":1");
   ck_assert(wait_gone(snap1));
   ck_assert_int_eq(kill(snap2, 0), 0);

   sc = structured(sim_tool(&f, "nvc_sim_restore", id, "\"checkpoint_id\":2"));
   char rid[40];
   snprintf(rid, sizeof(rid), "%s", str_field(sc, "sim_id"));
   const pid_t restored = worker_pid(&f, rid);
   ck_assert_int_ne(restored, orig);

   // The restored worker has a process group of its own, so stopping one
   // never stops the other
   ck_assert_int_eq(getpgid(restored), restored);
   ck_assert_int_eq(getpgid(orig), orig);

   // A checkpoint of the restored simulation is a copy of that worker
   sc = structured(sim_tool(&f, "nvc_sim_checkpoint", rid, ""));
   const pid_t snap3 = int_field(sc, "pid");
   ck_assert_int_eq(getpgid(snap3), restored);

   // ... which can be restored in turn, and runs
   sc = structured(sim_tool(&f, "nvc_sim_restore", rid, "\"checkpoint_id\":1"));
   char rid2[40];
   snprintf(rid2, sizeof(rid2), "%s", str_field(sc, "sim_id"));
   const pid_t restored2 = worker_pid(&f, rid2);
   sc = structured(sim_tool(&f, "nvc_sim_run", rid2, "\"duration\":\"10 ns\","
                            "\"pause_on\":\"never\""));
   ck_assert_int_eq(int_field(sc, "now_fs"), 10000000);

   // Closing the server leaves nothing behind
   fixture_close(&f);

   ck_assert(wait_gone(orig));
   ck_assert(wait_gone(snap2));
   ck_assert(wait_gone(restored));
   ck_assert(wait_gone(snap3));
   ck_assert(wait_gone(restored2));
}
END_TEST

START_TEST(test_sim_checkpoint_worker_death)
{
   fixture_t f;
   const char *names[] = { "tb.vhd" }, *sources[] = { tb_src };
   const char *tops[] = { "tb" };
   fixture_open(&f, names, sources, 1, tops, 1);

   char id[40];
   open_sim(&f, "tb", id);
   const pid_t pid = worker_pid(&f, id);
   structured(sim_tool(&f, "nvc_sim_checkpoint", id, ""));

   // The copy must not keep the control socket open: the supervisor
   // has to notice at once that the original has gone
   ck_assert_int_eq(kill(pid, SIGKILL), 0);

   const uint64_t start = get_timestamp_us();
   const char *err = tool_error(sim_tool(&f, "nvc_signal_get", id,
                                         "\"paths\":[\"/clk\"]"));
   ck_assert_ptr_nonnull(strstr(err, "terminated by signal 9"));
   ck_assert_int_lt(get_timestamp_us() - start, 2000000);

   fixture_close(&f);
}
END_TEST

Suite *get_mcp_tests(void)
{
   Suite *s = suite_create("mcp");

   TCase *tc = nvc_unit_test();
   tcase_add_test(tc, test_json_parse);
   tcase_add_test(tc, test_json_errors);
   tcase_add_test(tc, test_json_limits);
   tcase_add_test(tc, test_json_writer);
   tcase_add_test(tc, test_schema);
   tcase_add_test(tc, test_cond);
   tcase_add_test(tc, test_proto_modern);
   tcase_add_test(tc, test_proto_errors);
   tcase_add_test(tc, test_proto_legacy);
   tcase_add_test(tc, test_analyse_real);
   tcase_add_test(tc, test_analyse_bad_arguments);
   tcase_add_test(tc, test_analyse_worker_failures);
   tcase_add_test(tc, test_cancel_running);
   tcase_add_test(tc, test_cancel_queued);
   tcase_add_test(tc, test_input_framing);
   tcase_add_test(tc, test_input_too_large);
   tcase_add_test(tc, test_workspace_list);
   tcase_add_test(tc, test_library_and_elaborate);
   tcase_add_test(tc, test_sim_lifecycle);
   tcase_add_test(tc, test_sim_run_modes);
   tcase_add_test(tc, test_sim_trace);
   tcase_add_test(tc, test_sim_trace_overflow);
   tcase_add_test(tc, test_sim_hang);
   tcase_add_test(tc, test_sim_fake_workers);
   tcase_add_test(tc, test_sim_not_elaborated);
   tcase_add_test(tc, test_sim_cancel_run);
   tcase_add_test(tc, test_sim_progress);
   tcase_add_test(tc, test_sim_background);
   tcase_add_test(tc, test_sim_waves);
   tcase_add_test(tc, test_sim_wait_cancel);
   tcase_add_test(tc, test_sim_files_misc);
   tcase_add_test(tc, test_sim_log_limit);
   tcase_add_test(tc, test_sim_busy);
   tcase_add_test(tc, test_sim_live_trace);
   tcase_add_test(tc, test_sim_queries);
   tcase_add_test(tc, test_sim_checkpoint);
   tcase_add_test(tc, test_sim_restore_files);
   tcase_add_test(tc, test_sim_restore_errors);
   tcase_add_test(tc, test_sim_restore_failed);
   tcase_add_test(tc, test_sim_checkpoint_wave);
   tcase_add_test(tc, test_sim_checkpoint_auto);
   tcase_add_test(tc, test_sim_checkpoint_cleanup);
   tcase_add_test(tc, test_sim_checkpoint_worker_death);
   tcase_set_timeout(tc, 60);
   suite_add_tcase(s, tc);

   return s;
}
