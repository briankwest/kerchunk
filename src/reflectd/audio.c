/*
 * audio.c — SRTP fan-out implementation. See audio.h.
 *
 * Each node has two libsrtp sessions:
 *   - in_session  decrypts node→reflector packets (key=node's master,
 *                 expected ssrc = node_ssrc).
 *   - out_session encrypts reflector→node packets (key=node's master,
 *                 src ssrc = reflector_ssrc).
 *
 * The reflector picks a fresh master key + salt + ssrc pair per node at
 * login time and ships them in login_ok. libsrtp owns sequence numbers
 * internally per stream so we don't have to track them.
 */

#include "audio.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <srtp2/srtp.h>

/* libsrtp wants key||salt as one buffer for the policy. */
#define MASTER_KEY_LEN  (KERCHUNK_LINK_SRTP_KEY_BYTES + \
                         KERCHUNK_LINK_SRTP_SALT_BYTES)

struct audio_node_s {
    srtp_t   in_session;
    srtp_t   out_session;
    uint32_t node_ssrc;        /* what we expect on packets FROM the node */
    uint32_t reflector_ssrc;   /* what we set on packets TO the node */
    uint8_t  master[MASTER_KEY_LEN];   /* keep for debug; not strictly needed */

    struct sockaddr_storage rtp_addr;
    socklen_t               rtp_addr_len;
    int                     have_addr;
};

int audio_global_init(void)
{
    srtp_err_status_t rc = srtp_init();
    if (rc != srtp_err_status_ok) {
        fprintf(stderr, "audio: srtp_init failed (%d)\n", rc);
        return -1;
    }
    return 0;
}

void audio_global_shutdown(void)
{
    srtp_shutdown();
}

audio_node_t *audio_node_create(const uint8_t *master_key,
                                const uint8_t *master_salt,
                                uint32_t       node_ssrc,
                                uint32_t       reflector_ssrc)
{
    audio_node_t *an = calloc(1, sizeof(*an));
    if (!an) return NULL;

    memcpy(an->master,                         master_key,
           KERCHUNK_LINK_SRTP_KEY_BYTES);
    memcpy(an->master + KERCHUNK_LINK_SRTP_KEY_BYTES, master_salt,
           KERCHUNK_LINK_SRTP_SALT_BYTES);
    an->node_ssrc      = node_ssrc;
    an->reflector_ssrc = reflector_ssrc;

    /* IN session: decrypt packets from the node. Sender's SSRC is
     * node_ssrc; receiver context is the reflector. */
    srtp_policy_t in_pol;
    memset(&in_pol, 0, sizeof(in_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtcp);
    in_pol.ssrc.type    = ssrc_specific;
    in_pol.ssrc.value   = node_ssrc;
    in_pol.key          = an->master;
    in_pol.window_size  = 1024;
    in_pol.allow_repeat_tx = 0;

    if (srtp_create(&an->in_session, &in_pol) != srtp_err_status_ok) {
        fprintf(stderr, "audio: srtp_create(in) failed for ssrc=%u\n",
                node_ssrc);
        free(an);
        return NULL;
    }

    /* OUT session: encrypt packets going to the node. SSRC the reflector
     * stamps on every outbound packet for this node. */
    srtp_policy_t out_pol;
    memset(&out_pol, 0, sizeof(out_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtcp);
    out_pol.ssrc.type     = ssrc_specific;
    out_pol.ssrc.value    = reflector_ssrc;
    out_pol.key           = an->master;
    out_pol.window_size   = 1024;
    out_pol.allow_repeat_tx = 1;   /* benign; we don't repeat anyway */

    if (srtp_create(&an->out_session, &out_pol) != srtp_err_status_ok) {
        fprintf(stderr, "audio: srtp_create(out) failed for ssrc=%u\n",
                reflector_ssrc);
        srtp_dealloc(an->in_session);
        free(an);
        return NULL;
    }

    return an;
}

void audio_node_destroy(audio_node_t *an)
{
    if (!an) return;
    srtp_dealloc(an->in_session);
    srtp_dealloc(an->out_session);
    free(an);
}

void audio_node_get_addr(const audio_node_t *an,
                         struct sockaddr_storage *out, socklen_t *out_len)
{
    *out     = an->rtp_addr;
    *out_len = an->rtp_addr_len;
}

int audio_node_have_addr(const audio_node_t *an)
{
    return an->have_addr;
}

int audio_node_unprotect(audio_node_t *an,
                         uint8_t *pkt, int *plen,
                         const struct sockaddr_storage *src,
                         socklen_t src_len)
{
    int rc = srtp_unprotect(an->in_session, pkt, plen);
    if (rc == srtp_err_status_ok) {
        /* Learn the sender's UDP addr on first successful auth. */
        an->rtp_addr     = *src;
        an->rtp_addr_len = src_len;
        an->have_addr    = 1;
    }
    return rc;
}

int audio_node_send_to(audio_node_t *an, int udp_fd,
                       const uint8_t *cleartext_rtp, int rtp_len)
{
    if (!an->have_addr) return -1;
    if (rtp_len < 12 || rtp_len > KERCHUNK_LINK_RTP_MAX_PACKET) return -1;

    /* Copy into a working buffer with room for SRTP auth tag growth.
     * SRTP_AES128_CM_HMAC_SHA1_80 grows by 10 bytes. */
    uint8_t buf[KERCHUNK_LINK_RTP_MAX_PACKET];
    memcpy(buf, cleartext_rtp, (size_t)rtp_len);

    /* Rewrite SSRC field (offset 8, network order). */
    uint32_t ssrc_n = htonl(an->reflector_ssrc);
    memcpy(buf + 8, &ssrc_n, 4);

    int len = rtp_len;
    if (srtp_protect(an->out_session, buf, &len) != srtp_err_status_ok)
        return -1;

    ssize_t s = sendto(udp_fd, buf, (size_t)len, 0,
                       (struct sockaddr *)&an->rtp_addr, an->rtp_addr_len);
    return s == (ssize_t)len ? 0 : -1;
}
