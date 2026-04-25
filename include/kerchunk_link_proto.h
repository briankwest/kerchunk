/*
 * kerchunk_link_proto.h — wire protocol constants for kerchunk repeater
 *                         linking (reflectd ↔ mod_link).
 *
 * Header-only. No runtime allocations. Consumed by both the reflector
 * (src/reflectd/) and the local module (modules/mod_link.c).
 *
 * See PLAN-LINK.md § 4.1 for the full message catalog and § 4.1.6 for
 * the error-code enum.
 */

#ifndef KERCHUNK_LINK_PROTO_H
#define KERCHUNK_LINK_PROTO_H

#include <string.h>

/* Bumped when an incompatible change lands on the wire. Reflector compares
 * client_version >= min_client_version (string lex compare for now). */
#define KERCHUNK_LINK_PROTO_VERSION   "1"

#define KERCHUNK_LINK_DEFAULT_WS_PORT     8443
#define KERCHUNK_LINK_DEFAULT_RTP_PORT    7878
#define KERCHUNK_LINK_DEFAULT_KEEPALIVE_S 15
#define KERCHUNK_LINK_DEFAULT_HANGTIME_MS 1500

/* Challenge / nonce sizes (bytes, hex-encoded on the wire so 2× chars). */
#define KERCHUNK_LINK_CHALLENGE_BYTES 16
#define KERCHUNK_LINK_NONCE_BYTES     16
#define KERCHUNK_LINK_HMAC_BYTES      32   /* SHA-256 */
#define KERCHUNK_LINK_PSK_BYTES       32

/* SRTP material as delivered in login_ok. AES-128-CTR + HMAC-SHA1-80
 * (libsrtp profile SRTP_AES128_CM_HMAC_SHA1_80). */
#define KERCHUNK_LINK_SRTP_KEY_BYTES  16
#define KERCHUNK_LINK_SRTP_SALT_BYTES 14

/* RTP payload type for Opus (dynamic, only one codec on the wire). */
#define KERCHUNK_LINK_RTP_PAYLOAD_TYPE 100

/* Opus parameters per PLAN-LINK.md § 4.2 + the 24 kHz decision. */
#define KERCHUNK_LINK_OPUS_SAMPLE_RATE 24000
#define KERCHUNK_LINK_OPUS_FRAME_MS    60
#define KERCHUNK_LINK_OPUS_FRAME_SAMPLES \
    (KERCHUNK_LINK_OPUS_SAMPLE_RATE * KERCHUNK_LINK_OPUS_FRAME_MS / 1000)
#define KERCHUNK_LINK_OPUS_BITRATE     32000

/* RTP timestamps for Opus run at 48 kHz per RFC 7587 regardless of the
 * internal codec sample rate. */
#define KERCHUNK_LINK_RTP_CLOCK_HZ        48000
#define KERCHUNK_LINK_RTP_FRAME_TS_TICKS  \
    (KERCHUNK_LINK_RTP_CLOCK_HZ * KERCHUNK_LINK_OPUS_FRAME_MS / 1000)

/* Maximum RTP packet size we expect on the wire (header + Opus payload +
 * SRTP auth tag). 12-byte RTP header, ~200 byte Opus, 10-byte tag. */
#define KERCHUNK_LINK_RTP_MAX_PACKET 1500

/* Maximum control-plane message length. JSON; one message per WS frame
 * (or per line on plain TCP during phase 1). Sized for the largest
 * realistic message (login_ok with all hex blobs). */
#define KERCHUNK_LINK_MAX_MSG 1024

/* ── Message types — control plane ─────────────────────────────────── */

#define LINK_MSG_HELLO                 "hello"
#define LINK_MSG_LOGIN                 "login"
#define LINK_MSG_LOGIN_OK              "login_ok"
#define LINK_MSG_LOGIN_DENIED          "login_denied"
#define LINK_MSG_KICKED                "kicked"
#define LINK_MSG_REFLECTOR_SHUTDOWN    "reflector_shutdown"
#define LINK_MSG_PING                  "ping"
#define LINK_MSG_PONG                  "pong"
#define LINK_MSG_SET_TG                "set_tg"
#define LINK_MSG_TG_OK                 "tg_ok"
#define LINK_MSG_TG_DENIED             "tg_denied"
#define LINK_MSG_TG_MEMBERSHIP_CHANGED "tg_membership_changed"
#define LINK_MSG_MUTE                  "mute"
#define LINK_MSG_UNMUTE                "unmute"
#define LINK_MSG_TALKER                "talker"
#define LINK_MSG_FLOOR_DENIED          "floor_denied"
#define LINK_MSG_FLOOR_REVOKED         "floor_revoked"
#define LINK_MSG_QUALITY               "quality"
#define LINK_MSG_TARGET_BITRATE        "target_bitrate"
#define LINK_MSG_ERROR                 "error"

/* ── Error codes — § 4.1.6 ─────────────────────────────────────────── */

#define LINK_ERR_BAD_KEY          "bad_key"
#define LINK_ERR_UNKNOWN_NODE     "unknown_node"
#define LINK_ERR_BANNED           "banned"
#define LINK_ERR_VERSION_MISMATCH "version_mismatch"
#define LINK_ERR_NOT_AUTHORIZED   "not_authorized"
#define LINK_ERR_UNKNOWN_TG       "unknown_tg"
#define LINK_ERR_NODE_BUSY        "node_busy"
#define LINK_ERR_IDLE_TIMEOUT     "idle_timeout"
#define LINK_ERR_LOSS_TOO_HIGH    "loss_too_high"
#define LINK_ERR_LEASE_EXPIRED    "lease_expired"
#define LINK_ERR_AUTH_FAILURES    "auth_failures"
#define LINK_ERR_ADMIN_ACTION     "admin_action"
#define LINK_ERR_CONFIG_RELOAD    "config_reload"
#define LINK_ERR_PROTOCOL_ERROR   "protocol_error"
#define LINK_ERR_INTERNAL         "internal"
#define LINK_ERR_DEPLOY           "deploy"

/* True if the client should stop reconnecting on this code (operator
 * intervention required). False codes are transient — back off and retry. */
static inline int link_err_is_permanent(const char *code)
{
    if (!code) return 0;
    return  !strcmp(code, LINK_ERR_BAD_KEY)
         || !strcmp(code, LINK_ERR_UNKNOWN_NODE)
         || !strcmp(code, LINK_ERR_BANNED)
         || !strcmp(code, LINK_ERR_VERSION_MISMATCH)
         || !strcmp(code, LINK_ERR_NOT_AUTHORIZED)
         || !strcmp(code, LINK_ERR_UNKNOWN_TG);
}

#endif /* KERCHUNK_LINK_PROTO_H */
