/*
 * test_integ_userdb.c — Integration tests for user database
 *
 * Creates a config with two users and tests lookup functions.
 */

#include "kerchunk_user.h"
#include "kerchunk_config.h"
#include <string.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

void test_integ_userdb(void)
{
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name",       "Brian West");
    kerchunk_config_set(cfg, "user.1", "username",   "bwest");
    kerchunk_config_set(cfg, "user.1", "email",      "brian@example.com");
    kerchunk_config_set(cfg, "user.1", "dtmf_login", "101");
    kerchunk_config_set(cfg, "user.1", "ani",         "5551");
    kerchunk_config_set(cfg, "user.1", "access",      "2");
    kerchunk_config_set(cfg, "user.2", "name",       "Alice");
    kerchunk_config_set(cfg, "user.2", "dtmf_login", "102");
    kerchunk_config_set(cfg, "user.2", "access",      "1");
    kerchunk_user_init(cfg);

    /* 1. lookup by ID */
    test_begin("userdb: lookup by ID");
    const kerchunk_user_t *u = kerchunk_user_lookup_by_id(1);
    test_assert(u != NULL, "user 1 not found");
    test_assert(strcmp(u->name, "Brian West") == 0, "wrong name");
    test_assert(strcmp(u->username, "bwest") == 0, "wrong username");
    test_assert(strcmp(u->email, "brian@example.com") == 0, "wrong email");
    test_assert(u->access == 2, "wrong access");
    test_end();

    /* 2. lookup by ANI */
    test_begin("userdb: lookup by ANI");
    u = kerchunk_user_lookup_by_ani("5551");
    test_assert(u != NULL, "ANI 5551 not found");
    test_assert(u->id == 1, "wrong user");
    test_end();

    /* 3. lookup by username */
    test_begin("userdb: lookup by username");
    u = kerchunk_user_lookup_by_username("bwest");
    test_assert(u != NULL, "username bwest not found");
    test_assert(u->id == 1, "wrong user id");
    u = kerchunk_user_lookup_by_username("BWEST");
    test_assert(u != NULL, "case-insensitive lookup failed");
    test_end();

    /* 4. auto-derived username */
    test_begin("userdb: auto-derived username");
    u = kerchunk_user_lookup_by_id(2);
    test_assert(u != NULL, "user 2 not found");
    test_assert(strcmp(u->username, "alice") == 0, "wrong derived username");
    test_end();

    /* 5. miss returns NULL */
    test_begin("userdb: miss returns NULL");
    test_assert(kerchunk_user_lookup_by_id(99)       == NULL, "ghost user ID");
    test_assert(kerchunk_user_lookup_by_ani("0000")  == NULL, "ghost ANI");
    test_assert(kerchunk_user_lookup_by_username("nobody") == NULL, "ghost username");
    test_end();

    /* 6. count */
    test_begin("userdb: count");
    test_assert(kerchunk_user_count() == 2, "wrong count");
    test_end();

    /* 7. group TX (user tones removed — should get group tones) */
    test_begin("userdb: group TX lookup");
    kerchunk_config_set(cfg, "group.1", "name", "Family");
    kerchunk_config_set(cfg, "group.1", "tx_ctcss", "1318");
    kerchunk_config_set(cfg, "user.1", "group", "1");
    kerchunk_user_shutdown();
    kerchunk_user_init(cfg);
    uint16_t ctcss = 0, dcs = 0;
    int rc = kerchunk_user_lookup_group_tx(1, &ctcss, &dcs);
    test_assert(rc == 0, "group tx lookup failed");
    test_assert(ctcss == 1318, "wrong group ctcss");
    test_end();

    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
