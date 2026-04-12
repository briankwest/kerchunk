/*
 * mod_weather.c — Weather announcement module
 *
 * Fetches current conditions + forecast from weatherapi.com via libcurl.
 * Two announcements:
 *   *93# / "weather now"      — current conditions
 *   *94# / "weather forecast"  — today's forecast (high/low, rain chance, humidity)
 *
 * Config: [weather] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <curl/curl.h>

#define LOG_MOD "weather"
#define DTMF_EVT_WEATHER   (KERCHEVT_CUSTOM + 8)
#define DTMF_EVT_FORECAST  (KERCHEVT_CUSTOM + 9)

static kerchunk_core_t *g_core;

/* ---- config ---- */
static char g_api_key[128];
static char g_location[64]    = "74104";
static int  g_auto_announce   = 0;         /* off by default (FCC 95.1733) */
static int  g_interval_ms     = 1800000;   /* 30 min */
static int  g_announce_temp   = 1;
static int  g_announce_cond   = 1;
static int  g_announce_wind   = 1;
static char g_sounds_dir[256] = "/etc/kerchunk/sounds";

static int  g_timer = -1;

/* ---- cached current conditions ---- */
static float g_temp_f;
static float g_wind_mph;
static char  g_wind_dir[16];
static int   g_humidity;
static char  g_condition[64];
static int   g_valid;

/* ---- cached forecast ---- */
static float g_fc_high_f;
static float g_fc_low_f;
static int   g_fc_rain_pct;
static int   g_fc_snow_pct;
static int   g_fc_humidity;
static char  g_fc_condition[64];
static int   g_fc_valid;

/* ================================================================== */
/*  libcurl helpers                                                    */
/* ================================================================== */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    curl_buf_t *buf = ud;
    size_t total = size * nmemb;
    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < buf->len + total + 1)
            new_cap = buf->len + total + 1;
        char *p = realloc(buf->data, new_cap);
        if (!p) return 0;
        buf->data = p;
        buf->cap  = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* ================================================================== */
/*  JSON parsing helpers                                              */
/* ================================================================== */

/* Extract a float after "key": */
static int json_float(const char *json, const char *key, float *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    *out = (float)strtod(p + strlen(needle), NULL);
    return 0;
}

/* Extract an int after "key": */
static int json_int(const char *json, const char *key, int *out)
{
    float v;
    if (json_float(json, key, &v) < 0) return -1;
    *out = (int)v;
    return 0;
}

/* Extract a quoted string after "key": "value" */
static int json_str(const char *json, const char *key, char *out, size_t max)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= max) len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ================================================================== */
/*  JSON parsing                                                      */
/* ================================================================== */

static int weather_parse_json(const char *json)
{
    /* --- current conditions (in "current" object) --- */
    const char *cur = strstr(json, "\"current\"");
    if (!cur) return -1;

    if (json_float(cur, "temp_f", &g_temp_f) < 0) return -1;
    json_float(cur, "wind_mph", &g_wind_mph);
    json_str(cur, "wind_dir", g_wind_dir, sizeof(g_wind_dir));
    json_int(cur, "humidity", &g_humidity);

    /* condition.text — find the first "condition" after "current" */
    const char *cond = strstr(cur, "\"condition\"");
    if (cond)
        json_str(cond, "text", g_condition, sizeof(g_condition));

    g_valid = 1;

    /* --- forecast (in "forecast" → "forecastday" → [0] → "day") --- */
    const char *fc = strstr(json, "\"forecastday\"");
    if (fc) {
        const char *day = strstr(fc, "\"day\"");
        if (day) {
            json_float(day, "maxtemp_f", &g_fc_high_f);
            json_float(day, "mintemp_f", &g_fc_low_f);
            json_int(day, "daily_chance_of_rain", &g_fc_rain_pct);
            json_int(day, "daily_chance_of_snow", &g_fc_snow_pct);
            json_int(day, "avghumidity", &g_fc_humidity);

            const char *fc_cond = strstr(day, "\"condition\"");
            if (fc_cond)
                json_str(fc_cond, "text", g_fc_condition, sizeof(g_fc_condition));

            g_fc_valid = 1;
        }
    }

    return 0;
}

/* ================================================================== */
/*  HTTP fetch                                                        */
/* ================================================================== */

static int weather_fetch(void)
{
    if (g_api_key[0] == '\0') {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "no API key configured");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=1&aqi=no",
             g_api_key, g_location);

    curl_buf_t buf = { .data = malloc(8192), .len = 0, .cap = 8192 };
    if (!buf.data) {
        curl_easy_cleanup(curl);
        return -1;
    }
    buf.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "fetch failed: %s",
                     curl_easy_strerror(res));
        free(buf.data);
        return -1;
    }

    int rc = weather_parse_json(buf.data);
    free(buf.data);

    if (rc == 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                     "current: %.0fF wind %s %.0f mph %s | "
                     "forecast: hi %.0f lo %.0f rain %d%% snow %d%% humidity %d%%",
                     g_temp_f, g_wind_dir, g_wind_mph, g_condition,
                     g_fc_high_f, g_fc_low_f, g_fc_rain_pct,
                     g_fc_snow_pct, g_fc_humidity);
    }
    return rc;
}

/* ================================================================== */
/*  Number-to-speech                                                  */
/* ================================================================== */

static void speak_wav(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.wav", g_sounds_dir, name);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
}

/*
 * Speak an integer using concatenated WAV files:
 *   0-10  → num_N.wav
 *   11-19 → num_10.wav + num_(N-10).wav
 *   20-99 → num_X0.wav + num_(N%10).wav
 *   100+  → num_100.wav + remainder
 */
static void speak_number(int n)
{
    if (n < 0) {
        speak_wav("weather/wx_minus");
        n = -n;
    }
    if (n > 199) n = 199;

    if (n >= 100) {
        speak_wav("numbers/num_100");
        n -= 100;
        if (n == 0) return;
    }

    if (n >= 20) {
        char name[32];
        snprintf(name, sizeof(name), "numbers/num_%d", (n / 10) * 10);
        speak_wav(name);
        n %= 10;
        if (n == 0) return;
    } else if (n >= 11) {
        speak_wav("numbers/num_10");
        n -= 10;
    }

    char name[32];
    snprintf(name, sizeof(name), "numbers/num_%d", n);
    speak_wav(name);
}

/* ================================================================== */
/*  Condition → WAV mapping                                           */
/* ================================================================== */

static const char *map_condition(const char *cond)
{
    if (strstr(cond, "Sunny"))      return "weather/wx_sunny";
    if (strstr(cond, "Clear"))      return "weather/wx_clear";
    if (strstr(cond, "Partly"))     return "weather/wx_partly_cloudy";
    if (strstr(cond, "Overcast"))   return "weather/wx_overcast";
    if (strstr(cond, "Cloudy"))     return "weather/wx_cloudy";
    if (strstr(cond, "Thunder"))    return "weather/wx_thunderstorm";
    if (strstr(cond, "Rain") || strstr(cond, "Drizzle"))
        return "weather/wx_rainy";
    if (strstr(cond, "Snow") || strstr(cond, "Sleet") || strstr(cond, "Ice"))
        return "weather/wx_snowy";
    if (strstr(cond, "Fog") || strstr(cond, "Mist"))
        return "weather/wx_foggy";
    return NULL;
}

/* ================================================================== */
/*  Announcement builders                                             */
/* ================================================================== */

/* Expand compass abbreviations to spoken words */
static const char *expand_wind_dir(const char *abbr)
{
    if (!abbr || !abbr[0]) return NULL;

    /* Exact matches — most specific first */
    if (strcmp(abbr, "NNE") == 0) return "north northeast";
    if (strcmp(abbr, "NNW") == 0) return "north northwest";
    if (strcmp(abbr, "SSE") == 0) return "south southeast";
    if (strcmp(abbr, "SSW") == 0) return "south southwest";
    if (strcmp(abbr, "ENE") == 0) return "east northeast";
    if (strcmp(abbr, "ESE") == 0) return "east southeast";
    if (strcmp(abbr, "WNW") == 0) return "west northwest";
    if (strcmp(abbr, "WSW") == 0) return "west southwest";
    if (strcmp(abbr, "NE") == 0)  return "northeast";
    if (strcmp(abbr, "NW") == 0)  return "northwest";
    if (strcmp(abbr, "SE") == 0)  return "southeast";
    if (strcmp(abbr, "SW") == 0)  return "southwest";
    if (strcmp(abbr, "N") == 0)   return "north";
    if (strcmp(abbr, "S") == 0)   return "south";
    if (strcmp(abbr, "E") == 0)   return "east";
    if (strcmp(abbr, "W") == 0)   return "west";

    return abbr;  /* Unknown — pass through as-is */
}

/* *93# — current conditions */
static void weather_announce(void)
{
    if (!g_valid) return;

    /* TTS path: build natural sentence */
    if (g_core->tts_speak) {
        char text[512];
        int off = 0;
        off += snprintf(text + off, sizeof(text) - off, "Current weather.");

        if (g_announce_cond && g_condition[0])
            off += snprintf(text + off, sizeof(text) - off, " %s.", g_condition);

        if (g_announce_temp)
            off += snprintf(text + off, sizeof(text) - off,
                            " Temperature %d degrees.", (int)roundf(g_temp_f));

        if (g_announce_wind) {
            const char *dir = expand_wind_dir(g_wind_dir);
            if (dir)
                off += snprintf(text + off, sizeof(text) - off,
                                " Wind from the %s at %d miles per hour.",
                                dir, (int)roundf(g_wind_mph));
            else
                off += snprintf(text + off, sizeof(text) - off,
                                " Wind %d miles per hour.",
                                (int)roundf(g_wind_mph));
        }
        (void)off;
        g_core->tts_speak(text, KERCHUNK_PRI_ELEVATED);
    } else {
        /* WAV fallback */
        speak_wav("weather/wx_current_weather");
        if (g_announce_cond) {
            const char *cw = map_condition(g_condition);
            if (cw) speak_wav(cw);
        }
        if (g_announce_temp) {
            speak_wav("weather/wx_temperature");
            speak_number((int)roundf(g_temp_f));
            speak_wav("weather/wx_degrees");
        }
        if (g_announce_wind) {
            speak_wav("weather/wx_wind");
            if (g_wind_dir[0] != '\0') {
                char dir_wav[48];
                snprintf(dir_wav, sizeof(dir_wav), "weather/wx_dir_%s", g_wind_dir);
                for (char *p = dir_wav; *p; p++)
                    if (*p >= 'A' && *p <= 'Z') *p += 32;
                speak_wav(dir_wav);
            }
            speak_number((int)roundf(g_wind_mph));
            speak_wav("weather/wx_miles_per_hour");
        }
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "weather", .description = "current conditions" } };
    kerchevt_fire(&ae);
}

/* *94# — today's forecast */
static void forecast_announce(void)
{
    if (!g_fc_valid) return;

    /* TTS path */
    if (g_core->tts_speak) {
        char text[512];
        int off = 0;
        off += snprintf(text + off, sizeof(text) - off,
                        "Today's forecast. %s.",
                        g_fc_condition[0] ? g_fc_condition : "");
        off += snprintf(text + off, sizeof(text) - off,
                        " High %d, low %d degrees.",
                        (int)roundf(g_fc_high_f), (int)roundf(g_fc_low_f));
        if (g_fc_rain_pct > 0)
            off += snprintf(text + off, sizeof(text) - off,
                            " %d percent chance of rain.", g_fc_rain_pct);
        if (g_fc_snow_pct > 0)
            off += snprintf(text + off, sizeof(text) - off,
                            " %d percent chance of snow.", g_fc_snow_pct);
        off += snprintf(text + off, sizeof(text) - off,
                        " Humidity %d percent.", g_fc_humidity);
        (void)off;
        g_core->tts_speak(text, KERCHUNK_PRI_ELEVATED);
    } else {
        /* WAV fallback */
        speak_wav("weather/wx_forecast");
        const char *cw = map_condition(g_fc_condition);
        if (cw) speak_wav(cw);
        speak_wav("weather/wx_high");
        speak_number((int)roundf(g_fc_high_f));
        speak_wav("weather/wx_low");
        speak_number((int)roundf(g_fc_low_f));
        speak_wav("weather/wx_degrees");
        if (g_fc_rain_pct > 0) {
            speak_wav("weather/wx_chance_of_rain");
            speak_number(g_fc_rain_pct);
            speak_wav("weather/wx_percent");
        }
        if (g_fc_snow_pct > 0) {
            speak_wav("weather/wx_chance_of_snow");
            speak_number(g_fc_snow_pct);
            speak_wav("weather/wx_percent");
        }
        speak_wav("weather/wx_humidity");
        speak_number(g_fc_humidity);
        speak_wav("weather/wx_percent");
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "weather", .description = "forecast" } };
    kerchevt_fire(&ae);
}

/* ================================================================== */
/*  Event handlers                                                    */
/* ================================================================== */

static void weather_update(void *ud)
{
    (void)ud;

    /* Suppress during emergency mode */
    if (kerchunk_core_get_emergency())
        return;

    if (weather_fetch() == 0)
        weather_announce();
}

static void on_dtmf_weather(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "current weather via DTMF");
    weather_update(NULL);
}

static void on_dtmf_forecast(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "forecast via DTMF");
    if (weather_fetch() == 0)
        forecast_announce();
}

/* ================================================================== */
/*  Module lifecycle                                                  */
/* ================================================================== */

static int weather_load(kerchunk_core_t *core)
{
    g_core  = core;
    g_valid = 0;
    g_fc_valid = 0;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (core->dtmf_register) {
        core->dtmf_register("93", 8, "Weather report",   "weather_report");
        core->dtmf_register("94", 9, "Weather forecast", "weather_forecast");
    }

    core->subscribe(DTMF_EVT_WEATHER,  on_dtmf_weather, NULL);
    core->subscribe(DTMF_EVT_FORECAST, on_dtmf_forecast, NULL);
    return 0;
}

static int weather_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "weather", "api_key");
    if (v) snprintf(g_api_key, sizeof(g_api_key), "%s", v);
    if (g_api_key[0] == '\0' || strcmp(g_api_key, "YOUR_KEY_HERE") == 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "*** weather API key not configured — set api_key in [weather] section ***");
        g_api_key[0] = '\0';
    }

    v = kerchunk_config_get(cfg, "weather", "location");
    if (v) snprintf(g_location, sizeof(g_location), "%s", v);

    g_interval_ms = kerchunk_config_get_duration_ms(cfg, "weather", "interval", 1800000);

    v = kerchunk_config_get(cfg, "weather", "announce_temp");
    g_announce_temp = (!v || strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "weather", "announce_conditions");
    g_announce_cond = (!v || strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "weather", "announce_wind");
    g_announce_wind = (!v || strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "weather", "auto_announce");
    g_auto_announce = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "general", "sounds_dir");
    if (v) snprintf(g_sounds_dir, sizeof(g_sounds_dir), "%s", v);

    /* Start periodic timer only if auto_announce is on */
    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
    g_timer = -1;
    if (g_auto_announce && g_api_key[0] != '\0' && g_interval_ms > 0)
        g_timer = g_core->timer_create(g_interval_ms, 1, weather_update, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "location=%s interval=%dms auto_announce=%d temp=%d cond=%d wind=%d",
                g_location, g_interval_ms, g_auto_announce,
                g_announce_temp, g_announce_cond, g_announce_wind);

    /* Fetch weather on startup so dashboard has data immediately */
    if (g_api_key[0] != '\0')
        weather_fetch();

    return 0;
}

static void weather_unload(void)
{
    if (g_core->dtmf_unregister) {
        g_core->dtmf_unregister("93");
        g_core->dtmf_unregister("94");
    }
    g_core->unsubscribe(DTMF_EVT_WEATHER,  on_dtmf_weather);
    g_core->unsubscribe(DTMF_EVT_FORECAST, on_dtmf_forecast);
    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
    curl_global_cleanup();
}

/* ---- CLI ---- */

static int cli_weather(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 2 && strcmp(argv[1], "now") == 0) {
        if (weather_fetch() == 0) {
            weather_announce();
            resp_bool(r, "ok", 1);
            resp_str(r, "action", "announced");
        } else {
            resp_str(r, "error", "Weather fetch failed");
        }
    } else if (argc >= 2 && strcmp(argv[1], "forecast") == 0) {
        if (weather_fetch() == 0) {
            forecast_announce();
            resp_bool(r, "ok", 1);
            resp_str(r, "action", "forecast_announced");
        } else {
            resp_str(r, "error", "Forecast fetch failed");
        }
    } else {
        resp_str(r, "location", g_location);
        resp_int(r, "interval_ms", g_interval_ms);
        resp_str(r, "api_key", g_api_key[0] ? "configured" : "NOT SET");
        resp_bool(r, "valid", g_valid);
        if (g_valid) {
            resp_float(r, "temp_f", (double)g_temp_f);
            resp_str(r, "wind_dir", g_wind_dir);
            resp_float(r, "wind_mph", (double)g_wind_mph);
            resp_str(r, "condition", g_condition);
            resp_int(r, "humidity", g_humidity);
        }
        if (g_fc_valid) {
            resp_float(r, "forecast_high_f", (double)g_fc_high_f);
            resp_float(r, "forecast_low_f", (double)g_fc_low_f);
            resp_int(r, "forecast_rain_pct", g_fc_rain_pct);
            resp_int(r, "forecast_snow_pct", g_fc_snow_pct);
            resp_int(r, "forecast_humidity", g_fc_humidity);
        }
    }
    return 0;

usage:
    resp_text_raw(r, "Weather announcements via weatherapi.com\n\n"
        "  weather\n"
        "    Show cached weather data: location, API key status,\n"
        "    current conditions (temp, wind, humidity), and forecast.\n\n"
        "  weather now\n"
        "    Fetch current conditions and announce on-air.\n\n"
        "  weather forecast\n"
        "    Fetch today's forecast and announce on-air.\n"
        "    Includes high/low temps, rain/snow chance, humidity.\n\n"
        "    Fetches from weatherapi.com. Supports TTS or WAV fallback.\n"
        "    Auto-announce is off by default (FCC 95.1733).\n"
        "    DTMF *93# current conditions, *94# forecast.\n\n"
        "Config: [weather] api_key, location, interval, auto_announce,\n"
        "        announce_temp, announce_conditions, announce_wind\n"
        "DTMF:   *93# current, *94# forecast\n");
    resp_str(r, "error", "usage: weather [now|forecast]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "weather", .usage = "weather [now|forecast]", .description = "Weather announcements",
      .handler = cli_weather,
      .category = "Announcements", .ui_label = "Weather", .ui_type = CLI_UI_BUTTON,
      .ui_command = "weather now",
      .subcommands = "now,forecast" },
    { .name = "weather", .usage = "weather forecast", .description = "Weather forecast",
      .handler = cli_weather,
      .category = "Announcements", .ui_label = "Forecast", .ui_type = CLI_UI_BUTTON,
      .ui_command = "weather forecast" },
};

static kerchunk_module_def_t mod_weather = {
    .name             = "mod_weather",
    .version          = "1.0.0",
    .description      = "Weather announcements",
    .load             = weather_load,
    .configure        = weather_configure,
    .unload           = weather_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 2,
};

KERCHUNK_MODULE_DEFINE(mod_weather);
