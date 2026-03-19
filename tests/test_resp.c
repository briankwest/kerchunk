/*
 * test_resp.c — Unit tests for kerchunk_resp_t response system
 */

#include "kerchunk.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

void test_resp(void)
{
    /* 1. Init creates empty JSON object */
    test_begin("resp: init creates empty state");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        test_assert(r.json[0] == '{', "json doesn't start with {");
        test_assert(r.tlen == 0, "text not empty");
        test_assert(r.jfirst == 1, "jfirst not set");
        resp_finish(&r);
        test_assert(strstr(r.json, "}") != NULL, "no closing brace");
    }
    test_end();

    /* 2. String field */
    test_begin("resp: string field in both outputs");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_str(&r, "state", "IDLE");
        resp_finish(&r);
        test_assert(strstr(r.json, "\"state\":\"IDLE\"") != NULL, "json wrong");
        test_assert(strstr(r.text, "IDLE") != NULL, "text missing value");
    }
    test_end();

    /* 3. Integer field */
    test_begin("resp: integer field");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_int(&r, "count", 42);
        resp_finish(&r);
        test_assert(strstr(r.json, "\"count\":42") != NULL, "json wrong");
        test_assert(strstr(r.text, "42") != NULL, "text wrong");
    }
    test_end();

    /* 4. Boolean field */
    test_begin("resp: boolean field");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_bool(&r, "active", 1);
        resp_bool(&r, "pending", 0);
        resp_finish(&r);
        test_assert(strstr(r.json, "\"active\":true") != NULL, "true wrong");
        test_assert(strstr(r.json, "\"pending\":false") != NULL, "false wrong");
        test_assert(strstr(r.text, "yes") != NULL, "text true wrong");
        test_assert(strstr(r.text, "no") != NULL, "text false wrong");
    }
    test_end();

    /* 5. Float field */
    test_begin("resp: float field");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_float(&r, "duty", 3.5);
        resp_finish(&r);
        test_assert(strstr(r.json, "\"duty\":3.5") != NULL, "json wrong");
        test_assert(strstr(r.text, "3.5") != NULL, "text wrong");
    }
    test_end();

    /* 6. Multiple fields with comma separation */
    test_begin("resp: multiple fields comma-separated in JSON");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_str(&r, "a", "x");
        resp_int(&r, "b", 1);
        resp_bool(&r, "c", 0);
        resp_finish(&r);
        /* Should have commas between fields */
        test_assert(strstr(r.json, "\"a\":\"x\",\"b\":1,\"c\":false") != NULL,
                    "comma separation wrong");
    }
    test_end();

    /* 7. JSON raw injection */
    test_begin("resp: json_raw appends to JSON only");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_str(&r, "name", "test");
        if (!r.jfirst) resp_json_raw(&r, ",");
        resp_json_raw(&r, "\"items\":[1,2,3]");
        r.jfirst = 0;
        resp_finish(&r);
        test_assert(strstr(r.json, "\"items\":[1,2,3]") != NULL, "raw json missing");
        test_assert(strstr(r.text, "items") == NULL, "raw leaked to text");
    }
    test_end();

    /* 8. Text raw injection */
    test_begin("resp: text_raw appends to text only");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_text_raw(&r, "Custom dashboard\n");
        resp_finish(&r);
        test_assert(strstr(r.text, "Custom dashboard") != NULL, "text raw missing");
        test_assert(strstr(r.json, "Custom") == NULL, "text leaked to json");
    }
    test_end();

    /* 9. String escaping in JSON */
    test_begin("resp: JSON string escaping");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_str(&r, "msg", "hello \"world\"");
        resp_finish(&r);
        test_assert(strstr(r.json, "\\\"world\\\"") != NULL,
                    "quotes not escaped");
    }
    test_end();

    /* 10. int64 field */
    test_begin("resp: int64 field");
    {
        kerchunk_resp_t r;
        resp_init(&r);
        resp_int64(&r, "big", 1234567890123LL);
        resp_finish(&r);
        test_assert(strstr(r.json, "1234567890123") != NULL, "int64 wrong");
    }
    test_end();
}
