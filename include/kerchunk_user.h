/*
 * kerchunk_user.h — User database types
 */

#ifndef KERCHUNK_USER_H
#define KERCHUNK_USER_H

#include <stdint.h>

/* Access levels */
#define KERCHUNK_ACCESS_NONE    0
#define KERCHUNK_ACCESS_BASIC   1
#define KERCHUNK_ACCESS_ADMIN   2

/* Caller identification methods */
#define KERCHUNK_CALLER_DTMF_ANI  3
#define KERCHUNK_CALLER_DTMF_LOGIN 4
#define KERCHUNK_CALLER_WEB        5

/* Maximum groups */
#define KERCHUNK_MAX_GROUPS 16

/* User record */
typedef struct {
    int      id;
    char     username[32];      /* lowercase, no spaces — login identity */
    char     name[32];          /* display name */
    char     email[128];        /* email address */
    char     dtmf_login[8];     /* DTMF login code, "" = none */
    char     ani[16];           /* ANI code, "" = none */
    int      access;            /* Access level */
    int      voicemail;         /* Voicemail enabled */
    int      group;             /* Group ID, 0 = default/none */
    char     totp_secret[64];   /* Base32-encoded TOTP secret, "" = none */
} kerchunk_user_t;

/* Group record */
typedef struct {
    int      id;
    char     name[32];
    uint16_t tx_ctcss_freq_x10; /* Default TX CTCSS for group members */
    uint16_t tx_dcs_code;       /* Default TX DCS for group members */
} kerchunk_group_t;

/* Forward declaration */
typedef struct kerchunk_config kerchunk_config_t;

/* User database API */
int  kerchunk_user_init(const kerchunk_config_t *cfg);
void kerchunk_user_shutdown(void);

const kerchunk_user_t *kerchunk_user_lookup_by_id(int user_id);
const kerchunk_user_t *kerchunk_user_get(int index);  /* 0-based index for iteration */
const kerchunk_user_t *kerchunk_user_lookup_by_ani(const char *ani);
const kerchunk_user_t *kerchunk_user_lookup_by_username(const char *username);
int  kerchunk_user_count(void);

/* Group iteration and lookup */
int  kerchunk_group_count(void);
const kerchunk_group_t *kerchunk_group_get(int index);
const kerchunk_group_t *kerchunk_group_lookup_by_id(int group_id);

/* Group lookup — returns TX tone for a user (user override → group fallback) */
int  kerchunk_user_lookup_group_tx(int user_id, uint16_t *ctcss_out, uint16_t *dcs_out);

#endif /* KERCHUNK_USER_H */
