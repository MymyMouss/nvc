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
#include "mcp/mcp-json.h"
#include "mcp/mcp-schema.h"
#include "mcp/mcp-server.h"

#include <ftw.h>
#include <limits.h>
#include <pthread.h>
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

Suite *get_mcp_tests(void)
{
   Suite *s = suite_create("mcp");

   TCase *tc = nvc_unit_test();
   tcase_add_test(tc, test_json_parse);
   tcase_add_test(tc, test_json_errors);
   tcase_add_test(tc, test_json_limits);
   tcase_add_test(tc, test_json_writer);
   tcase_add_test(tc, test_schema);
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
   suite_add_tcase(s, tc);

   return s;
}
