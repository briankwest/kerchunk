/*
 * kerchunk_resp.c — Structured response serialization
 *
 * Handlers populate a response object with typed key-value pairs.
 * Both JSON and human-readable text are built simultaneously.
 * The socket layer picks the format based on client mode.
 */

#include "kerchunk.h"
#include <stdio.h>
#include <string.h>

void resp_init(kerchunk_resp_t *r)
{
    r->tlen = 0;
    r->text[0] = '\0';
    r->jlen = 0;
    r->jlen = snprintf(r->json, RESP_MAX, "{");
    r->jfirst = 1;
}

/* JSON key prefix: comma separator if not first field */
static void json_key(kerchunk_resp_t *r, const char *key)
{
    if (!r->jfirst)
        r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, ",");
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "\"%s\":", key);
    r->jfirst = 0;
}

/* Capitalize first letter for text display */
static void text_key(char *out, size_t max, const char *key)
{
    snprintf(out, max, "%s", key);
    if (out[0] >= 'a' && out[0] <= 'z')
        out[0] -= 32;
    /* Replace underscores with spaces for readability */
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

void resp_str(kerchunk_resp_t *r, const char *key, const char *val)
{
    if (!val) val = "";

    /* JSON: "key":"value" (with escaping) */
    json_key(r, key);
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "\"");
    for (const char *p = val; *p && r->jlen < RESP_MAX - 6; p++) {
        switch (*p) {
        case '"':  r->json[r->jlen++] = '\\'; r->json[r->jlen++] = '"'; break;
        case '\\': r->json[r->jlen++] = '\\'; r->json[r->jlen++] = '\\'; break;
        case '\n': r->json[r->jlen++] = '\\'; r->json[r->jlen++] = 'n'; break;
        case '\r': r->json[r->jlen++] = '\\'; r->json[r->jlen++] = 'r'; break;
        case '\t': r->json[r->jlen++] = '\\'; r->json[r->jlen++] = 't'; break;
        default:
            if ((unsigned char)*p < 0x20) {
                r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen,
                                    "\\u%04x", (unsigned char)*p);
            } else {
                r->json[r->jlen++] = *p;
            }
            break;
        }
    }
    r->json[r->jlen] = '\0';
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "\"");

    /* Text: Key: value */
    char label[64];
    text_key(label, sizeof(label), key);
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen,
                         "%-16s %s\n", label, val);
}

void resp_int(kerchunk_resp_t *r, const char *key, int val)
{
    json_key(r, key);
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "%d", val);

    char label[64];
    text_key(label, sizeof(label), key);
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen,
                         "%-16s %d\n", label, val);
}

void resp_int64(kerchunk_resp_t *r, const char *key, int64_t val)
{
    json_key(r, key);
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen,
                         "%lld", (long long)val);

    char label[64];
    text_key(label, sizeof(label), key);
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen,
                         "%-16s %lld\n", label, (long long)val);
}

void resp_bool(kerchunk_resp_t *r, const char *key, int val)
{
    json_key(r, key);
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen,
                         "%s", val ? "true" : "false");

    char label[64];
    text_key(label, sizeof(label), key);
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen,
                         "%-16s %s\n", label, val ? "yes" : "no");
}

void resp_float(kerchunk_resp_t *r, const char *key, double val)
{
    json_key(r, key);
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "%.1f", val);

    char label[64];
    text_key(label, sizeof(label), key);
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen,
                         "%-16s %.1f\n", label, val);
}

void resp_json_raw(kerchunk_resp_t *r, const char *fragment)
{
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "%s", fragment);
}

void resp_text_raw(kerchunk_resp_t *r, const char *fragment)
{
    r->tlen += snprintf(r->text + r->tlen, RESP_MAX - r->tlen, "%s", fragment);
}

void resp_finish(kerchunk_resp_t *r)
{
    r->jlen += snprintf(r->json + r->jlen, RESP_MAX - r->jlen, "}");
}
