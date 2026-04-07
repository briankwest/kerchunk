/*
 * test_web_acl.c — Unit tests for mod_web admin ACL
 *
 * Tests parse_acl_entry(), parse_admin_acl(), and check_admin_acl()
 * with various IPv4/IPv6/CIDR combinations.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

/* ── Minimal mongoose stubs needed by the ACL code ── */
struct mg_addr {
    union {
        uint8_t  ip[16];
        uint32_t ip4;
        uint64_t ip6[2];
    } addr;
    uint16_t port;
    uint8_t  scope_id;
    int      is_ip6;    /* bool in real mongoose, int here for C99 */
};

struct mg_connection {
    struct mg_addr rem;
};

/* ── Pull in the ACL types and functions directly ── */

#define MAX_ACL_ENTRIES 16

typedef struct {
    uint8_t  addr[16];
    int      prefix_len;
    int      is_ip6;
} acl_entry_t;

static acl_entry_t g_admin_acl[MAX_ACL_ENTRIES];
static int          g_admin_acl_count = 0;

/* Stub g_core->log to suppress output */
struct stub_core {
    void (*log)(int, const char*, const char*, ...);
};
static void stub_log(int lvl, const char *mod, const char *fmt, ...) {
    (void)lvl; (void)mod; (void)fmt;
}
static struct stub_core stub_core_val = { .log = stub_log };
static struct stub_core *g_core __attribute__((unused)) = &stub_core_val;
#define LOG_MOD "test"
#define KERCHUNK_LOG_WARN 2

/* Include the ACL functions from mod_web.c by copy.
 * These are the exact functions under test. */

static int parse_acl_entry(const char *str, acl_entry_t *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", str);
    char *s = buf;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && s[len-1] == ' ') s[--len] = '\0';
    if (len == 0) return -1;

    int prefix_len = -1;
    char *slash = strchr(s, '/');
    if (slash) { *slash = '\0'; prefix_len = atoi(slash + 1); }

    memset(out, 0, sizeof(*out));

    struct in6_addr addr6;
    if (inet_pton(AF_INET6, s, &addr6) == 1) {
        memcpy(out->addr, &addr6, 16);
        out->is_ip6 = 1;
        out->prefix_len = (prefix_len >= 0) ? prefix_len : 128;
        return 0;
    }

    struct in_addr addr4;
    if (inet_pton(AF_INET, s, &addr4) == 1) {
        memcpy(out->addr, &addr4, 4);
        out->is_ip6 = 0;
        out->prefix_len = (prefix_len >= 0) ? prefix_len : 32;
        return 0;
    }
    return -1;
}

static void parse_admin_acl(const char *acl_str)
{
    g_admin_acl_count = 0;
    if (!acl_str || !acl_str[0]) return;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", acl_str);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && g_admin_acl_count < MAX_ACL_ENTRIES) {
        acl_entry_t entry;
        if (parse_acl_entry(tok, &entry) == 0)
            g_admin_acl[g_admin_acl_count++] = entry;
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

static int check_admin_acl(struct mg_connection *c)
{
    if (g_admin_acl_count == 0) return 1;

    for (int i = 0; i < g_admin_acl_count; i++) {
        acl_entry_t *acl = &g_admin_acl[i];
        if (c->rem.is_ip6 != acl->is_ip6) {
            if (c->rem.is_ip6 && !acl->is_ip6) {
                uint8_t *ip6 = c->rem.addr.ip;
                if (ip6[10] == 0xff && ip6[11] == 0xff) {
                    int bits = acl->prefix_len;
                    int match = 1;
                    for (int b = 0; b < 4 && bits > 0; b++) {
                        uint8_t mask = (bits >= 8) ? 0xff
                                     : (uint8_t)(0xff << (8 - bits));
                        if ((ip6[12 + b] & mask) != (acl->addr[b] & mask)) {
                            match = 0; break;
                        }
                        bits -= 8;
                    }
                    if (match) return 1;
                }
            }
            continue;
        }
        int addr_len = acl->is_ip6 ? 16 : 4;
        int bits = acl->prefix_len;
        int match = 1;
        for (int b = 0; b < addr_len && bits > 0; b++) {
            uint8_t mask = (bits >= 8) ? 0xff : (uint8_t)(0xff << (8 - bits));
            if ((c->rem.addr.ip[b] & mask) != (acl->addr[b] & mask)) {
                match = 0; break;
            }
            bits -= 8;
        }
        if (match) return 1;
    }
    return 0;
}

/* ── Test helpers ── */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-55s", name); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define ASSERT_PASS(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static struct mg_connection make_ipv4_conn(const char *ip_str)
{
    struct mg_connection c;
    memset(&c, 0, sizeof(c));
    c.rem.is_ip6 = 0;
    struct in_addr a;
    inet_pton(AF_INET, ip_str, &a);
    memcpy(c.rem.addr.ip, &a, 4);
    return c;
}

static struct mg_connection make_ipv6_conn(const char *ip_str)
{
    struct mg_connection c;
    memset(&c, 0, sizeof(c));
    c.rem.is_ip6 = 1;
    struct in6_addr a;
    inet_pton(AF_INET6, ip_str, &a);
    memcpy(c.rem.addr.ip, &a, 16);
    return c;
}

/* ── Tests ── */

static void test_parse_ipv4(void)
{
    TEST("parse: IPv4 single host");
    acl_entry_t e;
    ASSERT(parse_acl_entry("192.168.86.105", &e) == 0, "parse failed");
    ASSERT(!e.is_ip6, "should be IPv4");
    ASSERT(e.prefix_len == 32, "prefix should be 32");
    ASSERT(e.addr[0] == 192 && e.addr[1] == 168, "addr wrong");
    PASS();
}

static void test_parse_ipv4_cidr(void)
{
    TEST("parse: IPv4 CIDR /24");
    acl_entry_t e;
    ASSERT(parse_acl_entry("10.0.0.0/8", &e) == 0, "parse failed");
    ASSERT(!e.is_ip6, "should be IPv4");
    ASSERT(e.prefix_len == 8, "prefix should be 8");
    ASSERT(e.addr[0] == 10, "addr wrong");
    PASS();
}

static void test_parse_ipv6(void)
{
    TEST("parse: IPv6 CIDR");
    acl_entry_t e;
    ASSERT(parse_acl_entry("fd00::/8", &e) == 0, "parse failed");
    ASSERT(e.is_ip6, "should be IPv6");
    ASSERT(e.prefix_len == 8, "prefix should be 8");
    ASSERT(e.addr[0] == 0xfd, "addr wrong");
    PASS();
}

static void test_parse_invalid(void)
{
    TEST("parse: invalid entry");
    acl_entry_t e;
    ASSERT(parse_acl_entry("not_an_ip", &e) != 0, "should fail");
    ASSERT(parse_acl_entry("", &e) != 0, "empty should fail");
    PASS();
}

static void test_parse_multi(void)
{
    TEST("parse: comma-separated list");
    parse_admin_acl("192.168.86.0/24, 10.0.0.0/8, fd00::/8");
    ASSERT(g_admin_acl_count == 3, "expected 3 entries");
    ASSERT(!g_admin_acl[0].is_ip6, "first should be IPv4");
    ASSERT(!g_admin_acl[1].is_ip6, "second should be IPv4");
    ASSERT(g_admin_acl[2].is_ip6, "third should be IPv6");
    PASS();
}

static void test_empty_acl_allows_all(void)
{
    TEST("ACL: empty allows all");
    parse_admin_acl(NULL);
    struct mg_connection c = make_ipv4_conn("1.2.3.4");
    ASSERT(check_admin_acl(&c) == 1, "empty ACL should allow");
    PASS();
}

static void test_ipv4_match(void)
{
    TEST("ACL: IPv4 in /24 matches");
    parse_admin_acl("192.168.86.0/24");
    struct mg_connection c = make_ipv4_conn("192.168.86.42");
    ASSERT(check_admin_acl(&c) == 1, "should match");
    PASS();
}

static void test_ipv4_nomatch(void)
{
    TEST("ACL: IPv4 outside /24 denied");
    parse_admin_acl("192.168.86.0/24");
    struct mg_connection c = make_ipv4_conn("192.168.87.1");
    ASSERT(check_admin_acl(&c) == 0, "should deny");
    PASS();
}

static void test_ipv4_exact(void)
{
    TEST("ACL: IPv4 exact host /32");
    parse_admin_acl("10.0.0.5/32");
    struct mg_connection c1 = make_ipv4_conn("10.0.0.5");
    struct mg_connection c2 = make_ipv4_conn("10.0.0.6");
    ASSERT(check_admin_acl(&c1) == 1, "exact should match");
    ASSERT(check_admin_acl(&c2) == 0, "other should deny");
    PASS();
}

static void test_ipv4_wide(void)
{
    TEST("ACL: IPv4 /8 wide subnet");
    parse_admin_acl("10.0.0.0/8");
    struct mg_connection c1 = make_ipv4_conn("10.255.255.255");
    struct mg_connection c2 = make_ipv4_conn("11.0.0.1");
    ASSERT(check_admin_acl(&c1) == 1, "10.x should match");
    ASSERT(check_admin_acl(&c2) == 0, "11.x should deny");
    PASS();
}

static void test_ipv6_match(void)
{
    TEST("ACL: IPv6 in fd00::/8 matches");
    parse_admin_acl("fd00::/8");
    struct mg_connection c = make_ipv6_conn("fd12:3456:7890::1");
    ASSERT(check_admin_acl(&c) == 1, "should match");
    PASS();
}

static void test_ipv6_nomatch(void)
{
    TEST("ACL: IPv6 outside fd00::/8 denied");
    parse_admin_acl("fd00::/8");
    struct mg_connection c = make_ipv6_conn("2001:db8::1");
    ASSERT(check_admin_acl(&c) == 0, "should deny");
    PASS();
}

static void test_ipv4_mapped_ipv6(void)
{
    TEST("ACL: IPv4-mapped IPv6 matches IPv4 ACL");
    parse_admin_acl("192.168.86.0/24");
    /* ::ffff:192.168.86.42 */
    struct mg_connection c = make_ipv6_conn("::ffff:192.168.86.42");
    ASSERT(check_admin_acl(&c) == 1, "mapped address should match");
    PASS();
}

static void test_ipv4_mapped_ipv6_nomatch(void)
{
    TEST("ACL: IPv4-mapped IPv6 outside subnet denied");
    parse_admin_acl("192.168.86.0/24");
    struct mg_connection c = make_ipv6_conn("::ffff:10.0.0.1");
    ASSERT(check_admin_acl(&c) == 0, "wrong subnet should deny");
    PASS();
}

static void test_multi_acl(void)
{
    TEST("ACL: multiple entries, any match allows");
    parse_admin_acl("192.168.86.0/24, 10.0.0.0/8");
    struct mg_connection c1 = make_ipv4_conn("192.168.86.1");
    struct mg_connection c2 = make_ipv4_conn("10.5.5.5");
    struct mg_connection c3 = make_ipv4_conn("172.16.0.1");
    ASSERT(check_admin_acl(&c1) == 1, "first subnet should match");
    ASSERT(check_admin_acl(&c2) == 1, "second subnet should match");
    ASSERT(check_admin_acl(&c3) == 0, "third IP should deny");
    PASS();
}

static void test_mixed_v4_v6_acl(void)
{
    TEST("ACL: mixed IPv4 + IPv6 entries");
    parse_admin_acl("192.168.86.0/24, fd00::/8");
    struct mg_connection c1 = make_ipv4_conn("192.168.86.50");
    struct mg_connection c2 = make_ipv6_conn("fd00::99");
    struct mg_connection c3 = make_ipv4_conn("8.8.8.8");
    struct mg_connection c4 = make_ipv6_conn("2001:db8::1");
    ASSERT(check_admin_acl(&c1) == 1, "IPv4 match");
    ASSERT(check_admin_acl(&c2) == 1, "IPv6 match");
    ASSERT(check_admin_acl(&c3) == 0, "IPv4 deny");
    ASSERT(check_admin_acl(&c4) == 0, "IPv6 deny");
    PASS();
}

static void test_localhost(void)
{
    TEST("ACL: localhost /32");
    parse_admin_acl("127.0.0.1/32");
    struct mg_connection c1 = make_ipv4_conn("127.0.0.1");
    struct mg_connection c2 = make_ipv4_conn("127.0.0.2");
    ASSERT(check_admin_acl(&c1) == 1, "localhost should match");
    ASSERT(check_admin_acl(&c2) == 0, "127.0.0.2 should deny");
    PASS();
}

static void test_ipv6_localhost(void)
{
    TEST("ACL: IPv6 ::1/128");
    parse_admin_acl("::1/128");
    struct mg_connection c1 = make_ipv6_conn("::1");
    struct mg_connection c2 = make_ipv6_conn("::2");
    ASSERT(check_admin_acl(&c1) == 1, "::1 should match");
    ASSERT(check_admin_acl(&c2) == 0, "::2 should deny");
    PASS();
}

static void test_whitespace_handling(void)
{
    TEST("ACL: whitespace in entries");
    parse_admin_acl("  192.168.86.0/24 , 10.0.0.0/8  ");
    ASSERT(g_admin_acl_count == 2, "should parse 2 entries");
    struct mg_connection c = make_ipv4_conn("192.168.86.1");
    ASSERT(check_admin_acl(&c) == 1, "should match with whitespace");
    PASS();
}

int main(void)
{
    printf("mod_web admin ACL tests:\n");

    test_parse_ipv4();
    test_parse_ipv4_cidr();
    test_parse_ipv6();
    test_parse_invalid();
    test_parse_multi();
    test_empty_acl_allows_all();
    test_ipv4_match();
    test_ipv4_nomatch();
    test_ipv4_exact();
    test_ipv4_wide();
    test_ipv6_match();
    test_ipv6_nomatch();
    test_ipv4_mapped_ipv6();
    test_ipv4_mapped_ipv6_nomatch();
    test_multi_acl();
    test_mixed_v4_v6_acl();
    test_localhost();
    test_ipv6_localhost();
    test_whitespace_handling();

    printf("\n==================\n");
    printf("Results: %d/%d passed", tests_passed, tests_run);
    if (tests_passed < tests_run)
        printf(", %d FAILED", tests_run - tests_passed);
    printf("\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
