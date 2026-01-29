// Minimal iButton tester for Linux w1 + MQTT (Paho C) + tiny JSON (jsmn)
// Build deps: paho-mqtt3c, libc, jsmn.c/h (included in this project)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <limits.h>

#include "MQTTClient.h" // from Paho C (install dev package or link from SDK)
#include "jsmn.h"

#define MAX_DEVICES 32
#define MAX_ID_LEN 64
#define MAX_JSON 1024
#define MAX_TOPIC 256

typedef struct
{
    char broker[256];
    char client_id[128];
    char username[128];
    char password[128];
    char topic_cmd[128];
    char topic_state[128];
    int qos;
} mqtt_cfg_t;

typedef struct
{
    char devices_dir[256];
    char exclude[4][64];
    int exclude_count;
    char family_filter[8][4]; // e.g. {"01"}
    int family_count;
} w1_cfg_t;

typedef struct
{
    int default_timeout_ms;
    int debounce_ms;
} test_cfg_t;

typedef struct
{
    mqtt_cfg_t mqtt;
    w1_cfg_t w1;
    test_cfg_t test;
} app_cfg_t;

static app_cfg_t CFG;
static MQTTClient client;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}



enum
{
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3
};
static int g_log_level = LOG_INFO; // default INFO
static char g_config_path[PATH_MAX] = "/etc/ibutton-tester/config.json";

static void log_print(int level, const char *fmt, ...)
{
    if (level > g_log_level)
        return;
    static const char *L[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    const char *tag = (level >= 0 && level <= 3) ? L[level] : "LOG";
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

#define LOGE(...) log_print(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) log_print(LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_print(LOG_INFO, __VA_ARGS__)
#define LOGD(...) log_print(LOG_DEBUG, __VA_ARGS__)

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c PATH] [-v|-q|--log-level=N] [--dry-run]\n"
            "  -c, --config PATH     Path to config.json (default: %s)\n"
            "  -v                    Increase verbosity (INFO->DEBUG). Repeatable.\n"
            "  -q                    Quiet (set WARN level).\n"
            "  --log-level=N         0=ERROR,1=WARN,2=INFO,3=DEBUG\n"
            "  --dry-run             Load config, print status, then exit 0.\n"
            "  -h, --help            Show this help.\n",
            prog, g_config_path);
}

static int parse_args(int argc, char **argv, int *dry_run)
{
    *dry_run = 0;
    // Backward-compat: if argv[1] is a file path (no leading '-'), treat as config
    if (argc > 1 && argv[1][0] != '-')
    {
        snprintf(g_config_path, sizeof(g_config_path), "%s", argv[1]);
        LOGW("Deprecated: passing config path without -c. Use -c %s", g_config_path);
        return 0;
    }
    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))
        {
            print_usage(argv[0]);
            exit(0);
        }
        else if (!strcmp(a, "-v"))
        {
            if (g_log_level < LOG_DEBUG)
                g_log_level++;
        }
        else if (!strcmp(a, "-q"))
        {
            g_log_level = LOG_WARN;
        }
        else if (!strncmp(a, "--log-level=", 12))
        {
            int n = atoi(a + 12);
            if (n < 0)
                n = 0;
            if (n > 3)
                n = 3;
            g_log_level = n;
        }
        else if (!strcmp(a, "--log-level"))
        {
            if (i + 1 >= argc)
            {
                LOGE("--log-level requires a value");
                return -1;
            }
            int n = atoi(argv[++i]);
            if (n < 0)
                n = 0;
            if (n > 3)
                n = 3;
            g_log_level = n;
        }
        else if (!strcmp(a, "-c") || !strcmp(a, "--config"))
        {
            if (i + 1 >= argc)
            {
                LOGE("%s requires a path", a);
                return -1;
            }
            snprintf(g_config_path, sizeof(g_config_path), "%s", argv[++i]);
        }
        else if (!strncmp(a, "--config=", 9))
        {
            snprintf(g_config_path, sizeof(g_config_path), "%s", a + 9);
        }
        else if (!strcmp(a, "--dry-run"))
        {
            *dry_run = 1;
        }
        else
        {
            LOGE("Unknown option: %s", a);
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static void paho_trace_cb(enum MQTTCLIENT_TRACE_LEVELS level, char *message)
{
    LOGD("[PAHO] %s", message);
}

// --- Very small JSON helpers using jsmn ---
static int json_get_string(const char *js, jsmntok_t *toks, int ntok,
                           const char *key, char *out, size_t outlen)
{
    for (int i = 1; i < ntok; i++)
    {
        if (toks[i].type == JSMN_STRING &&
            (int)strlen(key) == toks[i].end - toks[i].start &&
            strncmp(js + toks[i].start, key, toks[i].end - toks[i].start) == 0)
        {
            // value tok is next
            if (i + 1 < ntok && toks[i + 1].type == JSMN_STRING)
            {
                int len = toks[i + 1].end - toks[i + 1].start;
                if ((size_t)len >= outlen)
                    len = (int)outlen - 1;
                memcpy(out, js + toks[i + 1].start, len);
                out[len] = 0;
                return 1;
            }
        }
    }
    return 0;
}

static int json_get_int(const char *js, jsmntok_t *toks, int ntok,
                        const char *key, int *out)
{
    char buf[32];
    if (!json_get_string(js, toks, ntok, key, buf, sizeof(buf)))
        return 0;
    *out = atoi(buf);
    return 1;
}

// Quick & tiny config parser for our known fields (expects valid JSON)
static int load_config(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "Config open %s: %s\n", path, strerror(errno));
        return -1;
    }
    LOGI("Loaded config from %s", path);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;

    // Defaults
    memset(&CFG, 0, sizeof(CFG));
    strcpy(CFG.mqtt.broker, "tcp://127.0.0.1:1883");
    strcpy(CFG.mqtt.client_id, "imx6ul-ibutton");
    strcpy(CFG.mqtt.topic_cmd, "board/ibutton/command");
    strcpy(CFG.mqtt.topic_state, "board/ibutton/state");
    CFG.mqtt.qos = 1;
    strcpy(CFG.w1.devices_dir, "/sys/bus/w1/devices");
    CFG.test.default_timeout_ms = 10000;
    CFG.test.debounce_ms = 100;

    jsmn_parser p;
    jsmn_init(&p);
    jsmntok_t toks[256];
    int ntok = jsmn_parse(&p, buf, strlen(buf), toks, 256);
    if (ntok < 1 || toks[0].type != JSMN_OBJECT)
    {
        fprintf(stderr, "Bad JSON in %s\n", path);
        return -1;
    }

    // mqtt.*
    json_get_string(buf, toks, ntok, "broker", CFG.mqtt.broker, sizeof(CFG.mqtt.broker));
    json_get_string(buf, toks, ntok, "client_id", CFG.mqtt.client_id, sizeof(CFG.mqtt.client_id));
    json_get_string(buf, toks, ntok, "username", CFG.mqtt.username, sizeof(CFG.mqtt.username));
    json_get_string(buf, toks, ntok, "password", CFG.mqtt.password, sizeof(CFG.mqtt.password));
    json_get_string(buf, toks, ntok, "topic_cmd", CFG.mqtt.topic_cmd, sizeof(CFG.mqtt.topic_cmd));
    json_get_string(buf, toks, ntok, "topic_state", CFG.mqtt.topic_state, sizeof(CFG.mqtt.topic_state));
    json_get_int(buf, toks, ntok, "qos", &CFG.mqtt.qos);

    // w1.*
    json_get_string(buf, toks, ntok, "devices_dir", CFG.w1.devices_dir, sizeof(CFG.w1.devices_dir));
    // arrays (simple: copy up to a few)
    // exclude
    {
        const char *k = "exclude";
        for (int i = 1; i < ntok; i++)
        {
            if (toks[i].type == JSMN_STRING &&
                (int)strlen(k) == toks[i].end - toks[i].start &&
                strncmp(buf + toks[i].start, k, toks[i].end - toks[i].start) == 0 &&
                i + 1 < ntok && toks[i + 1].type == JSMN_ARRAY)
            {
                int count = toks[i + 1].size;
                int idx = i + 2; // first element
                for (int j = 0; j < count && j < 4; j++)
                {
                    if (idx < ntok && toks[idx].type == JSMN_STRING)
                    {
                        int len = toks[idx].end - toks[idx].start;
                        if (len > 63)
                            len = 63;
                        memcpy(CFG.w1.exclude[j], buf + toks[idx].start, len);
                        CFG.w1.exclude[j][len] = 0;
                        CFG.w1.exclude_count++;
                    }
                    // advance to next element (naively)
                    idx++;
                }
                break;
            }
        }
    }
    // family_filter
    {
        const char *k = "family_filter";
        for (int i = 1; i < ntok; i++)
        {
            if (toks[i].type == JSMN_STRING &&
                (int)strlen(k) == toks[i].end - toks[i].start &&
                strncmp(buf + toks[i].start, k, toks[i].end - toks[i].start) == 0 &&
                i + 1 < ntok && toks[i + 1].type == JSMN_ARRAY)
            {
                int count = toks[i + 1].size;
                int idx = i + 2;
                for (int j = 0; j < count && j < 8; j++)
                {
                    if (idx < ntok && toks[idx].type == JSMN_STRING)
                    {
                        int len = toks[idx].end - toks[idx].start;
                        if (len > 3)
                            len = 3;
                        memcpy(CFG.w1.family_filter[j], buf + toks[idx].start, len);
                        CFG.w1.family_filter[j][len] = 0;
                        CFG.w1.family_count++;
                    }
                    idx++;
                }
                break;
            }
        }
    }

    // test.*
    json_get_int(buf, toks, ntok, "default_timeout_ms", &CFG.test.default_timeout_ms);
    json_get_int(buf, toks, ntok, "debounce_ms", &CFG.test.debounce_ms);

    return 0;
}

// ---- w1 helpers ----
static int is_excluded(const char *name)
{
    for (int i = 0; i < CFG.w1.exclude_count; i++)
    {
        if (strcmp(name, CFG.w1.exclude[i]) == 0)
            return 1;
    }
    return 0;
}
static int family_allowed(const char *name)
{
    // name looks like "01-abcdef..." → family = first two chars
    if (CFG.w1.family_count == 0)
        return 1; // accept all
    if (strlen(name) < 2)
        return 0;
    char fam[3] = {name[0], name[1], 0};
    for (int i = 0; i < CFG.w1.family_count; i++)
    {
        if (strcmp(fam, CFG.w1.family_filter[i]) == 0)
            return 1;
    }
    return 0;
}

static int list_w1_devices(char out[][MAX_ID_LEN], int max)
{
    int count = 0;
    DIR *d = opendir(CFG.w1.devices_dir);
    if (!d)
        return -1;
    struct dirent *de;
    while ((de = readdir(d)))
    {
        if (de->d_name[0] == '.')
            continue;
        if (is_excluded(de->d_name))
            continue;
        if (!family_allowed(de->d_name))
            continue;
        if (count < max)
        {
            snprintf(out[count], MAX_ID_LEN, "%s", de->d_name);
            count++;
        }
    }
    closedir(d);
    LOGD("w1 scan found %d device(s)", count);
    return count;
}

static int contains(const char arr[][MAX_ID_LEN], int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i], id) == 0)
            return 1;
    return 0;
}

// ---- MQTT ----
static void publish_json(const char *json)
{
    LOGD("MQTT send: topic=%s payload=%s", CFG.mqtt.topic_state, json);
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = (void *)json;
    msg.payloadlen = (int)strlen(json);
    msg.qos = CFG.mqtt.qos;
    msg.retained = 0;
    MQTTClient_deliveryToken tok;
    MQTTClient_publishMessage(client, CFG.mqtt.topic_state, &msg, &tok);
    MQTTClient_waitForCompletion(client, tok, 5000L);
}

static void send_status_event(void)
{
    char ids[MAX_DEVICES][MAX_ID_LEN];
    int n = list_w1_devices(ids, MAX_DEVICES);
    if (n < 0)
    {
        publish_json("{\"event\":\"status\",\"result\":\"fail\",\"error\":\"w1_read\"}");
        return;
    }
    char buf[MAX_JSON];
    char *p = buf;
    int rem = sizeof(buf);
    int w = snprintf(p, rem, "{\"event\":\"status\",\"result\":\"%s\",\"devices\":[",
                     (n >= 0) ? "pass" : "fail");
    p += w;
    rem -= w;
    for (int i = 0; i < n; i++)
    {
        w = snprintf(p, rem, "%s\"%s\"", (i ? "," : ""), ids[i]);
        p += w;
        rem -= w;
    }
    snprintf(p, rem, "]}");
    publish_json(buf);
}

static void handle_scan(void)
{
    // same as status; kernel w1 auto-updates dir
    send_status_event();
}

static void handle_test(int timeout_ms)
{
    if (timeout_ms <= 0)
        timeout_ms = CFG.test.default_timeout_ms;

    // baseline: what is connected now?
    char baseline[MAX_DEVICES][MAX_ID_LEN];
    LOGI("Starting iButton test (timeout=%d ms)", timeout_ms);
    int nbase = list_w1_devices(baseline, MAX_DEVICES);
    if (nbase < 0)
    {
        publish_json("{\"event\":\"test\",\"result\":\"fail\",\"error\":\"w1_read\"}");
        return;
    }

    uint64_t t0 = now_ms();
    char now[MAX_DEVICES][MAX_ID_LEN];

    // wait for NEW device (not in baseline), with a tiny debounce
    while ((int)(now_ms() - t0) < timeout_ms)
    {
        int n = list_w1_devices(now, MAX_DEVICES);
        if (n > 0)
        {
            for (int i = 0; i < n; i++)
            {
                if (!contains(baseline, nbase, now[i]))
                {
                    // debounce
                    usleep(CFG.test.debounce_ms * 1000);
                    // confirm still present
                    char confirm[MAX_DEVICES][MAX_ID_LEN];
                    int nc = list_w1_devices(confirm, MAX_DEVICES);
                    if (nc > 0 && contains(confirm, nc, now[i]))
                    {
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                                 "{\"event\":\"test\",\"result\":\"pass\",\"device\":\"%s\",\"elapsed_ms\":%u}",
                                 now[i], (unsigned)(now_ms() - t0));
                        publish_json(buf);
                        LOGI("Test PASS: %s", now[i]);
                        return;
                    }
                }
            }
        }
        usleep(50 * 1000);
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"test\",\"result\":\"fail\",\"error\":\"timeout\",\"elapsed_ms\":%u}",
             (unsigned)(now_ms() - t0));
    publish_json(buf);
    LOGW("Test FAIL: timeout after %u ms", (unsigned)(now_ms() - t0));
}

static int json_action_and_timeout(const char *payload, char *action_out, int *timeout_ms_out)
{
    jsmn_parser p;
    jsmn_init(&p);
    jsmntok_t toks[64];
    int ntok = jsmn_parse(&p, payload, (int)strlen(payload), toks, 64);
    if (ntok < 1 || toks[0].type != JSMN_OBJECT)
        return 0;
    if (!json_get_string(payload, toks, ntok, "action", action_out, 32))
        return 0;
    int tms = 0;
    if (json_get_int(payload, toks, ntok, "timeout_ms", &tms))
        *timeout_ms_out = tms;
    return 1;
}

static int msg_arrived_cb(void *ctx, char *topic, int tlen, MQTTClient_message *msg)
{
    (void)ctx;
    (void)tlen;
    char *pl = (char *)msg->payload;
    int len = msg->payloadlen;
    char payload[MAX_JSON];
    int cpy = len < MAX_JSON - 1 ? len : MAX_JSON - 1;
    memcpy(payload, pl, cpy);
    payload[cpy] = 0;

    char action[32] = {0};
    int tms = 0;

    LOGD("MQTT recv: topic=%s len=%d payload=%.*s",
         topic ? topic : "(null)",
         msg ? msg->payloadlen : 0,
         msg ? msg->payloadlen : 0,
         (char *)(msg ? msg->payload : NULL));

    if (!json_action_and_timeout(payload, action, &tms))
    {
        publish_json("{\"event\":\"error\",\"result\":\"fail\",\"error\":\"bad_json\"}");
        MQTTClient_freeMessage(&msg);
        MQTTClient_free(topic);
        return;
    }

    if (strcmp(action, "status") == 0)
    {
        send_status_event();
    }
    else if (strcmp(action, "scan") == 0)
    {
        handle_scan();
    }
    else if (strcmp(action, "test") == 0)
    {
        handle_test(tms);
    }
    else
    {
        publish_json("{\"event\":\"error\",\"result\":\"fail\",\"error\":\"unknown_action\"}");
    }

    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}
static void connection_lost_cb(void *ctx, char *cause)
{
    LOGW("MQTT connection lost: %s", cause ? cause : "(no cause)");
}
static void delivery_complete_cb(void *ctx, MQTTClient_deliveryToken dt)
{
    LOGD("MQTT delivery complete: token=%d", (int)dt);
}

static int mqtt_connect_and_sub(void)
{
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_create(&client, CFG.mqtt.broker, CFG.mqtt.client_id,
                      MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_setCallbacks(client, NULL, connection_lost_cb, msg_arrived_cb, delivery_complete_cb);
    conn_opts.keepAliveInterval = 20;
    conn_opts.cleansession = 1;
    if (CFG.mqtt.username[0])
        conn_opts.username = CFG.mqtt.username;
    if (CFG.mqtt.password[0])
        conn_opts.password = CFG.mqtt.password;
    LOGI("Connecting MQTT to %s as %s", CFG.mqtt.broker, CFG.mqtt.client_id);
    int rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        fprintf(stderr, "MQTT connect failed: %d\n", rc);
        LOGE("MQTT connect failed: %d", rc);
        return -1;
    }
    MQTTClient_subscribe(client, CFG.mqtt.topic_cmd, CFG.mqtt.qos);
    LOGI("Subscribing to topic %s", CFG.mqtt.topic_cmd);
    return 0;
}





int main(int argc, char **argv)
{
    int dry_run = 0;
    // default already set to /etc/ibutton-tester/config.json in g_config_path
    if (parse_args(argc, argv, &dry_run) != 0)
        return 2;

    LOGI("Log level: %d (0=ERROR,1=WARN,2=INFO,3=DEBUG)", g_log_level);
    LOGI("Config path: %s", g_config_path);

    if (load_config(g_config_path) != 0)
        return 1;

    if (dry_run)
    {
        // Quick sanity: list devices once and exit
        char ids[16][MAX_ID_LEN];
        int n = list_w1_devices(ids, 16);
        if (n >= 0)
        {
            LOGI("Dry-run OK. %d device(s) currently present:", n);
            for (int i = 0; i < n; i++)
                LOGI("  - %s", ids[i]);
            return 0;
        }
        else
        {
            LOGE("Dry-run failed to read w1 devices.");
            return 1;
        }
    }
    // if (g_log_level == LOG_DEBUG)
    // {
    //     MQTTClient_setTraceCallback(paho_trace_cb);
    //     MQTTClient_setTraceLevel(MQTTCLIENT_TRACE_MAXIMUM);
    // }
   
    if (mqtt_connect_and_sub() != 0)
        return 2;

    publish_json("{\"event\":\"startup\",\"result\":\"pass\"}");
    for (;;)
        pause();

    // (unreached)
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    return 0;
}
