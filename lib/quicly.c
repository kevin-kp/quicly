/*
 * Copyright (c) 2017 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "khash.h"
#include "quicly.h"
#include "quicly/ack.h"
#include "quicly/frame.h"

#define QUICLY_PROTOCOL_VERSION 0xff00000c

#define QUICLY_PACKET_TYPE_INITIAL 0xff
#define QUICLY_PACKET_TYPE_RETRY 0xfe
#define QUICLY_PACKET_TYPE_HANDSHAKE 0xfd
#define QUICLY_PACKET_TYPE_0RTT_PROTECTED 0xfc
#define QUICLY_PACKET_TYPE_LONG_MIN QUICLY_PACKET_TYPE_0RTT_PROTECTED

#define QUICLY_PACKET_TYPE_IS_1RTT(t) (((t)&0x80) == 0)
#define QUICLY_PACKET_TYPE_1RTT_TO_KEY_PHASE(t) (((t)&0x40) != 0)

#define QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS 26
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA 0
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA 1
#define QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT 3
#define QUICLY_TRANSPORT_PARAMETER_ID_STATELESS_RESET_TOKEN 6
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_BIDI 2
#define QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_UNI 8

#define STATELESS_RESET_TOKEN_SIZE 16

#define STREAM_IS_CLIENT_INITIATED(stream_id) (((stream_id)&1) == 0)
#define STREAM_IS_UNI(stream_id) (((stream_id)&2) != 0)

KHASH_MAP_INIT_INT64(quicly_stream_t, quicly_stream_t *)

#define DEBUG_LOG(conn, stream_id, ...)                                                                                            \
    do {                                                                                                                           \
        quicly_conn_t *_conn = (conn);                                                                                             \
        if (_conn->super.ctx->debug_log != NULL) {                                                                                 \
            char buf[1024];                                                                                                        \
            int is_client = quicly_is_client(_conn);                                                                               \
            const quicly_cid_t *cid = is_client ? &conn->super.peer.cid : &conn->super.host.cid;                                   \
            char *cidhex = quicly_hexdump(cid->cid, cid->len, SIZE_MAX);                                                           \
            snprintf(buf, sizeof(buf), __VA_ARGS__);                                                                               \
            _conn->super.ctx->debug_log(_conn->super.ctx, "%s:%s,%" PRIu64 ": %s\n", is_client ? "client" : "server", cidhex,      \
                                        (uint64_t)(stream_id), buf);                                                               \
            free(cidhex);                                                                                                          \
        }                                                                                                                          \
    } while (0)

struct st_quicly_cipher_context_t {
    ptls_aead_context_t *aead;
    ptls_cipher_context_t *pne;
};

struct st_quicly_packet_protection_t {
    struct st_quicly_cipher_context_t handshake;
    struct st_quicly_cipher_context_t early_data;
    struct st_quicly_cipher_context_t one_rtt[2];
};

struct st_quicly_pending_path_challenge_t {
    struct st_quicly_pending_path_challenge_t *next;
    uint8_t is_response;
    uint8_t data[QUICLY_PATH_CHALLENGE_DATA_LEN];
};

struct st_quicly_conn_t {
    struct _st_quicly_conn_public_t super;
    /**
     * hashtable of streams
     */
    khash_t(quicly_stream_t) * streams;
    /**
     *
     */
    struct {
        /**
         * crypto parameters
         */
        struct st_quicly_packet_protection_t pp;
        /**
         * acks to be sent to peer
         */
        quicly_ranges_t ack_queue;
        /**
         *
         */
        struct {
            uint64_t bytes_consumed;
            quicly_maxsender_t sender;
        } max_data;
        /**
         *
         */
        quicly_maxsender_t max_stream_id_bidi;
        /**
         *
         */
        quicly_maxsender_t max_stream_id_uni;
        /**
         *
         */
        uint64_t next_expected_packet_number;
    } ingress;
    /**
     *
     */
    struct {
        /**
         * crypto parameters
         */
        struct st_quicly_packet_protection_t pp;
        /**
         * contains actions that needs to be performed when an ack is being received
         */
        quicly_acks_t acks;
        /**
         *
         */
        quicly_loss_t loss;
        /**
         *
         */
        uint64_t packet_number;
        /**
         *
         */
        struct {
            uint64_t permitted;
            uint64_t sent;
        } max_data;
        /**
         *
         */
        uint64_t max_stream_id_bidi;
        /**
         *
         */
        uint64_t max_stream_id_uni;
        /**
         *
         */
        quicly_sender_state_t stream_id_blocked_state;
        /**
         *
         */
        struct {
            struct st_quicly_pending_path_challenge_t *head, **tail_ref;
        } path_challenge;
        /**
         *
         */
        int64_t send_ack_at;
    } egress;
    /**
     * crypto data
     */
    struct {
        quicly_stream_t stream;
        ptls_t *tls;
        ptls_handshake_properties_t handshake_properties;
        struct {
            ptls_raw_extension_t ext[2];
            ptls_buffer_t buf;
        } transport_parameters;
        unsigned pending_control : 1;
        unsigned pending_data : 1;
    } crypto;
    /**
     *
     */
    struct {
        quicly_linklist_t control;
        quicly_linklist_t stream_fin_only;
        quicly_linklist_t stream_with_payload;
    } pending_link;
};

const quicly_context_t quicly_default_context = {
    NULL,                      /* tls */
    1280,                      /* max_packet_size */
    &quicly_loss_default_conf, /* loss */
    16384,                     /* initial_max_stream_data */
    65536,                     /* initial_max_data */
    600,                       /* idle_timeout */
    100,                       /* max_concurrent_streams_bidi */
    0,                         /* max_concurrent_streams_uni */
    {0, NULL},                 /* stateless_retry {enforce_use, key} */
    0,                         /* enforce_version_negotiation */
    quicly_default_alloc_packet,
    quicly_default_free_packet,
    quicly_default_alloc_stream,
    quicly_default_free_stream,
    NULL, /* on_stream_open */
    NULL, /* on_conn_close */
    quicly_default_now,
    NULL, /* debug_log */
};

static const quicly_transport_parameters_t transport_params_before_handshake = {16384, 16384, 10, 60, 0};

static void dispose_cipher(struct st_quicly_cipher_context_t *ctx)
{
    ptls_aead_free(ctx->aead);
    ptls_cipher_free(ctx->pne);
}

static void free_packet_protection(struct st_quicly_packet_protection_t *pp)
{
    if (pp->handshake.aead != NULL)
        dispose_cipher(&pp->handshake);
    if (pp->early_data.aead != NULL)
        dispose_cipher(&pp->early_data);
    if (pp->one_rtt[0].aead != NULL)
        dispose_cipher(&pp->one_rtt[0]);
    if (pp->one_rtt[1].aead != NULL)
        dispose_cipher(&pp->one_rtt[1]);
}

static size_t decode_cid_length(uint8_t src)
{
    return src != 0 ? src + 3 : 0;
}

static uint8_t encode_cid_length(size_t len)
{
    return len != 0 ? (uint8_t)len - 3 : 0;
}

size_t quicly_decode_packet(quicly_decoded_packet_t *packet, const uint8_t *src, size_t len, size_t host_cidl)
{
    const uint8_t *src_end = src + len;

    if (len < 2)
        goto Error;

    packet->octets = ptls_iovec_init(src, len);
    packet->datagram_size = len;
    ++src;

    if (!QUICLY_PACKET_TYPE_IS_1RTT(packet->octets.base[0])) {
        /* long header */
        uint64_t rest_length;
        if (src_end - src < 5)
            goto Error;
        packet->version = quicly_decode32(&src);
        packet->cid.dest.len = decode_cid_length(*src >> 4);
        packet->cid.src.len = decode_cid_length(*src & 0xf);
        ++src;
        if (src_end - src < packet->cid.dest.len + packet->cid.src.len)
            goto Error;
        packet->cid.dest.base = (void *)src;
        src += packet->cid.dest.len;
        packet->cid.src.base = (void *)src;
        src += packet->cid.src.len;
        if (packet->version == 0) {
            /* version negotiation packet does not have the length field nor is ever coalesced */
            packet->encrypted_off = src - packet->octets.base;
        } else {
            if ((rest_length = quicly_decodev(&src, src_end)) == UINT64_MAX)
                goto Error;
            if (rest_length < 1)
                goto Error;
            if (src_end - src < rest_length)
                goto Error;
            packet->encrypted_off = src - packet->octets.base;
            packet->octets.len = packet->encrypted_off + rest_length;
        }
    } else {
        /* short header */
        if (host_cidl != 0) {
            if (src_end - src < host_cidl)
                goto Error;
            packet->cid.dest = ptls_iovec_init(src, host_cidl);
            src += host_cidl;
        } else {
            packet->cid.dest = ptls_iovec_init(NULL, 0);
        }
        packet->cid.src = ptls_iovec_init(NULL, 0);
        packet->version = 0;
        packet->encrypted_off = src - packet->octets.base;
    }

    return packet->octets.len;

Error:
    return SIZE_MAX;
}

uint64_t quicly_determine_packet_number(uint32_t bits, uint32_t mask, uint64_t next_expected)
{
    uint64_t actual = (next_expected & ~(uint64_t)mask) + bits;

    if (((bits - (uint32_t)next_expected) & mask) > (mask >> 1)) {
        if (actual >= (uint64_t)mask + 1)
            actual -= (uint64_t)mask + 1;
    }

    return actual;
}

int quicly_connection_is_ready(quicly_conn_t *conn)
{
    return conn->egress.pp.one_rtt[0].aead != NULL || conn->egress.pp.early_data.aead != NULL;
}

static int set_peeraddr(quicly_conn_t *conn, struct sockaddr *addr, socklen_t addrlen)
{
    int ret;

    if (conn->super.peer.salen != addrlen) {
        struct sockaddr *newsa;
        if ((newsa = malloc(addrlen)) == NULL) {
            ret = PTLS_ERROR_NO_MEMORY;
            goto Exit;
        }
        free(conn->super.peer.sa);
        conn->super.peer.sa = newsa;
        conn->super.peer.salen = addrlen;
    }

    memcpy(conn->super.peer.sa, addr, addrlen);
    ret = 0;

Exit:
    return ret;
}

static void sched_stream_control(quicly_stream_t *stream)
{
    if (stream->stream_id != 0) {
        if (!quicly_linklist_is_linked(&stream->_send_aux.pending_link.control))
            quicly_linklist_insert(&stream->conn->pending_link.control, &stream->_send_aux.pending_link.control);
    } else {
        stream->conn->crypto.pending_control = 1;
    }
}

static void resched_stream_data(quicly_stream_t *stream)
{
    quicly_linklist_t *target = NULL;

    if (stream->stream_id == 0) {
        stream->conn->crypto.pending_data = 1;
        return;
    }

    /* unlink so that we would round-robin the streams */
    if (quicly_linklist_is_linked(&stream->_send_aux.pending_link.stream))
        quicly_linklist_unlink(&stream->_send_aux.pending_link.stream);

    if (stream->sendbuf.pending.num_ranges != 0) {
        if (stream->sendbuf.pending.ranges[0].start == stream->sendbuf.eos) {
            /* fin is the only thing to be sent, and it can be sent if window size is zero */
            target = &stream->conn->pending_link.stream_fin_only;
        } else {
            /* check if we can send payload */
            if (stream->sendbuf.pending.ranges[0].start < stream->_send_aux.max_stream_data)
                target = &stream->conn->pending_link.stream_with_payload;
        }
    }

    if (target != NULL)
        quicly_linklist_insert(target, &stream->_send_aux.pending_link.stream);
}

static int stream_id_blocked(quicly_conn_t *conn, int uni)
{
    uint64_t *next_id = uni ? &conn->super.host.next_stream_id_uni : &conn->super.host.next_stream_id_bidi,
             *max_id = uni ? &conn->egress.max_stream_id_uni : &conn->egress.max_stream_id_bidi;
    return *next_id > *max_id;
}

static int should_update_max_stream_data(quicly_stream_t *stream)
{
    return quicly_maxsender_should_update(&stream->_send_aux.max_stream_data_sender, stream->recvbuf.data_off,
                                          stream->_recv_aux.window, 512);
}

static void on_sendbuf_change(quicly_sendbuf_t *buf)
{
    quicly_stream_t *stream = (void *)((char *)buf - offsetof(quicly_stream_t, sendbuf));
    assert(stream->stream_id != 0 || buf->eos == UINT64_MAX);

    resched_stream_data(stream);
}

static void on_recvbuf_change(quicly_recvbuf_t *buf, size_t shift_amount)
{
    quicly_stream_t *stream = (void *)((char *)buf - offsetof(quicly_stream_t, recvbuf));

    if (stream->stream_id != 0) {
        stream->conn->ingress.max_data.bytes_consumed += shift_amount;
        if (should_update_max_stream_data(stream))
            sched_stream_control(stream);
    }
}

static int schedule_path_challenge(quicly_conn_t *conn, int is_response, const uint8_t *data)
{
    struct st_quicly_pending_path_challenge_t *pending;

    if ((pending = malloc(sizeof(struct st_quicly_pending_path_challenge_t))) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    pending->next = NULL;
    pending->is_response = is_response;
    memcpy(pending->data, data, QUICLY_PATH_CHALLENGE_DATA_LEN);

    *conn->egress.path_challenge.tail_ref = pending;
    conn->egress.path_challenge.tail_ref = &pending->next;
    return 0;
}

static void init_stream_properties(quicly_stream_t *stream)
{
    quicly_sendbuf_init(&stream->sendbuf, on_sendbuf_change);
    quicly_recvbuf_init(&stream->recvbuf, on_recvbuf_change);

    stream->_send_aux.max_stream_data = stream->conn->super.peer.transport_params.initial_max_stream_data;
    stream->_send_aux.max_sent = 0;
    stream->_send_aux.stop_sending.sender_state = QUICLY_SENDER_STATE_NONE;
    stream->_send_aux.stop_sending.reason = 0;
    stream->_send_aux.rst.sender_state = QUICLY_SENDER_STATE_NONE;
    stream->_send_aux.rst.reason = 0;
    quicly_maxsender_init(&stream->_send_aux.max_stream_data_sender, stream->conn->super.ctx->initial_max_stream_data);
    quicly_linklist_init(&stream->_send_aux.pending_link.control);
    quicly_linklist_init(&stream->_send_aux.pending_link.stream);

    stream->_recv_aux.window = stream->conn->super.ctx->initial_max_stream_data;
    stream->_recv_aux.rst_reason = QUICLY_ERROR_FIN_CLOSED;
}

static void dispose_stream_properties(quicly_stream_t *stream)
{
    quicly_sendbuf_dispose(&stream->sendbuf);
    quicly_recvbuf_dispose(&stream->recvbuf);
    quicly_maxsender_dispose(&stream->_send_aux.max_stream_data_sender);
    quicly_linklist_unlink(&stream->_send_aux.pending_link.control);
    quicly_linklist_unlink(&stream->_send_aux.pending_link.stream);
}

static void init_stream(quicly_stream_t *stream, quicly_conn_t *conn, uint64_t stream_id)
{
    stream->conn = conn;
    stream->stream_id = stream_id;

    int r;
    khiter_t iter = kh_put(quicly_stream_t, conn->streams, stream_id, &r);
    assert(iter != kh_end(conn->streams));
    kh_val(conn->streams, iter) = stream;

    init_stream_properties(stream);
}

static void reinit_stream_properties(quicly_stream_t *stream)
{
    dispose_stream_properties(stream);
    init_stream_properties(stream);
}

static quicly_stream_t *open_stream(quicly_conn_t *conn, uint64_t stream_id)
{
    quicly_stream_t *stream;

    if ((stream = conn->super.ctx->alloc_stream(conn->super.ctx)) == NULL)
        return NULL;
    init_stream(stream, conn, stream_id);
    return stream;
}

static void destroy_stream(quicly_stream_t *stream)
{
    quicly_conn_t *conn = stream->conn;
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream->stream_id);
    assert(iter != kh_end(conn->streams));
    kh_del(quicly_stream_t, conn->streams, iter);

    conn->ingress.max_data.bytes_consumed += stream->recvbuf.data.len;
    dispose_stream_properties(stream);

    if (stream->stream_id != 0) {
        if (quicly_is_client(conn) == STREAM_IS_CLIENT_INITIATED(stream->stream_id)) {
            --conn->super.host.num_streams;
        } else {
            --conn->super.peer.num_streams;
        }
        conn->super.ctx->free_stream(stream);
    }
}

quicly_stream_t *quicly_get_stream(quicly_conn_t *conn, uint64_t stream_id)
{
    khiter_t iter = kh_get(quicly_stream_t, conn->streams, stream_id);
    if (iter != kh_end(conn->streams))
        return kh_val(conn->streams, iter);
    return NULL;
}

void quicly_get_max_data(quicly_conn_t *conn, uint64_t *send_permitted, uint64_t *sent, uint64_t *consumed)
{
    if (send_permitted != NULL)
        *send_permitted = conn->egress.max_data.permitted;
    if (sent != NULL)
        *sent = conn->egress.max_data.sent;
    if (consumed != NULL)
        *consumed = conn->ingress.max_data.bytes_consumed;
}

void quicly_free(quicly_conn_t *conn)
{
    quicly_stream_t *stream;

    free_packet_protection(&conn->ingress.pp);
    quicly_ranges_dispose(&conn->ingress.ack_queue);
    quicly_maxsender_dispose(&conn->ingress.max_data.sender);
    quicly_maxsender_dispose(&conn->ingress.max_stream_id_bidi);
    quicly_maxsender_dispose(&conn->ingress.max_stream_id_uni);
    free_packet_protection(&conn->egress.pp);
    quicly_acks_dispose(&conn->egress.acks);
    while (conn->egress.path_challenge.head != NULL) {
        struct st_quicly_pending_path_challenge_t *pending = conn->egress.path_challenge.head;
        conn->egress.path_challenge.head = pending->next;
        free(pending);
    }

    kh_foreach_value(conn->streams, stream, { destroy_stream(stream); });
    kh_destroy(quicly_stream_t, conn->streams);

    assert(!quicly_linklist_is_linked(&conn->pending_link.control));
    assert(!quicly_linklist_is_linked(&conn->pending_link.stream_fin_only));
    assert(!quicly_linklist_is_linked(&conn->pending_link.stream_with_payload));

    free(conn->super.peer.sa);
    free(conn);
}

static int qhkdf_expand(ptls_hash_algorithm_t *algo, void *output, size_t outlen, const void *secret, const char *label)
{
    ptls_buffer_t hkdf_label;
    uint8_t hkdf_label_buf[16];
    int ret;

    ptls_buffer_init(&hkdf_label, hkdf_label_buf, sizeof(hkdf_label_buf));

    ptls_buffer_push16(&hkdf_label, (uint16_t)outlen);
    ptls_buffer_push_block(&hkdf_label, 1, {
        const char *base_label = "QUIC ";
        ptls_buffer_pushv(&hkdf_label, base_label, strlen(base_label));
        ptls_buffer_pushv(&hkdf_label, label, strlen(label));
    });

    ret = ptls_hkdf_expand(algo, output, outlen, ptls_iovec_init(secret, algo->digest_size),
                           ptls_iovec_init(hkdf_label.base, hkdf_label.off));

Exit:
    ptls_buffer_dispose(&hkdf_label);
    return ret;
}

static int setup_cipher(struct st_quicly_cipher_context_t *ctx, ptls_aead_algorithm_t *aead, ptls_hash_algorithm_t *hash,
                        int is_enc, const void *secret)
{
    uint8_t key[PTLS_MAX_SECRET_SIZE];
    int ret;

    *ctx = (struct st_quicly_cipher_context_t){NULL};

    if ((ret = qhkdf_expand(hash, key, aead->key_size, secret, "key")) != 0)
        goto Exit;
    if ((ctx->aead = ptls_aead_new(aead, is_enc, key)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    if ((ret = qhkdf_expand(hash, ctx->aead->static_iv, aead->iv_size, secret, "iv")) != 0)
        goto Exit;

    if ((ret = qhkdf_expand(hash, key, aead->ctr_cipher->key_size, secret, "pn")) != 0)
        goto Exit;
    if ((ctx->pne = ptls_cipher_new(aead->ctr_cipher, is_enc, key)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }

    ret = 0;
Exit:
    if (ret != 0) {
        if (ctx->aead != NULL) {
            ptls_aead_free(ctx->aead);
            ctx->aead = NULL;
        }
        if (ctx->pne != NULL) {
            ptls_cipher_free(ctx->pne);
            ctx->pne = NULL;
        }
    }
    ptls_clear_memory(key, sizeof(key));
    return ret;
}

static int setup_handshake_secret(struct st_quicly_cipher_context_t *ctx, ptls_cipher_suite_t *cs, const void *master_secret,
                                  const char *label, int is_enc)
{
    uint8_t aead_secret[PTLS_MAX_DIGEST_SIZE];
    int ret;

    if ((ret = qhkdf_expand(cs->hash, aead_secret, cs->hash->digest_size, master_secret, label)) != 0)
        goto Exit;
    if (QUICLY_DEBUG) {
        char *aead_secret_hex = quicly_hexdump(aead_secret, cs->hash->digest_size, SIZE_MAX);
        fprintf(stderr, "%s: label: \"%s\", aead-secret: %s\n", __FUNCTION__, label, aead_secret_hex);
        free(aead_secret_hex);
    }
    if ((ret = setup_cipher(ctx, cs->aead, cs->hash, is_enc, aead_secret)) != 0)
        goto Exit;

Exit:
    ptls_clear_memory(aead_secret, sizeof(aead_secret));
    return ret;
}

static int setup_handshake_encryption(struct st_quicly_cipher_context_t *ingress, struct st_quicly_cipher_context_t *egress,
                                      ptls_cipher_suite_t **cipher_suites, ptls_iovec_t cid, int is_client)
{
    static const uint8_t salt[] = {0x9c, 0x10, 0x8f, 0x98, 0x52, 0x0a, 0x5c, 0x5c, 0x32, 0x96,
                                   0x8e, 0x95, 0x0e, 0x8a, 0x2c, 0x5f, 0xe0, 0x6d, 0x6c, 0x38};
    static const char *labels[2] = {"client hs", "server hs"};
    ptls_cipher_suite_t **cs;
    uint8_t secret[PTLS_MAX_DIGEST_SIZE];
    int ret;

    /* find aes128gcm cipher */
    for (cs = cipher_suites;; ++cs) {
        assert(cs != NULL);
        if ((*cs)->id == PTLS_CIPHER_SUITE_AES_128_GCM_SHA256)
            break;
    }

    /* extract master secret */
    if ((ret = ptls_hkdf_extract((*cs)->hash, secret, ptls_iovec_init(salt, sizeof(salt)), cid)) != 0)
        goto Exit;
    if (QUICLY_DEBUG) {
        char *cid_hex = quicly_hexdump(cid.base, cid.len, SIZE_MAX),
             *secret_hex = quicly_hexdump(secret, (*cs)->hash->digest_size, SIZE_MAX);
        fprintf(stderr, "%s: cid: %s -> secret: %s\n", __FUNCTION__, cid_hex, secret_hex);
        free(cid_hex);
        free(secret_hex);
    }

    /* create aead contexts */
    if ((ret = setup_handshake_secret(ingress, *cs, secret, labels[is_client], 0)) != 0)
        goto Exit;
    if ((ret = setup_handshake_secret(egress, *cs, secret, labels[!is_client], 1)) != 0)
        goto Exit;

Exit:
    ptls_clear_memory(secret, sizeof(secret));
    return ret;
}

static int setup_secret(struct st_quicly_packet_protection_t *pp, ptls_t *tls, const char *label, int is_early, int is_enc)
{
    ptls_cipher_suite_t *cipher = ptls_get_cipher(tls);
    struct st_quicly_cipher_context_t *ctx = is_early ? &pp->early_data : &pp->one_rtt[0];
    uint8_t secret[PTLS_MAX_DIGEST_SIZE];
    int ret;

    if ((ret = ptls_export_secret(tls, secret, cipher->hash->digest_size, label, ptls_iovec_init(NULL, 0), is_early)) != 0)
        goto Exit;
    if ((ret = setup_cipher(ctx, cipher->aead, cipher->hash, is_enc, secret)) != 0)
        goto Exit;

Exit:
    ptls_clear_memory(secret, sizeof(secret));
    return ret;
}

static void apply_peer_transport_params(quicly_conn_t *conn)
{
    conn->egress.max_data.permitted = conn->super.peer.transport_params.initial_max_data;
    conn->egress.max_stream_id_bidi =
        conn->super.peer.transport_params.initial_max_streams_bidi * 4 - (quicly_is_client(conn) ? 0 : 3);
    conn->egress.max_stream_id_uni =
        conn->super.peer.transport_params.initial_max_streams_uni * 4 + (quicly_is_client(conn) ? 2 : 1);
}

static int setup_1rtt(quicly_conn_t *conn, ptls_t *tls)
{
    static const char *labels[2] = {"EXPORTER-QUIC client 1rtt", "EXPORTER-QUIC server 1rtt"};
    int ret;

    /* release early-data AEAD for egress, but not for ingress since they can arrive later */
    if (conn->egress.pp.early_data.aead != NULL) {
        dispose_cipher(&conn->egress.pp.early_data);
        conn->egress.pp.early_data = (struct st_quicly_cipher_context_t){NULL};
    }

    if ((ret = setup_secret(&conn->ingress.pp, tls, labels[quicly_is_client(conn)], 0, 0)) != 0)
        goto Exit;
    if ((ret = setup_secret(&conn->egress.pp, tls, labels[!quicly_is_client(conn)], 0, 1)) != 0)
        goto Exit;

    apply_peer_transport_params(conn);
    conn->super.state = QUICLY_STATE_1RTT_ENCRYPTED;

Exit:
    return 0;
}

static void senddata_free(struct st_quicly_buffer_vec_t *vec)
{
    free(vec->p);
    free(vec);
}

static void write_tlsbuf(quicly_conn_t *conn, ptls_buffer_t *tlsbuf)
{
    if (tlsbuf->off != 0) {
        assert(tlsbuf->is_allocated);
        quicly_sendbuf_write(&conn->crypto.stream.sendbuf, tlsbuf->base, tlsbuf->off, senddata_free);
        ptls_buffer_init(tlsbuf, "", 0);
    } else {
        assert(!tlsbuf->is_allocated);
    }
}

static int crypto_stream_receive_post_handshake(quicly_stream_t *_stream)
{
    quicly_conn_t *conn = (void *)((char *)_stream - offsetof(quicly_conn_t, crypto.stream));
    ptls_buffer_t buf;
    ptls_iovec_t input;
    int ret = 0;

    ptls_buffer_init(&buf, "", 0);
    while ((input = quicly_recvbuf_get(&conn->crypto.stream.recvbuf)).len != 0) {
        if ((ret = ptls_receive(conn->crypto.tls, &buf, input.base, &input.len)) != 0)
            goto Exit;
        quicly_recvbuf_shift(&conn->crypto.stream.recvbuf, input.len);
        if (buf.off != 0) {
            fprintf(stderr, "ptls_receive returned application data\n");
            ret = QUICLY_ERROR_TBD;
            goto Exit;
        }
    }

Exit:
    ptls_buffer_dispose(&buf);
    return ret;
}

static int crypto_stream_receive_handshake(quicly_stream_t *_stream)
{
    quicly_conn_t *conn = (void *)((char *)_stream - offsetof(quicly_conn_t, crypto.stream));
    ptls_iovec_t input;
    ptls_buffer_t buf;
    int ret = PTLS_ERROR_IN_PROGRESS;

    ptls_buffer_init(&buf, "", 0);
    while (ret == PTLS_ERROR_IN_PROGRESS && (input = quicly_recvbuf_get(&conn->crypto.stream.recvbuf)).len != 0) {
        ret = ptls_handshake(conn->crypto.tls, &buf, input.base, &input.len, &conn->crypto.handshake_properties);
        quicly_recvbuf_shift(&conn->crypto.stream.recvbuf, input.len);
    }
    write_tlsbuf(conn, &buf);

    switch (ret) {
    case 0:
        DEBUG_LOG(conn, 0, "handshake complete");
        conn->crypto.stream.on_update = crypto_stream_receive_post_handshake;
        /* state is 1RTT_ENCRYPTED when handling ClientFinished */
        if (conn->super.state < QUICLY_STATE_1RTT_ENCRYPTED) {
            if (!quicly_is_client(conn)) {
                /* ignore error, which will be returned if early secret is unavailable */
                setup_secret(&conn->ingress.pp, conn->crypto.tls, "EXPORTER-QUIC 0rtt", 1, 0);
            }
            if ((ret = setup_1rtt(conn, conn->crypto.tls)) != 0)
                goto Exit;
        }
        if (quicly_recvbuf_get(&conn->crypto.stream.recvbuf).len != 0)
            ret = conn->crypto.stream.on_update(&conn->crypto.stream);
        break;
    case PTLS_ERROR_IN_PROGRESS:
        if (conn->super.state == QUICLY_STATE_FIRSTFLIGHT)
            conn->super.state = QUICLY_STATE_HANDSHAKE;
        assert(conn->super.state == QUICLY_STATE_HANDSHAKE);
        ret = 0;
        break;
    case PTLS_ERROR_STATELESS_RETRY:
        assert(!quicly_is_client(conn));
        assert(conn->super.state == QUICLY_STATE_FIRSTFLIGHT);
        conn->super.state = QUICLY_STATE_SEND_RETRY;
        conn->egress.packet_number = conn->ingress.next_expected_packet_number - 1;
        ret = 0;
        break;
    default:
        break;
    }

Exit:
    return ret;
}

static int crypto_stream_receive_stateless_retry(quicly_stream_t *_stream)
{
    quicly_conn_t *conn = (void *)((char *)_stream - offsetof(quicly_conn_t, crypto.stream));
    ptls_iovec_t input = quicly_recvbuf_get(&conn->crypto.stream.recvbuf);
    size_t consumed = input.len;
    ptls_buffer_t buf;
    int ret;

    ptls_buffer_init(&buf, "", 0);

    /* should have received HRR */
    ret = ptls_handshake(conn->crypto.tls, &buf, input.base, &consumed, &conn->crypto.handshake_properties);
    quicly_recvbuf_shift(&conn->crypto.stream.recvbuf, consumed);
    if (ret != PTLS_ERROR_IN_PROGRESS)
        goto Error;
    if (input.len != consumed)
        goto Error;
    if (buf.off == 0)
        goto Error;

    /* send the 2nd ClientHello, reinitializing the transport */
    dispose_cipher(&conn->egress.pp.handshake);
    if ((ret = setup_handshake_encryption(&conn->ingress.pp.handshake, &conn->egress.pp.handshake,
                                          conn->super.ctx->tls->cipher_suites,
                                          ptls_iovec_init(conn->super.peer.cid.cid, conn->super.peer.cid.len), 1)) != 0)
        goto Error;
    quicly_acks_dispose(&conn->egress.acks);
    quicly_acks_init(&conn->egress.acks);
    reinit_stream_properties(&conn->crypto.stream);
    conn->crypto.stream.on_update = crypto_stream_receive_handshake;
    write_tlsbuf(conn, &buf);

    return 0;

Error:
    ptls_buffer_dispose(&buf);
    return QUICLY_ERROR_TBD;
}

static int do_apply_stream_frame(quicly_stream_t *stream, uint64_t off, ptls_iovec_t data)
{
    int ret;

    /* adjust the range of supplied data so that we not go above eos */
    if (stream->recvbuf.eos < off)
        return 0;
    if (stream->recvbuf.eos < off + data.len)
        data.len = stream->recvbuf.eos - off;

    /* make adjustments for retransmit */
    if (off < stream->recvbuf.data_off) {
        if (off + data.len <= stream->recvbuf.data_off)
            return 0;
        size_t delta = stream->recvbuf.data_off - off;
        off = stream->recvbuf.data_off;
        data.base += delta;
        data.len -= delta;
    }

    /* try the fast (copyless) path */
    if (stream->recvbuf.data_off == off && stream->recvbuf.data.len == 0) {
        struct st_quicly_buffer_vec_t vec = {NULL};
        assert(stream->recvbuf.received.num_ranges == 1);
        assert(stream->recvbuf.received.ranges[0].end == stream->recvbuf.data_off);

        if (data.len != 0) {
            stream->recvbuf.received.ranges[0].end += data.len;
            quicly_buffer_set_fast_external(&stream->recvbuf.data, &vec, data.base, data.len);
        }
        if ((ret = stream->on_update(stream)) != 0)
            return ret;
        /* stream might have been destroyed; in such case vec.len would be zero (see quicly_buffer_dispose) */
        if (vec.len != 0 && stream->recvbuf.data.len != 0) {
            size_t keeplen = stream->recvbuf.data.len;
            quicly_buffer_init(&stream->recvbuf.data);
            if ((ret = quicly_buffer_push(&stream->recvbuf.data, data.base + data.len - keeplen, keeplen, NULL)) != 0)
                return ret;
        }
        return 0;
    }

    uint64_t prev_end = stream->recvbuf.received.ranges[0].end;
    if ((ret = quicly_recvbuf_write(&stream->recvbuf, off, data.base, data.len)) != 0)
        return ret;
    if (prev_end != stream->recvbuf.received.ranges[0].end || prev_end == stream->recvbuf.eos)
        ret = stream->on_update(stream);
    return ret;
}

static int apply_stream_frame(quicly_stream_t *stream, quicly_stream_frame_t *frame)
{
    int ret;

    DEBUG_LOG(stream->conn, stream->stream_id, "received; off=%" PRIu64 ",len=%zu", frame->offset, frame->data.len);

    if (frame->is_fin && (ret = quicly_recvbuf_mark_eos(&stream->recvbuf, frame->offset + frame->data.len)) != 0)
        return ret;
    if ((ret = do_apply_stream_frame(stream, frame->offset, frame->data)) != 0)
        return ret;
    if (should_update_max_stream_data(stream))
        sched_stream_control(stream);

    return ret;
}

#define PUSH_TRANSPORT_PARAMETER(buf, id, block)                                                                                   \
    do {                                                                                                                           \
        ptls_buffer_push16((buf), (id));                                                                                           \
        ptls_buffer_push_block((buf), 2, block);                                                                                   \
    } while (0)

static int encode_transport_parameter_list(quicly_context_t *ctx, ptls_buffer_t *buf, int is_client)
{
    int ret;

    ptls_buffer_push_block(buf, 2, {
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA,
                                 { ptls_buffer_push32(buf, ctx->initial_max_stream_data); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA,
                                 { ptls_buffer_push32(buf, ctx->initial_max_data); });
        PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT, { ptls_buffer_push16(buf, ctx->idle_timeout); });
        if (!is_client) {
            PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_STATELESS_RESET_TOKEN, {
                /* FIXME implement stateless reset */
                static const uint8_t zeroes[16] = {0};
                ptls_buffer_pushv(buf, zeroes, sizeof(zeroes));
            });
        }
        if (ctx->max_streams_bidi != 0)
            PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_BIDI,
                                     { ptls_buffer_push16(buf, ctx->max_streams_bidi); });
        if (ctx->max_streams_uni != 0) {
            PUSH_TRANSPORT_PARAMETER(buf, QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_UNI,
                                     { ptls_buffer_push16(buf, ctx->max_streams_uni); });
        }
    });
    ret = 0;
Exit:
    return ret;
}

static int decode_transport_parameter_list(quicly_transport_parameters_t *params, int is_client, const uint8_t *src,
                                           const uint8_t *end)
{
#define ID_TO_BIT(id) ((uint64_t)1 << (id))

    uint64_t found_id_bits = 0,
             must_found_id_bits = ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA) |
                                  ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT);
    int ret;

    if (is_client)
        must_found_id_bits = ID_TO_BIT(QUICLY_TRANSPORT_PARAMETER_ID_STATELESS_RESET_TOKEN);

    /* set optional parameters to their default values */
    params->initial_max_streams_bidi = 0;
    params->initial_max_streams_uni = 0;

    /* decode the parameters block */
    ptls_decode_block(src, end, 2, {
        while (src != end) {
            uint16_t id;
            if ((ret = ptls_decode16(&id, &src, end)) != 0)
                goto Exit;
            if (id < sizeof(found_id_bits) * 8) {
                if ((found_id_bits & ID_TO_BIT(id)) != 0) {
                    ret = QUICLY_ERROR_TRANSPORT_PARAMETER;
                    goto Exit;
                }
                found_id_bits |= ID_TO_BIT(id);
            }
            found_id_bits |= ID_TO_BIT(id);
            ptls_decode_open_block(src, end, 2, {
                switch (id) {
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAM_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_stream_data, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_DATA:
                    if ((ret = ptls_decode32(&params->initial_max_data, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_STATELESS_RESET_TOKEN:
                    if (!is_client || end - src != STATELESS_RESET_TOKEN_SIZE) {
                        ret = QUICLY_ERROR_TRANSPORT_PARAMETER;
                        goto Exit;
                    }
                    /* TODO remember */
                    src = end;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_IDLE_TIMEOUT:
                    if ((ret = ptls_decode16(&params->idle_timeout, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_BIDI:
                    if ((ret = ptls_decode16(&params->initial_max_streams_bidi, &src, end)) != 0)
                        goto Exit;
                    break;
                case QUICLY_TRANSPORT_PARAMETER_ID_INITIAL_MAX_STREAMS_UNI:
                    if ((ret = ptls_decode16(&params->initial_max_streams_uni, &src, end)) != 0)
                        goto Exit;
                    break;
                default:
                    src = end;
                    break;
                }
            });
        }
    });

    /* check that we have found all the required parameters */
    if ((found_id_bits & must_found_id_bits) != must_found_id_bits) {
        ret = QUICLY_ERROR_TRANSPORT_PARAMETER;
        goto Exit;
    }

    ret = 0;
Exit:
    /* FIXME convert to quic error */
    return ret;

#undef ID_TO_BIT
}

static int collect_transport_parameters(ptls_t *tls, struct st_ptls_handshake_properties_t *properties, uint16_t type)
{
    return type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS;
}

static void set_cid(quicly_cid_t *dest, ptls_iovec_t src)
{
    memcpy(dest->cid, src.base, src.len);
    dest->len = src.len;
}

static quicly_conn_t *create_connection(quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                                        ptls_handshake_properties_t *handshake_properties)
{
    ptls_t *tls = NULL;
    quicly_conn_t *conn;

    if ((tls = ptls_new(ctx->tls, server_name == NULL)) == NULL)
        return NULL;
    if (server_name != NULL && ptls_set_server_name(tls, server_name, strlen(server_name)) != 0) {
        ptls_free(tls);
        return NULL;
    }
    if ((conn = malloc(sizeof(*conn))) == NULL) {
        ptls_free(tls);
        return NULL;
    }

    memset(conn, 0, sizeof(*conn));
    conn->super.ctx = ctx;
    conn->super.state = QUICLY_STATE_FIRSTFLIGHT;
    if (server_name != NULL) {
        ctx->tls->random_bytes(conn->super.peer.cid.cid, 8);
        conn->super.peer.cid.len = 8;
        conn->super.host.next_stream_id_bidi = 4;
        conn->super.host.next_stream_id_uni = 1;
        conn->super.peer.next_stream_id_bidi = 2;
        conn->super.peer.next_stream_id_uni = 3;
    } else {
        conn->super.host.next_stream_id_bidi = 2;
        conn->super.host.next_stream_id_uni = 3;
        conn->super.peer.next_stream_id_bidi = 4;
        conn->super.peer.next_stream_id_uni = 1;
    }
    conn->super.peer.transport_params = transport_params_before_handshake;
    if (server_name != NULL && ctx->enforce_version_negotiation) {
        ctx->tls->random_bytes(&conn->super.version, sizeof(conn->super.version));
        conn->super.version = (conn->super.version & 0xf0f0f0f0) | 0x0a0a0a0a;
    } else {
        conn->super.version = QUICLY_PROTOCOL_VERSION;
    }
    conn->streams = kh_init(quicly_stream_t);
    quicly_ranges_init(&conn->ingress.ack_queue);
    quicly_maxsender_init(&conn->ingress.max_data.sender, conn->super.ctx->initial_max_data);
    quicly_maxsender_init(&conn->ingress.max_stream_id_bidi,
                          conn->super.ctx->max_streams_bidi * 4 + conn->super.peer.next_stream_id_bidi);
    quicly_maxsender_init(&conn->ingress.max_stream_id_uni,
                          conn->super.ctx->max_streams_uni * 4 + conn->super.peer.next_stream_id_uni);
    quicly_acks_init(&conn->egress.acks);
    quicly_loss_init(&conn->egress.loss, conn->super.ctx->loss,
                     conn->super.ctx->loss->default_initial_rtt /* FIXME remember initial_rtt in session ticket */);
    conn->egress.path_challenge.tail_ref = &conn->egress.path_challenge.head;
    conn->egress.send_ack_at = INT64_MAX;
    init_stream(&conn->crypto.stream, conn, 0);
    conn->crypto.stream.on_update = crypto_stream_receive_handshake;
    conn->crypto.tls = tls;
    if (handshake_properties != NULL) {
        assert(handshake_properties->additional_extensions == NULL);
        assert(handshake_properties->collect_extension == NULL);
        assert(handshake_properties->collected_extensions == NULL);
        conn->crypto.handshake_properties = *handshake_properties;
    } else {
        conn->crypto.handshake_properties = (ptls_handshake_properties_t){{{{NULL}}}};
    }
    conn->crypto.handshake_properties.collect_extension = collect_transport_parameters;
    quicly_linklist_init(&conn->pending_link.control);
    quicly_linklist_init(&conn->pending_link.stream_fin_only);
    quicly_linklist_init(&conn->pending_link.stream_with_payload);

    if (set_peeraddr(conn, sa, salen) != 0) {
        quicly_free(conn);
        return NULL;
    }

    return conn;
}

static int client_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    quicly_conn_t *conn = (void *)((char *)properties - offsetof(quicly_conn_t, crypto.handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = 0; // allow abcense of the extension for the time being PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;

    uint32_t negotiated_version;
    if ((ret = ptls_decode32(&negotiated_version, &src, end)) != 0)
        goto Exit;
    if (negotiated_version != QUICLY_PROTOCOL_VERSION) {
        fprintf(stderr, "unexpected negotiated version\n");
        ret = QUICLY_ERROR_TBD;
        goto Exit;
    }

    ptls_decode_open_block(src, end, 1, {
        int found_negotiated_version = 0;
        do {
            uint32_t supported_version;
            if ((ret = ptls_decode32(&supported_version, &src, end)) != 0)
                goto Exit;
            if (supported_version == negotiated_version)
                found_negotiated_version = 1;
        } while (src != end);
        if (!found_negotiated_version) {
            ret = PTLS_ALERT_ILLEGAL_PARAMETER; /* FIXME is this the correct error code? */
            goto Exit;
        }
    });
    ret = decode_transport_parameter_list(&conn->super.peer.transport_params, 1, src, end);

Exit:
    return ret;
}

int quicly_connect(quicly_conn_t **_conn, quicly_context_t *ctx, const char *server_name, struct sockaddr *sa, socklen_t salen,
                   ptls_handshake_properties_t *handshake_properties)
{
    quicly_conn_t *conn;
    const quicly_cid_t *server_cid;
    ptls_buffer_t buf;
    size_t max_early_data_size;
    int ret;

    if ((conn = create_connection(ctx, server_name, sa, salen, handshake_properties)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    server_cid = quicly_get_peer_cid(conn);
    if ((ret = setup_handshake_encryption(&conn->ingress.pp.handshake, &conn->egress.pp.handshake, ctx->tls->cipher_suites,
                                          ptls_iovec_init(server_cid->cid, server_cid->len), 1)) != 0)
        goto Exit;

    /* handshake */
    ptls_buffer_init(&conn->crypto.transport_parameters.buf, "", 0);
    ptls_buffer_push32(&conn->crypto.transport_parameters.buf, conn->super.version);
    if ((ret = encode_transport_parameter_list(conn->super.ctx, &conn->crypto.transport_parameters.buf, 1)) != 0)
        goto Exit;
    conn->crypto.transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {conn->crypto.transport_parameters.buf.base, conn->crypto.transport_parameters.buf.off}};
    conn->crypto.transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    conn->crypto.handshake_properties.additional_extensions = conn->crypto.transport_parameters.ext;
    conn->crypto.handshake_properties.collected_extensions = client_collected_extensions;

    ptls_buffer_init(&buf, "", 0);
    conn->crypto.handshake_properties.client.max_early_data_size = &max_early_data_size;
    ret = ptls_handshake(conn->crypto.tls, &buf, NULL, 0, &conn->crypto.handshake_properties);
    conn->crypto.handshake_properties.client.max_early_data_size = NULL;
    if (ret != PTLS_ERROR_IN_PROGRESS)
        goto Exit;
    write_tlsbuf(conn, &buf);

    if (max_early_data_size != 0) {
        if ((ret = setup_secret(&conn->egress.pp, conn->crypto.tls, "EXPORTER-QUIC 0rtt", 1, 1)) != 0)
            goto Exit;
        apply_peer_transport_params(conn);
    }

    *_conn = conn;
    ret = 0;

Exit:
    if (ret != 0) {
        if (conn != NULL)
            quicly_free(conn);
    }
    return ret;
}

static int server_collected_extensions(ptls_t *tls, ptls_handshake_properties_t *properties, ptls_raw_extension_t *slots)
{
    quicly_conn_t *conn = (void *)((char *)properties - offsetof(quicly_conn_t, crypto.handshake_properties));
    int ret;

    if (slots[0].type == UINT16_MAX) {
        ret = 0; // allow abcense of the extension for the time being PTLS_ALERT_MISSING_EXTENSION;
        goto Exit;
    }
    assert(slots[0].type == QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS);
    assert(slots[1].type == UINT16_MAX);

    { /* decode transport_parameters extension */
        const uint8_t *src = slots[0].data.base, *end = src + slots[0].data.len;
        uint32_t initial_version;
        if ((ret = ptls_decode32(&initial_version, &src, end)) != 0)
            goto Exit;
        /* TODO we need to check initial_version when supporting multiple versions */
        if ((ret = decode_transport_parameter_list(&conn->super.peer.transport_params, 0, src, end)) != 0)
            goto Exit;
    }

    /* set transport_parameters extension to be sent in EE */
    assert(properties->additional_extensions == NULL);
    ptls_buffer_init(&conn->crypto.transport_parameters.buf, "", 0);
    ptls_buffer_push32(&conn->crypto.transport_parameters.buf, QUICLY_PROTOCOL_VERSION);
    ptls_buffer_push_block(&conn->crypto.transport_parameters.buf, 1,
                           { ptls_buffer_push32(&conn->crypto.transport_parameters.buf, QUICLY_PROTOCOL_VERSION); });
    if ((ret = encode_transport_parameter_list(conn->super.ctx, &conn->crypto.transport_parameters.buf, 0)) != 0)
        goto Exit;
    properties->additional_extensions = conn->crypto.transport_parameters.ext;
    conn->crypto.transport_parameters.ext[0] =
        (ptls_raw_extension_t){QUICLY_TLS_EXTENSION_TYPE_TRANSPORT_PARAMETERS,
                               {conn->crypto.transport_parameters.buf.base, conn->crypto.transport_parameters.buf.off}};
    conn->crypto.transport_parameters.ext[1] = (ptls_raw_extension_t){UINT16_MAX};
    conn->crypto.handshake_properties.additional_extensions = conn->crypto.transport_parameters.ext;

    ret = 0;

Exit:
    return ret;
}

static ptls_iovec_t decrypt_packet(struct st_quicly_cipher_context_t *ctx, quicly_decoded_packet_t *packet,
                                   uint64_t *next_expected_pn)
{
    size_t encrypted_len = packet->octets.len - packet->encrypted_off, iv_offset;
    uint8_t pnbuf[4];
    uint32_t pnbits, pnmask;
    size_t pnlen;

    /* determine the IV offset of pne */
    if (encrypted_len < ctx->pne->algo->iv_size + 1) {
        goto Error;
    } else if (encrypted_len < ctx->pne->algo->iv_size + 4) {
        iv_offset = packet->octets.len - ctx->pne->algo->iv_size;
    } else {
        iv_offset = packet->encrypted_off + 4;
    }

    /* decrypt PN */
    ptls_cipher_init(ctx->pne, packet->octets.base + iv_offset);
    ptls_cipher_encrypt(ctx->pne, pnbuf, packet->octets.base + packet->encrypted_off, sizeof(pnbuf));
    if ((pnbuf[0] & 0x80) == 0) {
        pnbits = pnbuf[0];
        pnmask = 0x7f;
        pnlen = 1;
    } else {
        pnbits = ((uint32_t)(pnbuf[0] & 0x3f) << 8) | pnbuf[1];
        if ((pnbuf[0] & 0x40) == 0) {
            pnmask = 0x3fff;
            pnlen = 2;
        } else {
            pnbits = (pnbits << 16) | ((uint32_t)pnbuf[2] << 8) | pnbuf[3];
            pnmask = 0x3fffffff;
            pnlen = 4;
        }
    }

    /* write-back the decrypted PN for AEAD */
    memcpy(packet->octets.base + packet->encrypted_off, pnbuf, pnlen);

    /* AEAD */
    uint64_t pn = quicly_determine_packet_number(pnbits, pnmask, *next_expected_pn);
    size_t aead_off = packet->encrypted_off + pnlen, ptlen;
    if ((ptlen = ptls_aead_decrypt(ctx->aead, packet->octets.base + aead_off, packet->octets.base + aead_off,
                                   packet->octets.len - aead_off, pn, packet->octets.base, aead_off)) == SIZE_MAX) {
        if (QUICLY_DEBUG)
            fprintf(stderr, "%s: aead decryption failure\n", __FUNCTION__);
        goto Error;
    }

    if (QUICLY_DEBUG) {
        char *payload_hex = quicly_hexdump(packet->octets.base + aead_off, ptlen, 4);
        fprintf(stderr, "%s: AEAD payload:\n%s", __FUNCTION__, payload_hex);
        free(payload_hex);
    }

    if (*next_expected_pn <= pn)
        *next_expected_pn = pn + 1;
    return ptls_iovec_init(packet->octets.base + aead_off, ptlen);

Error:
    return ptls_iovec_init(NULL, 0);
}

int quicly_accept(quicly_conn_t **_conn, quicly_context_t *ctx, struct sockaddr *sa, socklen_t salen,
                  ptls_handshake_properties_t *handshake_properties, quicly_decoded_packet_t *packet)
{
    quicly_conn_t *conn = NULL;
    struct st_quicly_cipher_context_t ingress_cipher = {NULL}, egress_cipher = {NULL};
    ptls_iovec_t payload;
    uint64_t next_expected_pn;
    quicly_stream_frame_t frame;
    int ret;

    /* ignore any packet that does not  */
    if (packet->octets.base[0] != QUICLY_PACKET_TYPE_INITIAL) {
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }
    if (packet->version != QUICLY_PROTOCOL_VERSION) {
        ret = QUICLY_ERROR_VERSION_NEGOTIATION;
        goto Exit;
    }
    if (packet->cid.dest.len < 8) {
        ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
        goto Exit;
    }
    if ((ret = setup_handshake_encryption(&ingress_cipher, &egress_cipher, ctx->tls->cipher_suites, packet->cid.dest, 0)) != 0)
        goto Exit;
    next_expected_pn = 0; /* is this correct? do we need to take care of underflow? */
    if ((payload = decrypt_packet(&ingress_cipher, packet, &next_expected_pn)).base == NULL) {
        ret = QUICLY_ERROR_TBD;
        goto Exit;
    }

    {
        const uint8_t *src = payload.base, *end = src + payload.len;
        uint8_t type_flags;
        for (; src < end; ++src) {
            if (*src != QUICLY_FRAME_TYPE_PADDING)
                break;
        }
        if (src == end || ((type_flags = *src++) & ~QUICLY_FRAME_TYPE_STREAM_BITS) != QUICLY_FRAME_TYPE_STREAM_BASE) {
            ret = QUICLY_ERROR_TBD;
            goto Exit;
        }
        if ((ret = quicly_decode_stream_frame(type_flags, &src, end, &frame)) != 0)
            goto Exit;
        if (!(frame.stream_id == 0 && frame.offset == 0)) {
            ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
            goto Exit;
        }
        /* FIXME check packet size */
        for (; src < end; ++src) {
            if (*src != QUICLY_FRAME_TYPE_PADDING) {
                ret = QUICLY_ERROR_TBD;
                goto Exit;
            }
        }
    }

    if ((conn = create_connection(ctx, NULL, sa, salen, handshake_properties)) == NULL) {
        ret = PTLS_ERROR_NO_MEMORY;
        goto Exit;
    }
    set_cid(&conn->super.peer.cid, packet->cid.src);
    /* TODO let the app set host cid after successful return from quicly_accept / quicly_connect */
    ctx->tls->random_bytes(conn->super.host.cid.cid, 8);
    conn->super.host.cid.len = 8;
    set_cid(&conn->super.host.offered_cid, packet->cid.dest);
    conn->ingress.pp.handshake = ingress_cipher;
    ingress_cipher = (struct st_quicly_cipher_context_t){NULL};
    conn->egress.pp.handshake = egress_cipher;
    egress_cipher = (struct st_quicly_cipher_context_t){NULL};
    conn->crypto.handshake_properties.collected_extensions = server_collected_extensions;
    /* TODO should there be a way to set use of stateless reset per SNI or something? */
    if (ctx->stateless_retry.enforce_use) {
        conn->crypto.handshake_properties.server.enforce_retry = 1;
        conn->crypto.handshake_properties.server.retry_uses_cookie = 1;
    }
    conn->crypto.handshake_properties.server.cookie.key = ctx->stateless_retry.key;

    if ((ret = quicly_ranges_update(&conn->ingress.ack_queue, next_expected_pn - 1, next_expected_pn)) != 0)
        goto Exit;
    assert(conn->egress.send_ack_at == INT64_MAX);
    conn->egress.send_ack_at = conn->super.ctx->now(conn->super.ctx) + QUICLY_DELAYED_ACK_TIMEOUT;
    conn->ingress.next_expected_packet_number = next_expected_pn;

    if ((ret = apply_stream_frame(&conn->crypto.stream, &frame)) != 0)
        goto Exit;
    if (conn->crypto.stream.recvbuf.data_off != frame.data.len) {
        /* garbage after clienthello? */
        ret = QUICLY_ERROR_TBD;
        goto Exit;
    }

    *_conn = conn;

Exit:
    if (!(ret == 0 || ret == PTLS_ERROR_IN_PROGRESS)) {
        if (conn != NULL)
            quicly_free(conn);
        if (ingress_cipher.aead != NULL)
            dispose_cipher(&ingress_cipher);
        if (egress_cipher.aead != NULL)
            dispose_cipher(&egress_cipher);
    }
    return ret;
}

static int on_ack_stream(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;
    int ret;

    DEBUG_LOG(conn, ack->data.stream.stream_id, "%s; off=%" PRIu64 ",len=%zu", acked ? "acked" : "lost",
              ack->data.stream.args.start, (size_t)(ack->data.stream.args.end - ack->data.stream.args.start));

    /* TODO cache pointer to stream (using a generation counter?) */
    if ((stream = quicly_get_stream(conn, ack->data.stream.stream_id)) == NULL)
        return 0;

    if (acked) {
        if ((ret = quicly_sendbuf_acked(&stream->sendbuf, &ack->data.stream.args)) != 0)
            return ret;
        if (quicly_stream_is_closable(stream) && (ret = stream->on_update(stream)) != 0)
            return ret;
    } else {
        /* FIXME handle rto error */
        if ((ret = quicly_sendbuf_lost(&stream->sendbuf, &ack->data.stream.args)) != 0)
            return ret;
        if (stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_NONE)
            resched_stream_data(stream);
    }

    return 0;
}

static int on_ack_max_stream_data(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;

    /* TODO cache pointer to stream (using a generation counter?) */
    if ((stream = quicly_get_stream(conn, ack->data.stream.stream_id)) != NULL) {
        if (acked) {
            quicly_maxsender_acked(&stream->_send_aux.max_stream_data_sender, &ack->data.max_stream_data.args);
        } else {
            quicly_maxsender_lost(&stream->_send_aux.max_stream_data_sender, &ack->data.max_stream_data.args);
            if (should_update_max_stream_data(stream))
                sched_stream_control(stream);
        }
    }

    return 0;
}

static int on_ack_max_data(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    if (acked) {
        quicly_maxsender_acked(&conn->ingress.max_data.sender, &ack->data.max_data.args);
    } else {
        quicly_maxsender_lost(&conn->ingress.max_data.sender, &ack->data.max_data.args);
    }

    return 0;
}

static int on_ack_max_stream_id_bidi(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    if (acked) {
        quicly_maxsender_acked(&conn->ingress.max_stream_id_bidi, &ack->data.max_stream_id.args);
    } else {
        quicly_maxsender_lost(&conn->ingress.max_stream_id_bidi, &ack->data.max_stream_id.args);
    }

    return 0;
}

static void on_ack_stream_state_sender(quicly_sender_state_t *sender_state, int acked)
{
    *sender_state = acked ? QUICLY_SENDER_STATE_ACKED : QUICLY_SENDER_STATE_SEND;
}

static int on_ack_rst_stream(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;
    int ret = 0;

    if ((stream = quicly_get_stream(conn, ack->data.stream_state_sender.stream_id)) != NULL) {
        assert(stream->sendbuf.acked.num_ranges == 1);
        assert(stream->sendbuf.acked.ranges[0].end - stream->sendbuf.eos <= 1);
        on_ack_stream_state_sender(&stream->_send_aux.rst.sender_state, acked);
        if (stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_ACKED) {
            stream->sendbuf.acked.ranges[0].end = stream->sendbuf.eos + 1;
            if (quicly_stream_is_closable(stream))
                ret = stream->on_update(stream);
        }
    }

    return ret;
}

static int on_ack_stop_sending(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    quicly_stream_t *stream;

    if ((stream = quicly_get_stream(conn, ack->data.stream_state_sender.stream_id)) != NULL) {
        on_ack_stream_state_sender(&stream->_send_aux.stop_sending.sender_state, acked);
        if (stream->_send_aux.stop_sending.sender_state != QUICLY_SENDER_STATE_ACKED)
            sched_stream_control(stream);
    }

    return 0;
}

static int on_ack_stream_id_blocked(quicly_conn_t *conn, int acked, quicly_ack_t *ack)
{
    if (!acked && conn->egress.stream_id_blocked_state == QUICLY_SENDER_STATE_UNACKED && stream_id_blocked(conn, 0)) {
        conn->egress.stream_id_blocked_state = QUICLY_SENDER_STATE_SEND;
    } else {
        conn->egress.stream_id_blocked_state = QUICLY_SENDER_STATE_NONE;
    }

    return 0;
}

int64_t quicly_get_first_timeout(quicly_conn_t *conn)
{
    if (conn->super.state == QUICLY_STATE_DRAINING)
        return INT64_MAX;

    if (1 /* CWND is not full (TODO) */) {
        if (conn->crypto.pending_control || conn->crypto.pending_data)
            return 0;
        if (quicly_linklist_is_linked(&conn->pending_link.control) ||
            quicly_linklist_is_linked(&conn->pending_link.stream_fin_only) ||
            quicly_linklist_is_linked(&conn->pending_link.stream_with_payload))
            return 0;
    }
    return conn->egress.loss.alarm_at < conn->egress.send_ack_at ? conn->egress.loss.alarm_at : conn->egress.send_ack_at;
}

struct st_quicly_send_context_t {
    struct {
        struct st_quicly_cipher_context_t *cipher;
        int first_byte;
    } current;
    struct {
        quicly_datagram_t *packet;
        struct st_quicly_cipher_context_t *cipher;
        uint8_t *first_byte_at;
    } target;
    int64_t now;
    quicly_datagram_t **packets;
    size_t max_packets;
    size_t num_packets;
    uint8_t *dst;
    uint8_t *dst_end;
    uint8_t *dst_encrypt_from;
};

static int commit_send_packet(quicly_conn_t *conn, struct st_quicly_send_context_t *s, int coalesced)
{
    assert(s->target.cipher->aead != NULL);

    if (!coalesced && s->target.packet->data.base[0] == QUICLY_PACKET_TYPE_INITIAL) {
        const size_t max_size = 1264; /* max UDP packet size excluding aead tag */
        assert(s->dst - s->target.packet->data.base <= max_size);
        memset(s->dst, 0, s->target.packet->data.base + max_size - s->dst);
        s->dst = s->target.packet->data.base + max_size;
    }

    if ((*s->target.first_byte_at & 0x80) != 0) {
        uint16_t length = s->dst - s->dst_encrypt_from + s->target.cipher->aead->algo->tag_size + 4;
        length |= 0x4000;
        quicly_encode16(s->dst_encrypt_from - 6, length);
    }
    quicly_encode32(s->dst_encrypt_from - 4, (uint32_t)conn->egress.packet_number | 0xc0000000);

    s->dst = s->dst_encrypt_from + ptls_aead_encrypt(s->target.cipher->aead, s->dst_encrypt_from, s->dst_encrypt_from,
                                                     s->dst - s->dst_encrypt_from, conn->egress.packet_number,
                                                     s->target.first_byte_at, s->dst_encrypt_from - s->target.first_byte_at);

    ptls_cipher_init(s->target.cipher->pne, s->dst_encrypt_from);
    ptls_cipher_encrypt(s->target.cipher->pne, s->dst_encrypt_from - 4, s->dst_encrypt_from - 4, 4);

    s->target.packet->data.len = s->dst - s->target.packet->data.base;
    assert(s->target.packet->data.len <= conn->super.ctx->max_packet_size);

    ++conn->egress.packet_number;

    if (!coalesced) {
        s->packets[s->num_packets++] = s->target.packet;
        s->target.packet = NULL;
        s->target.cipher = NULL;
        s->target.first_byte_at = NULL;
    }

    return 0;
}

static inline uint8_t *emit_cid(uint8_t *dst, const quicly_cid_t *cid)
{
    if (cid->len != 0) {
        memcpy(dst, cid->cid, cid->len);
        dst += cid->len;
    }
    return dst;
}

static int prepare_packet(quicly_conn_t *conn, struct st_quicly_send_context_t *s, size_t min_space)
{
    int coalescible, ret;

    assert(s->current.first_byte != -1);

    /* allocate and setup the new packet if necessary */
    if (s->dst_end - s->dst < min_space || s->target.first_byte_at == NULL) {
        coalescible = 0;
    } else if (*s->target.first_byte_at != s->current.first_byte) {
        coalescible = (*s->target.first_byte_at & 0x80) != 0;
    } else if (s->dst_end - s->dst < min_space) {
        coalescible = 0;
    } else {
        /* use the existing packet */
        return 0;
    }

    /* commit at the same time determining if we will coalesce the packets */
    if (s->target.packet != NULL) {
        if (coalescible) {
            size_t overhead = s->target.cipher->aead->algo->tag_size;
            if ((s->current.first_byte & 0x80) != 0) {
                overhead = 1 + 4 + 1 + conn->super.peer.cid.len + conn->super.host.cid.len + 2 + 4;
            } else {
                overhead = 1 + conn->super.peer.cid.len + 4;
            }
            overhead += s->current.cipher->aead->algo->tag_size;
            if (overhead + min_space > s->dst_end - s->dst)
                coalescible = 0;
        }
        if ((ret = commit_send_packet(conn, s, coalescible)) != 0)
            return ret;
    } else {
        coalescible = 0;
    }

    /* allocate packet */
    if (coalescible) {
        s->target.cipher = s->current.cipher;
    } else {
        if (s->num_packets >= s->max_packets)
            return QUICLY_ERROR_SENDBUF_FULL;
        if ((s->target.packet =
                 conn->super.ctx->alloc_packet(conn->super.ctx, conn->super.peer.salen, conn->super.ctx->max_packet_size)) == NULL)
            return PTLS_ERROR_NO_MEMORY;
        s->target.packet->salen = conn->super.peer.salen;
        memcpy(&s->target.packet->sa, conn->super.peer.sa, conn->super.peer.salen);
        s->target.cipher = s->current.cipher;
        s->dst = s->target.packet->data.base;
        s->dst_end = s->target.packet->data.base + conn->super.ctx->max_packet_size;
    }

    /* emit header */
    s->target.first_byte_at = s->dst;
    *s->dst++ = s->current.first_byte;
    if ((s->current.first_byte & 0x80) != 0) {
        s->dst = quicly_encode32(s->dst, conn->super.version);
        *s->dst++ = (encode_cid_length(conn->super.peer.cid.len) << 4) | encode_cid_length(conn->super.host.cid.len);
        s->dst = emit_cid(s->dst, &conn->super.peer.cid);
        s->dst = emit_cid(s->dst, &conn->super.host.cid);
        /* payload length is filled laterwards */
        *s->dst++ = 0;
        *s->dst++ = 0;
    } else {
        s->dst = emit_cid(s->dst, &conn->super.peer.cid);
    }
    s->dst += 4; /* space for PN bits, filled in at commit time */
    s->dst_encrypt_from = s->dst;
    assert(s->target.cipher->aead != NULL);
    s->dst_end -= s->target.cipher->aead->algo->tag_size;
    assert(s->dst < s->dst_end);

    return 0;
}

static int prepare_acked_packet(quicly_conn_t *conn, struct st_quicly_send_context_t *s, size_t min_space, quicly_ack_t **ack,
                                quicly_ack_cb ack_cb)
{
    int ret;

    if ((ret = prepare_packet(conn, s, min_space)) != 0)
        return ret;
    if ((*ack = quicly_acks_allocate(&conn->egress.acks, conn->egress.packet_number, s->now, ack_cb)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    return ret;
}

static int send_ack(quicly_conn_t *conn, struct st_quicly_send_context_t *s)
{
    size_t range_index;
    int ret;

    if (conn->ingress.ack_queue.num_ranges == 0)
        return 0;

    range_index = conn->ingress.ack_queue.num_ranges - 1;
    do {
        if ((ret = prepare_packet(conn, s, QUICLY_ACK_FRAME_CAPACITY)) != 0)
            break;
        s->dst = quicly_encode_ack_frame(s->dst, s->dst_end, &conn->ingress.ack_queue, &range_index);
    } while (range_index != SIZE_MAX);

    quicly_ranges_clear(&conn->ingress.ack_queue);
    conn->egress.send_ack_at = INT64_MAX;
    return ret;
}

static int prepare_stream_state_sender(quicly_stream_t *stream, quicly_sender_state_t *sender, struct st_quicly_send_context_t *s,
                                       size_t min_space, quicly_ack_cb ack_cb)
{
    quicly_ack_t *ack;
    int ret;

    if ((ret = prepare_acked_packet(stream->conn, s, min_space, &ack, ack_cb)) != 0)
        return ret;
    ack->data.stream_state_sender.stream_id = stream->stream_id;
    *sender = QUICLY_SENDER_STATE_UNACKED;

    return 0;
}

static int send_stream_control_frames(quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    int ret;

    /* send STOP_SENDING if necessray */
    if (stream->_send_aux.stop_sending.sender_state == QUICLY_SENDER_STATE_SEND) {
        if ((ret = prepare_stream_state_sender(stream, &stream->_send_aux.stop_sending.sender_state, s,
                                               QUICLY_STOP_SENDING_FRAME_CAPACITY, on_ack_stop_sending)) != 0)
            return ret;
        s->dst = quicly_encode_stop_sending_frame(s->dst, stream->stream_id, stream->_send_aux.stop_sending.reason);
    }

    /* send MAX_STREAM_DATA if necessary */
    if (should_update_max_stream_data(stream)) {
        uint64_t new_value = stream->recvbuf.data_off + stream->_recv_aux.window;
        quicly_ack_t *ack;
        /* prepare */
        if ((ret = prepare_acked_packet(stream->conn, s, QUICLY_MAX_STREAM_DATA_FRAME_CAPACITY, &ack, on_ack_max_stream_data)) != 0)
            return ret;
        /* send */
        s->dst = quicly_encode_max_stream_data_frame(s->dst, stream->stream_id, new_value);
        /* register ack */
        ack->data.max_stream_data.stream_id = stream->stream_id;
        quicly_maxsender_record(&stream->_send_aux.max_stream_data_sender, new_value, &ack->data.max_stream_data.args);
    }

    /* send RST_STREAM if necessary */
    if (stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_SEND) {
        if ((ret = prepare_stream_state_sender(stream, &stream->_send_aux.rst.sender_state, s, QUICLY_RST_FRAME_CAPACITY,
                                               on_ack_rst_stream)) != 0)
            return ret;
        s->dst =
            quicly_encode_rst_stream_frame(s->dst, stream->stream_id, stream->_send_aux.rst.reason, stream->_send_aux.max_sent);
    }

    return 0;
}

static int send_stream_frame(quicly_stream_t *stream, struct st_quicly_send_context_t *s, quicly_sendbuf_dataiter_t *iter,
                             size_t max_bytes)
{
    quicly_ack_t *ack;
    size_t copysize;
    int ret;

    if ((ret = prepare_acked_packet(stream->conn, s, QUICLY_STREAM_FRAME_CAPACITY, &ack, on_ack_stream)) != 0)
        return ret;

    copysize = max_bytes - (iter->stream_off + max_bytes > stream->sendbuf.eos);
    s->dst = quicly_encode_stream_frame_header(s->dst, s->dst_end, stream->stream_id,
                                               iter->stream_off + copysize >= stream->sendbuf.eos, iter->stream_off, &copysize);

    DEBUG_LOG(stream->conn, stream->stream_id, "sending; off=%" PRIu64 ",len=%zu", iter->stream_off, copysize);

    /* adjust remaining send window */
    if (stream->_send_aux.max_sent < iter->stream_off + copysize) {
        if (stream->stream_id != 0) {
            uint64_t delta = iter->stream_off + copysize - stream->_send_aux.max_sent;
            assert(stream->conn->egress.max_data.sent + delta <= stream->conn->egress.max_data.permitted);
            stream->conn->egress.max_data.sent += delta;
        }
        stream->_send_aux.max_sent = iter->stream_off + copysize;
        if (stream->_send_aux.max_sent == stream->sendbuf.eos)
            ++stream->_send_aux.max_sent;
    }

    /* send */
    quicly_sendbuf_emit(&stream->sendbuf, iter, copysize, s->dst, &ack->data.stream.args);
    s->dst += copysize;

    ack->data.stream.stream_id = stream->stream_id;

    return 0;
}

static int send_stream_data(quicly_stream_t *stream, struct st_quicly_send_context_t *s)
{
    quicly_sendbuf_dataiter_t iter;
    uint64_t max_stream_data;
    size_t i;
    int ret = 0;

    /* determine the maximum offset than can be sent */
    if (stream->_send_aux.max_sent >= stream->sendbuf.eos) {
        max_stream_data = stream->sendbuf.eos + 1;
    } else {
        uint64_t delta = stream->_send_aux.max_stream_data - stream->_send_aux.max_sent;
        if (stream->stream_id != 0 && stream->conn->egress.max_data.permitted - stream->conn->egress.max_data.sent < delta)
            delta = (uint64_t)(stream->conn->egress.max_data.permitted - stream->conn->egress.max_data.sent);
        max_stream_data = stream->_send_aux.max_sent + delta;
        if (max_stream_data == stream->sendbuf.eos)
            ++max_stream_data;
    }

    /* emit packets in the pending ranges */
    quicly_sendbuf_init_dataiter(&stream->sendbuf, &iter);
    for (i = 0; i != stream->sendbuf.pending.num_ranges; ++i) {
        uint64_t start = stream->sendbuf.pending.ranges[i].start, end = stream->sendbuf.pending.ranges[i].end;
        if (max_stream_data <= start)
            goto ShrinkRanges;
        if (max_stream_data < end)
            end = max_stream_data;

        if (iter.stream_off != start) {
            assert(iter.stream_off <= start);
            quicly_sendbuf_advance_dataiter(&iter, start - iter.stream_off);
        }
        /* when end == eos, iter.stream_off becomes end+1 after calling send_steram_frame; hence `<` is used */
        while (iter.stream_off < end) {
            if ((ret = send_stream_frame(stream, s, &iter, end - iter.stream_off)) != 0) {
                if (ret == QUICLY_ERROR_SENDBUF_FULL)
                    goto ShrinkToIter;
                return ret;
            }
        }

        if (iter.stream_off < stream->sendbuf.pending.ranges[i].end)
            goto ShrinkToIter;
    }

    quicly_ranges_clear(&stream->sendbuf.pending);
    return 0;

ShrinkToIter:
    stream->sendbuf.pending.ranges[i].start = iter.stream_off;
ShrinkRanges:
    quicly_ranges_shrink(&stream->sendbuf.pending, 0, i);
    return ret;
}

static int retire_acks(quicly_conn_t *conn, size_t count)
{
    quicly_acks_iter_t iter;
    quicly_ack_t *ack;
    uint64_t pn;
    int ret;

    assert(count != 0);

    quicly_acks_init_iter(&conn->egress.acks, &iter);
    ack = quicly_acks_get(&iter);

    do {
        if ((pn = ack->packet_number) == UINT64_MAX)
            break;
        do {
            if ((ret = ack->acked(conn, 0, ack)) != 0)
                return ret;
            quicly_acks_release(&conn->egress.acks, &iter);
            quicly_acks_next(&iter);
        } while ((ack = quicly_acks_get(&iter))->packet_number == pn);
    } while (--count != 0);

    return 0;
}

static int do_detect_loss(quicly_loss_t *ld, int64_t now, uint64_t largest_acked, uint32_t delay_until_lost, int64_t *loss_time)
{
    quicly_conn_t *conn = (void *)((char *)ld - offsetof(quicly_conn_t, egress.loss));
    quicly_acks_iter_t iter;
    quicly_ack_t *ack;
    int64_t sent_before = now - delay_until_lost;
    uint64_t logged_pn = UINT64_MAX;
    int ret;

    quicly_acks_init_iter(&conn->egress.acks, &iter);

    /* handle loss */
    while ((ack = quicly_acks_get(&iter))->sent_at <= sent_before) {
        if (ack->packet_number != logged_pn) {
            logged_pn = ack->packet_number;
            DEBUG_LOG(conn, 0, "RTO; packet-number: %" PRIu64, logged_pn);
        }
        if ((ret = ack->acked(conn, 0, ack)) != 0)
            return ret;
        quicly_acks_release(&conn->egress.acks, &iter);
        quicly_acks_next(&iter);
    }

    /* schedule next alarm */
    *loss_time = ack->sent_at == INT64_MAX ? INT64_MAX : ack->sent_at + delay_until_lost;

    return 0;
}

static int send_stream_frames(quicly_conn_t *conn, struct st_quicly_send_context_t *s)
{
    int ret = 0;

    /* fin-only STREAM frames */
    while (s->num_packets != s->max_packets && quicly_linklist_is_linked(&conn->pending_link.stream_fin_only)) {
        quicly_stream_t *stream =
            (void *)((char *)conn->pending_link.stream_fin_only.next - offsetof(quicly_stream_t, _send_aux.pending_link.stream));
        if ((ret = send_stream_data(stream, s)) != 0)
            goto Exit;
        resched_stream_data(stream);
    }
    /* STREAM frames with payload */
    while (s->num_packets != s->max_packets && quicly_linklist_is_linked(&conn->pending_link.stream_with_payload) &&
           conn->egress.max_data.sent < conn->egress.max_data.permitted) {
        quicly_stream_t *stream = (void *)((char *)conn->pending_link.stream_with_payload.next -
                                           offsetof(quicly_stream_t, _send_aux.pending_link.stream));
        if ((ret = send_stream_data(stream, s)) != 0)
            goto Exit;
        resched_stream_data(stream);
    }

Exit:
    return 0;
}

quicly_datagram_t *quicly_send_version_negotiation(quicly_context_t *ctx, struct sockaddr *sa, socklen_t salen,
                                                   ptls_iovec_t dest_cid, ptls_iovec_t src_cid)
{
    quicly_datagram_t *packet;
    uint8_t *dst;

    if ((packet = ctx->alloc_packet(ctx, salen, ctx->max_packet_size)) == NULL)
        return NULL;
    packet->salen = salen;
    memcpy(&packet->sa, sa, salen);
    dst = packet->data.base;

    /* type_flags */
    ctx->tls->random_bytes(dst, 1);
    *dst |= 0x80;
    ++dst;
    /* version */
    dst = quicly_encode32(dst, 0);
    /* connection-id */
    *dst++ = (encode_cid_length(dest_cid.len) << 4) | encode_cid_length(src_cid.len);
    if (dest_cid.len != 0) {
        memcpy(dst, dest_cid.base, dest_cid.len);
        dst += dest_cid.len;
    }
    if (src_cid.len != 0) {
        memcpy(dst, src_cid.base, src_cid.len);
        dst += src_cid.len;
    }
    /* supported_versions */
    dst = quicly_encode32(dst, QUICLY_PROTOCOL_VERSION);

    packet->data.len = dst - packet->data.base;

    return packet;
}

int quicly_send(quicly_conn_t *conn, quicly_datagram_t **packets, size_t *num_packets)
{
    struct st_quicly_send_context_t s = {
        {&conn->egress.pp.handshake, -1}, {NULL, NULL, NULL}, conn->super.ctx->now(conn->super.ctx), packets, *num_packets};
    int ret;

    if (quicly_get_state(conn) == QUICLY_STATE_DRAINING) {
        *num_packets = 0;
        return QUICLY_ERROR_CONNECTION_CLOSED;
    }

    /* handle timeouts */
    if (conn->egress.loss.alarm_at <= s.now) {
        size_t min_packets_to_send;
        if ((ret = quicly_loss_on_alarm(&conn->egress.loss, s.now, conn->egress.packet_number - 1, do_detect_loss,
                                        &min_packets_to_send)) != 0)
            goto Exit;
        if (min_packets_to_send != 0) {
            /* better way to notify the app that we want to send some packets outside the congestion window? */
            assert(min_packets_to_send <= s.max_packets);
            s.max_packets = min_packets_to_send;
            if ((ret = retire_acks(conn, min_packets_to_send)) != 0)
                goto Exit;
        }
    }

    /* send cleartext frames */
    switch (quicly_get_state(conn)) {
    case QUICLY_STATE_SEND_RETRY:
        assert(!quicly_is_client(conn));
        s.current.first_byte = QUICLY_PACKET_TYPE_RETRY;
        break;
    case QUICLY_STATE_FIRSTFLIGHT:
        assert(quicly_is_client(conn));
        s.current.first_byte = QUICLY_PACKET_TYPE_INITIAL;
        break;
    default:
        s.current.first_byte = QUICLY_PACKET_TYPE_HANDSHAKE;
        if (conn->egress.send_ack_at <= s.now && quicly_get_state(conn) != QUICLY_STATE_1RTT_ENCRYPTED) {
            if ((ret = send_ack(conn, &s)) != 0)
                goto Exit;
        }
        break;
    }

    /* respond to all pending received PATH_CHALLENGE frames */
    if (conn->egress.path_challenge.head != NULL) {
        do {
            struct st_quicly_pending_path_challenge_t *c = conn->egress.path_challenge.head;
            if ((ret = prepare_packet(conn, &s, QUICLY_PATH_CHALLENGE_FRAME_CAPACITY)) != 0)
                goto Exit;
            s.dst = quicly_encode_path_challenge_frame(s.dst, c->is_response, c->data);
            conn->egress.path_challenge.head = c->next;
            free(c);
        } while (conn->egress.path_challenge.head != NULL);
        conn->egress.path_challenge.tail_ref = &conn->egress.path_challenge.head;
    }

    /* process crypto stream */
    if (conn->crypto.pending_control || conn->crypto.pending_data) {
        if (conn->crypto.pending_control) {
            if ((ret = send_stream_control_frames(&conn->crypto.stream, &s)) != 0)
                goto Exit;
            conn->crypto.pending_control = 0;
        }
        if (conn->crypto.pending_data) {
            if ((ret = send_stream_data(&conn->crypto.stream, &s)) != 0)
                goto Exit;
            conn->crypto.pending_data = 0;
        }
    }

    /* send 0RTT packets if 0RTT key is available */
    if (conn->egress.pp.early_data.aead != NULL) {
        assert(conn->egress.pp.one_rtt[0].aead == NULL);
        s.current.cipher = &conn->egress.pp.early_data;
        s.current.first_byte = QUICLY_PACKET_TYPE_0RTT_PROTECTED;
        if ((ret = send_stream_frames(conn, &s)) != 0)
            goto Exit;
    }

    /* send 1RTT-encrypted packets */
    if (quicly_get_state(conn) == QUICLY_STATE_1RTT_ENCRYPTED) {
        assert(conn->egress.pp.early_data.aead == NULL);
        s.current.cipher = &conn->egress.pp.one_rtt[0];
        s.current.first_byte = 0x30; /* short header, key-phase=0 */
	/* send ack frame */
        if (conn->egress.send_ack_at <= s.now) {
            if ((ret = send_ack(conn, &s)) != 0)
                goto Exit;
        }
        /* send max_stream_id frame (TODO uni) */
        uint64_t max_stream_id;
        if ((max_stream_id = quicly_maxsender_should_update_stream_id(
                 &conn->ingress.max_stream_id_bidi, conn->super.peer.next_stream_id_bidi, conn->super.peer.num_streams,
                 conn->super.ctx->max_streams_bidi, 768)) != 0) {
            quicly_ack_t *ack;
            if ((ret = prepare_acked_packet(conn, &s, QUICLY_MAX_STREAM_ID_FRAME_CAPACITY, &ack, on_ack_max_stream_id_bidi)) != 0)
                goto Exit;
            s.dst = quicly_encode_max_stream_id_frame(s.dst, max_stream_id);
            quicly_maxsender_record(&conn->ingress.max_stream_id_bidi, max_stream_id, &ack->data.max_stream_id.args);
        }
        /* send connection-level flow control frame */
        if (quicly_maxsender_should_update(&conn->ingress.max_data.sender, conn->ingress.max_data.bytes_consumed,
                                           conn->super.ctx->initial_max_data, 512)) {
            quicly_ack_t *ack;
            if ((ret = prepare_acked_packet(conn, &s, QUICLY_MAX_DATA_FRAME_CAPACITY, &ack, on_ack_max_data)) != 0)
                goto Exit;
            uint64_t new_value = conn->ingress.max_data.bytes_consumed + conn->super.ctx->initial_max_data;
            s.dst = quicly_encode_max_data_frame(s.dst, new_value);
            quicly_maxsender_record(&conn->ingress.max_data.sender, new_value, &ack->data.max_data.args);
        }
        /* send stream_id_blocked frame (TODO uni) */
        if (conn->egress.stream_id_blocked_state == QUICLY_SENDER_STATE_SEND) {
            if (stream_id_blocked(conn, 0)) {
                quicly_ack_t *ack;
                if ((ret = prepare_acked_packet(conn, &s, QUICLY_STREAM_ID_BLOCKED_FRAME_CAPACITY, &ack,
                                                on_ack_stream_id_blocked)) != 0)
                    goto Exit;
                s.dst = quicly_encode_stream_id_blocked_frame(s.dst, conn->egress.max_stream_id_bidi);
                conn->egress.stream_id_blocked_state = QUICLY_SENDER_STATE_UNACKED;
            } else {
                conn->egress.stream_id_blocked_state = QUICLY_SENDER_STATE_NONE;
            }
        }
        /* send stream-level control frames */
        while (s.num_packets != s.max_packets && quicly_linklist_is_linked(&conn->pending_link.control)) {
            quicly_stream_t *stream =
                (void *)((char *)conn->pending_link.control.next - offsetof(quicly_stream_t, _send_aux.pending_link.control));
            if ((ret = send_stream_control_frames(stream, &s)) != 0)
                goto Exit;
            quicly_linklist_unlink(&stream->_send_aux.pending_link.control);
        }
        /* send STREAM frames */
        if ((ret = send_stream_frames(conn, &s)) != 0)
            goto Exit;
        /* piggyback an ack if a packet is under construction.
	 * TODO: This may cause a second packet to be emitted. Check for packet size before piggybacking ack.
	 */
        if (s.target.packet != NULL) {
            if ((ret = send_ack(conn, &s)) != 0)
                goto Exit;
        }
    }

    if (s.target.packet != NULL)
        commit_send_packet(conn, &s, 0);

    quicly_loss_update_alarm(&conn->egress.loss, s.now, conn->egress.acks.head != NULL);

    ret = 0;
Exit:
    if (ret == QUICLY_ERROR_SENDBUF_FULL)
        ret = 0;
    if (ret == 0) {
        *num_packets = s.num_packets;
        if (s.current.first_byte == QUICLY_PACKET_TYPE_RETRY)
            ret = QUICLY_ERROR_CONNECTION_CLOSED;
    }
    return ret;
}

static int get_stream_or_open_if_new(quicly_conn_t *conn, uint64_t stream_id, quicly_stream_t **stream)
{
    int ret = 0;

    if ((*stream = quicly_get_stream(conn, stream_id)) != NULL)
        goto Exit;

    /* TODO implement */
    if (STREAM_IS_UNI(stream_id)) {
        ret = QUICLY_ERROR_INTERNAL;
        goto Exit;
    }

    if (STREAM_IS_CLIENT_INITIATED(stream_id) != quicly_is_client(conn) && conn->super.peer.next_stream_id_bidi <= stream_id) {
        /* open new streams upto given id */
        do {
            if ((*stream = open_stream(conn, conn->super.peer.next_stream_id_bidi)) == NULL) {
                ret = PTLS_ERROR_NO_MEMORY;
                goto Exit;
            }
            if ((ret = conn->super.ctx->on_stream_open(*stream)) != 0) {
                destroy_stream(*stream);
                *stream = NULL;
                goto Exit;
            }
            ++conn->super.peer.num_streams;
            conn->super.peer.next_stream_id_bidi += 4;
        } while (stream_id != (*stream)->stream_id);
    }

Exit:
    return ret;
}

static int handle_stream_frame(quicly_conn_t *conn, quicly_stream_frame_t *frame)
{
    quicly_stream_t *stream;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;
    return apply_stream_frame(stream, frame);
}

static int handle_rst_stream_frame(quicly_conn_t *conn, quicly_rst_stream_frame_t *frame)
{
    quicly_stream_t *stream;
    uint64_t bytes_missing;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;

    if ((ret = quicly_recvbuf_reset(&stream->recvbuf, frame->final_offset, &bytes_missing)) != 0)
        return ret;
    stream->_recv_aux.rst_reason = frame->app_error_code;
    conn->ingress.max_data.bytes_consumed += bytes_missing;

    if (quicly_stream_is_closable(stream))
        ret = stream->on_update(stream);

    return ret;
}

static int handle_ack_frame(quicly_conn_t *conn, quicly_ack_frame_t *frame, int64_t now)
{
    quicly_acks_iter_t iter;
    uint64_t packet_number = frame->smallest_acknowledged;
    int64_t last_packet_sent_at = INT64_MAX;
    int ret;

    quicly_acks_init_iter(&conn->egress.acks, &iter);

    size_t gap_index = frame->num_gaps;
    while (1) {
        uint64_t block_length = frame->ack_block_lengths[gap_index];
        if (block_length != 0) {
            while (quicly_acks_get(&iter)->packet_number < packet_number)
                quicly_acks_next(&iter);
            do {
                quicly_ack_t *ack;
                while ((ack = quicly_acks_get(&iter))->packet_number == packet_number) {
                    last_packet_sent_at = ack->sent_at;
                    if ((ret = ack->acked(conn, 1, ack)) != 0)
                        return ret;
                    quicly_acks_release(&conn->egress.acks, &iter);
                    quicly_acks_next(&iter);
                }
                if (quicly_loss_on_packet_acked(&conn->egress.loss, packet_number)) {
                    /* FIXME notify CC that RTO has been verified */
                }
                ++packet_number;
            } while (--block_length != 0);
        }
        if (gap_index-- == 0)
            break;
        packet_number += frame->gaps[gap_index];
    }

    quicly_loss_on_ack_received(&conn->egress.loss, frame->largest_acknowledged,
                                last_packet_sent_at <= now && packet_number >= frame->largest_acknowledged
                                    ? (uint32_t)(now - last_packet_sent_at)
                                    : UINT32_MAX);
    quicly_loss_detect_loss(&conn->egress.loss, now, conn->egress.packet_number - 1, frame->largest_acknowledged, do_detect_loss);
    quicly_loss_update_alarm(&conn->egress.loss, now, conn->egress.acks.head != NULL);

    return 0;
}

static int handle_max_stream_data_frame(quicly_conn_t *conn, quicly_max_stream_data_frame_t *frame)
{
    quicly_stream_t *stream = quicly_get_stream(conn, frame->stream_id);

    if (stream == NULL)
        return 0;

    if (frame->max_stream_data < stream->_send_aux.max_stream_data)
        return 0;
    stream->_send_aux.max_stream_data = frame->max_stream_data;

    if (stream->_send_aux.rst.sender_state == QUICLY_SENDER_STATE_NONE)
        resched_stream_data(stream);

    return 0;
}

static int handle_stream_blocked_frame(quicly_conn_t *conn, quicly_stream_blocked_frame_t *frame)
{
    quicly_stream_t *stream;

    if ((stream = quicly_get_stream(conn, frame->stream_id)) != NULL)
        quicly_maxsender_reset(&stream->_send_aux.max_stream_data_sender, 0);

    return 0;
}

static int handle_max_stream_id_frame(quicly_conn_t *conn, quicly_max_stream_id_frame_t *frame)
{
    uint64_t *slot = STREAM_IS_UNI(frame->max_stream_id) ? &conn->egress.max_stream_id_uni : &conn->egress.max_stream_id_bidi;
    if (frame->max_stream_id < *slot)
        return 0;
    *slot = frame->max_stream_id;
    /* TODO notify the app? */
    return 0;
}

static int handle_path_challenge_frame(quicly_conn_t *conn, quicly_path_challenge_frame_t *frame)
{
    return schedule_path_challenge(conn, 1, frame->data);
}

static int handle_stop_sending_frame(quicly_conn_t *conn, quicly_stop_sending_frame_t *frame)
{
    quicly_stream_t *stream;
    int ret;

    if ((ret = get_stream_or_open_if_new(conn, frame->stream_id, &stream)) != 0 || stream == NULL)
        return ret;

    quicly_reset_stream(stream, QUICLY_RESET_STREAM_EGRESS, QUICLY_ERROR_TBD);
    return 0;
}

static int handle_max_data_frame(quicly_conn_t *conn, quicly_max_data_frame_t *frame)
{
    if (frame->max_data < conn->egress.max_data.permitted)
        return 0;
    conn->egress.max_data.permitted = frame->max_data;

    /* TODO schedule for delivery */
    return 0;
}

static int negotiate_using_version(quicly_conn_t *conn, uint32_t version)
{
    /* set selected version */
    conn->super.version = version;
    DEBUG_LOG(conn, 0, "switching version to %" PRIx32, version);

    { /* reschedule the Initial packet for immediate resend */
        quicly_acks_iter_t iter;
        quicly_acks_init_iter(&conn->egress.acks, &iter);
        quicly_ack_t *ack = quicly_acks_get(&iter);
        int ret = ack->acked(conn, 0, ack);
        assert(ret == 0);
        quicly_acks_release(&conn->egress.acks, &iter);
    }

    return 0;
}

static int handle_version_negotiation_packet(quicly_conn_t *conn, quicly_decoded_packet_t *packet)
{
#define CAN_SELECT(v) ((v) != conn->super.version && (v) == QUICLY_PROTOCOL_VERSION)

    const uint8_t *src = packet->octets.base + packet->encrypted_off, *end = packet->octets.base + packet->octets.len;

    if (src == end || (end - src) % 4 != 0)
        return QUICLY_ERROR_PROTOCOL_VIOLATION;
    while (src != end) {
        uint32_t supported_version = quicly_decode32(&src);
        if (CAN_SELECT(supported_version))
            return negotiate_using_version(conn, supported_version);
    }
    return QUICLY_ERROR_VERSION_NEGOTIATION_FAILURE;

#undef CAN_SELECT
}

int quicly_is_destination(quicly_conn_t *conn, int is_1rtt, ptls_iovec_t cid)
{
    if (quicly_cid_is_equal(&conn->super.host.cid, cid)) {
        return 1;
    } else if (!is_1rtt && !quicly_is_client(conn) && quicly_cid_is_equal(&conn->super.host.offered_cid, cid)) {
        /* long header pacekt carrying the offered CID */
        return 1;
    }
    return 0;
}

int quicly_receive(quicly_conn_t *conn, quicly_decoded_packet_t *packet)
{
    int64_t now = conn->super.ctx->now(conn->super.ctx);
    struct st_quicly_cipher_context_t *cipher;
    ptls_iovec_t payload;
    int ret;

    if (conn->super.state == QUICLY_STATE_FIRSTFLIGHT) {
        assert(quicly_is_client(conn));
        if (QUICLY_PACKET_TYPE_IS_1RTT(packet->octets.base[0])) {
            ret = QUICLY_ERROR_PACKET_IGNORED;
            goto Exit;
        }
        /* FIXME check peer address */
        memcpy(conn->super.peer.cid.cid, packet->cid.src.base, packet->cid.src.len);
        conn->super.peer.cid.len = packet->cid.src.len;
    }
    if (conn->super.state == QUICLY_STATE_DRAINING) {
        ret = 0;
        goto Exit;
    }

    if (conn->super.state != QUICLY_STATE_1RTT_ENCRYPTED && QUICLY_PACKET_TYPE_IS_1RTT(packet->octets.base[0])) {
        /* FIXME enqueue the packet? */
        ret = QUICLY_ERROR_PACKET_IGNORED;
        goto Exit;
    }

    if (QUICLY_PACKET_TYPE_IS_1RTT(packet->octets.base[0])) {
        int key_phase = QUICLY_PACKET_TYPE_1RTT_TO_KEY_PHASE(packet->octets.base[0]);
        if ((cipher = &conn->ingress.pp.one_rtt[key_phase])->aead == NULL) {
            /* drop 1rtt-encrypted packets received prior to handshake completion (due to loss of the packet carrying the latter) */
            ret = key_phase == 0 && quicly_get_state(conn) != QUICLY_STATE_1RTT_ENCRYPTED ? 0 : QUICLY_ERROR_TBD;
            goto Exit;
        }
    } else {
        if (conn->super.state == QUICLY_STATE_FIRSTFLIGHT) {
            if (packet->version == 0)
                return handle_version_negotiation_packet(conn, packet);
        }
        switch (packet->octets.base[0]) {
        case QUICLY_PACKET_TYPE_RETRY:
            if (!(quicly_is_client(conn) && conn->super.state == QUICLY_STATE_FIRSTFLIGHT) ||
                (cipher = &conn->ingress.pp.handshake)->aead == NULL) {
                ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
                goto Exit;
            }
            /* FIXME verify that the PN is the same as that of the Initial that we have sent */
            conn->crypto.stream.on_update = crypto_stream_receive_stateless_retry;
            break;
        case QUICLY_PACKET_TYPE_HANDSHAKE:
            if ((cipher = &conn->ingress.pp.handshake)->aead == NULL) {
                ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
                goto Exit;
            }
            if (conn->super.state == QUICLY_STATE_FIRSTFLIGHT)
                conn->super.state = QUICLY_STATE_HANDSHAKE;
            break;
        case QUICLY_PACKET_TYPE_0RTT_PROTECTED:
            if (quicly_is_client(conn)) {
                ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
                goto Exit;
            }
            if ((cipher = &conn->ingress.pp.early_data)->aead == NULL) {
                /* could happen upon resumption failure, VN, etc. */
                ret = QUICLY_ERROR_PACKET_IGNORED;
                goto Exit;
            }
            break;
        case QUICLY_PACKET_TYPE_INITIAL:
            /* FIXME ignore for time being */
            ret = 0;
            goto Exit;
        default:
            ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
            goto Exit;
        }
    }

    if ((payload = decrypt_packet(cipher, packet, &conn->ingress.next_expected_packet_number)).base == NULL) {
        ret = QUICLY_ERROR_TBD;
        goto Exit;
    }

    if (payload.len == 0) {
        ret = QUICLY_ERROR_PROTOCOL_VIOLATION;
        goto Exit;
    }

    const uint8_t *src = payload.base, *end = src + payload.len;
    int is_ack_only = 1;
    do {
        uint8_t type_flags = *src++;
        if ((type_flags & ~QUICLY_FRAME_TYPE_STREAM_BITS) == QUICLY_FRAME_TYPE_STREAM_BASE) {
            quicly_stream_frame_t frame;
            if ((ret = quicly_decode_stream_frame(type_flags, &src, end, &frame)) != 0)
                goto Exit;
            if ((ret = handle_stream_frame(conn, &frame)) != 0)
                goto Exit;
            is_ack_only = 0;
        } else if (type_flags == QUICLY_FRAME_TYPE_ACK) {
            quicly_ack_frame_t frame;
            if ((ret = quicly_decode_ack_frame(type_flags, &src, end, &frame)) != 0)
                goto Exit;
            if (packet->octets.base[0] == QUICLY_PACKET_TYPE_RETRY) {
                /* skip, TODO use separate decoding logic (like the one in quicly_accept) for stateless-retry */
            } else {
                if ((ret = handle_ack_frame(conn, &frame, now)) != 0)
                    goto Exit;
            }
        } else {
            switch (type_flags) {
            case QUICLY_FRAME_TYPE_PADDING:
                ret = 0;
                break;
            case QUICLY_FRAME_TYPE_RST_STREAM: {
                quicly_rst_stream_frame_t frame;
                if ((ret = quicly_decode_rst_stream_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_rst_stream_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_CONNECTION_CLOSE:
            case QUICLY_FRAME_TYPE_APPLICATION_CLOSE: {
                quicly_close_frame_t frame;
                if ((ret = quicly_decode_close_frame(&src, end, &frame)) != 0)
                    goto Exit;
                conn->super.state = QUICLY_STATE_DRAINING;
                if (conn->super.ctx->on_conn_close != NULL)
                    conn->super.ctx->on_conn_close(conn, type_flags, frame.error_code, (const char *)frame.reason_phrase.base,
                                                   frame.reason_phrase.len);
            } break;
            case QUICLY_FRAME_TYPE_MAX_DATA: {
                quicly_max_data_frame_t frame;
                if ((ret = quicly_decode_max_data_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_max_data_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_MAX_STREAM_DATA: {
                quicly_max_stream_data_frame_t frame;
                if ((ret = quicly_decode_max_stream_data_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_max_stream_data_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_MAX_STREAM_ID: {
                quicly_max_stream_id_frame_t frame;
                if ((ret = quicly_decode_max_stream_id_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_max_stream_id_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_PATH_CHALLENGE: {
                quicly_path_challenge_frame_t frame;
                if ((ret = quicly_decode_path_challenge_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_path_challenge_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_PING:
                ret = 0;
                break;
            case QUICLY_FRAME_TYPE_BLOCKED: {
                quicly_blocked_frame_t frame;
                if ((ret = quicly_decode_blocked_frame(&src, end, &frame)) != 0)
                    goto Exit;
                quicly_maxsender_reset(&conn->ingress.max_data.sender, 0);
                ret = 0;
            } break;
            case QUICLY_FRAME_TYPE_STREAM_BLOCKED: {
                quicly_stream_blocked_frame_t frame;
                if ((ret = quicly_decode_stream_blocked_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_stream_blocked_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            case QUICLY_FRAME_TYPE_STREAM_ID_BLOCKED: {
                quicly_stream_id_blocked_frame_t frame;
                if ((ret = quicly_decode_stream_id_blocked_frame(&src, end, &frame)) != 0)
                    goto Exit;
                quicly_maxsender_reset(
                    STREAM_IS_UNI(frame.stream_id) ? &conn->ingress.max_stream_id_uni : &conn->ingress.max_stream_id_bidi, 0);
                ret = 0;
            } break;
            case QUICLY_FRAME_TYPE_STOP_SENDING: {
                quicly_stop_sending_frame_t frame;
                if ((ret = quicly_decode_stop_sending_frame(&src, end, &frame)) != 0)
                    goto Exit;
                if ((ret = handle_stop_sending_frame(conn, &frame)) != 0)
                    goto Exit;
            } break;
            default:
                fprintf(stderr, "ignoring frame type:%02x\n", (unsigned)type_flags);
                ret = QUICLY_ERROR_TBD;
                goto Exit;
            }
            is_ack_only = 0;
        }
    } while (src != end);

    if (packet->octets.base[0] != QUICLY_PACKET_TYPE_RETRY) {
        if ((ret = quicly_ranges_update(&conn->ingress.ack_queue, conn->ingress.next_expected_packet_number - 1,
                                        conn->ingress.next_expected_packet_number)) != 0)
            goto Exit;
        if (!is_ack_only && conn->egress.send_ack_at == INT64_MAX)
            conn->egress.send_ack_at = conn->super.ctx->now(conn->super.ctx) + QUICLY_DELAYED_ACK_TIMEOUT;
    }

Exit:
    return ret;
}

int quicly_open_stream(quicly_conn_t *conn, quicly_stream_t **stream)
{
    if (stream_id_blocked(conn, 0)) {
        conn->egress.stream_id_blocked_state = QUICLY_SENDER_STATE_SEND;
        return QUICLY_ERROR_TOO_MANY_OPEN_STREAMS;
    }

    if ((*stream = open_stream(conn, conn->super.host.next_stream_id_bidi)) == NULL)
        return PTLS_ERROR_NO_MEMORY;

    ++conn->super.host.num_streams;
    conn->super.host.next_stream_id_bidi += 4;

    return 0;
}

void quicly_reset_stream(quicly_stream_t *stream, unsigned direction, uint32_t reason)
{
    if ((direction & QUICLY_RESET_STREAM_EGRESS) != 0) {
        /* if we have not yet sent FIN, then... */
        if (stream->_send_aux.max_sent <= stream->sendbuf.eos) {
            /* close the sender and mark the eos as the only byte that's not confirmed */
            assert(!quicly_sendbuf_transfer_complete(&stream->sendbuf));
            quicly_sendbuf_shutdown(&stream->sendbuf);
            quicly_sendbuf_ackargs_t ackargs = {0, stream->sendbuf.eos};
            quicly_sendbuf_acked(&stream->sendbuf, &ackargs);
            /* setup RST_STREAM */
            stream->_send_aux.rst.sender_state = QUICLY_SENDER_STATE_SEND;
            stream->_send_aux.rst.reason = reason;
            /* schedule for delivery */
            sched_stream_control(stream);
        }
    }

    if ((direction & QUICLY_RESET_STREAM_INGRESS) != 0) {
        /* send STOP_SENDING if the incoming side of the stream is still open */
        if (stream->recvbuf.eos == UINT64_MAX && stream->_send_aux.stop_sending.sender_state == QUICLY_SENDER_STATE_NONE) {
            stream->_send_aux.stop_sending.sender_state = QUICLY_SENDER_STATE_SEND;
            sched_stream_control(stream);
        }
    }
}

void quicly_close_stream(quicly_stream_t *stream)
{
    assert(quicly_stream_is_closable(stream));
    destroy_stream(stream);
}

quicly_datagram_t *quicly_default_alloc_packet(quicly_context_t *ctx, socklen_t salen, size_t payloadsize)
{
    quicly_datagram_t *packet;

    if ((packet = malloc(offsetof(quicly_datagram_t, sa) + salen + payloadsize)) == NULL)
        return NULL;
    packet->salen = salen;
    packet->data.base = (uint8_t *)packet + offsetof(quicly_datagram_t, sa) + salen;

    return packet;
}

void quicly_default_free_packet(quicly_context_t *ctx, quicly_datagram_t *packet)
{
    free(packet);
}

quicly_stream_t *quicly_default_alloc_stream(quicly_context_t *ctx)
{
    return malloc(sizeof(quicly_stream_t));
}

void quicly_default_free_stream(quicly_stream_t *stream)
{
    free(stream);
}

int64_t quicly_default_now(quicly_context_t *ctx)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void quicly_default_debug_log(quicly_context_t *ctx, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static void tohex(char *dst, uint8_t v)
{
    dst[0] = "0123456789abcdef"[v >> 4];
    dst[1] = "0123456789abcdef"[v & 0xf];
}

char *quicly_hexdump(const uint8_t *bytes, size_t len, size_t indent)
{
    size_t i, line, row, bufsize = indent == SIZE_MAX ? len * 2 + 1 : (indent + 5 + 3 * 16 + 2 + 16 + 1) * ((len + 15) / 16) + 1;
    char *buf, *p;

    if ((buf = malloc(bufsize)) == NULL)
        return NULL;
    p = buf;
    if (indent == SIZE_MAX) {
        for (i = 0; i != len; ++i) {
            tohex(p, bytes[i]);
            p += 2;
        }
    } else {
        for (line = 0; line * 16 < len; ++line) {
            for (i = 0; i < indent; ++i)
                *p++ = ' ';
            tohex(p, (line >> 4) & 0xff);
            p += 2;
            tohex(p, (line << 4) & 0xff);
            p += 2;
            *p++ = ' ';
            for (row = 0; row < 16; ++row) {
                *p++ = row == 8 ? '-' : ' ';
                if (line * 16 + row < len) {
                    tohex(p, bytes[line * 16 + row]);
                    p += 2;
                } else {
                    *p++ = ' ';
                    *p++ = ' ';
                }
            }
            *p++ = ' ';
            *p++ = ' ';
            for (row = 0; row < 16; ++row) {
                if (line * 16 + row < len) {
                    int ch = bytes[line * 16 + row];
                    *p++ = 0x20 <= ch && ch < 0x7f ? ch : '.';
                } else {
                    *p++ = ' ';
                }
            }
            *p++ = '\n';
        }
    }
    *p++ = '\0';

    assert(p - buf <= bufsize);

    return buf;
}
