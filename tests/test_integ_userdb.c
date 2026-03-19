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
    kerchunk_config_set(cfg, "user.1", "name",       "Brian");
    kerchunk_config_set(cfg, "user.1", "ctcss",      "1000");
    kerchunk_config_set(cfg, "user.1", "dtmf_login", "101");
    kerchunk_config_set(cfg, "user.1", "ani",         "5551");
    kerchunk_config_set(cfg, "user.1", "access",      "2");
    kerchunk_config_set(cfg, "user.2", "name",       "Alice");
    kerchunk_config_set(cfg, "user.2", "ctcss",      "1318");
    kerchunk_config_set(cfg, "user.2", "dtmf_login", "102");
    kerchunk_config_set(cfg, "user.2", "access",      "1");
    kerchunk_user_init(cfg);

    /* 1. lookup by ID */
    test_begin("userdb: lookup by ID");
    const kerchunk_user_t *u = kerchunk_user_lookup_by_id(1);
    test_assert(u != NULL, "user 1 not found");
    test_assert(strcmp(u->name, "Brian") == 0, "wrong name");
    test_assert(u->access == 2, "wrong access");
    test_end();

    /* 2. lookup by ANI */
    test_begin("userdb: lookup by ANI");
    u = kerchunk_user_lookup_by_ani("5551");
    test_assert(u != NULL, "ANI 5551 not found");
    test_assert(u->id == 1, "wrong user");
    test_end();

    /* 4. miss returns NULL */
    test_begin("userdb: miss returns NULL");
    test_assert(kerchunk_user_lookup_by_id(99)       == NULL, "ghost user ID");
    test_assert(kerchunk_user_lookup_by_ani("0000")  == NULL, "ghost ANI");
    test_end();

    /* 5. count */
    test_begin("userdb: count");
    test_assert(kerchunk_user_count() == 2, "wrong count");
    test_end();

    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
