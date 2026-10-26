/*
 * signal_source.c - Shutdown signal detection
 *
 * Mechanism B (inbound command) is the primary implementation.
 * Mechanism A (heartbeat reply flag) is stubbed pending capture.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "signal_source.h"
#include "dxp.h"
#include "log.h"

#define RECV_TIMEOUT_S 10

/* ------------------------------------------------------------------ */
/* Socket helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * set_recv_timeout: set SO_RCVTIMEO on a socket.
 */
static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/*
 * recv_exact: receive exactly len bytes.
 * Returns: 0 on success, -1 on error or premature close.
 */
static int recv_exact(int fd, unsigned char *buf, int len)
{
    int total = 0;
    int n;

    while (total < len) {
        n = (int)recv(fd, buf + total, (size_t)(len - total), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += n;
    }
    return 0;
}

/*
 * send_all: send exactly len bytes.
 * Returns: 0 on success, -1 on error.
 */
static int send_all(int fd, const unsigned char *buf, int len)
{
    int total = 0;
    int n;

    while (total < len) {
        n = (int)send(fd, buf + total, (size_t)(len - total), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

/*
 * generate_seq: produce a random 16-bit sequence number.
 */
static unsigned short generate_seq(void)
{
    unsigned short seq;
    int fd;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, &seq, sizeof(seq)) != (ssize_t)sizeof(seq)) {
            seq = (unsigned short)(time(NULL) ^ getpid());
        }
        close(fd);
    } else {
        seq = (unsigned short)(time(NULL) ^ getpid());
    }
    return seq;
}

/* ------------------------------------------------------------------ */
/* Hex dump helper                                                    */
/* ------------------------------------------------------------------ */

/*
 * hexdump: format raw bytes as hex + printable ASCII for logging.
 * Inputs: buffer, length, output string (must hold len*3 + 3 + len + 1).
 */
static void hexdump(const unsigned char *buf, int len, char *out, int outlen)
{
    char *p;
    int i;
    int remain;

    p = out;
    remain = outlen;

    for (i = 0; i < len && remain > 4; i++) {
        int w;
        w = snprintf(p, (size_t)remain, "%02x ", buf[i]);
        p += w;
        remain -= w;
    }

    if (remain > len + 3) {
        *p++ = '|';
        remain--;
        for (i = 0; i < len && remain > 1; i++) {
            *p = (buf[i] >= 0x20 && buf[i] <= 0x7e)
                 ? (char)buf[i] : '.';
            p++;
            remain--;
        }
        *p++ = '|';
    }
    *p = '\0';
}

/* ------------------------------------------------------------------ */
/* Mechanism B: inbound command                                       */
/* ------------------------------------------------------------------ */

/*
 * recv_accumulate: read from fd in a loop until no more data arrives.
 * Uses a short inter-read timeout so the final read does not block
 * longer than drain_timeout_s seconds.
 * Returns: total bytes read, or -1 if nothing arrived at all.
 */
static int recv_accumulate(int fd, unsigned char *buf, int bufsize,
                           int first_timeout_s, int drain_timeout_s)
{
    int total = 0;
    int n;

    set_recv_timeout(fd, first_timeout_s);

    while (total < bufsize) {
        n = (int)recv(fd, buf + total, (size_t)(bufsize - total), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += n;
        /* After first chunk, use a short timeout to drain remaining */
        set_recv_timeout(fd, drain_timeout_s);
    }

    return (total > 0) ? total : -1;
}

int signal_source_handle_connection(int conn_fd,
                                    const struct config *cfg,
                                    shutdown_event_t *event)
{
    unsigned char hello_buf[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char raw[1024];
    unsigned short initial_seq;
    char dump[512];
    int raw_len;
    int json_start;
    int json_len;

    (void)cfg;
    (void)event;

    set_recv_timeout(conn_fd, RECV_TIMEOUT_S);

    /* Receive hello */
    if (recv_exact(conn_fd, hello_buf, DXP_HELLO_LEN) < 0) {
        log_msg(LOG_WARNING, "connection: failed to receive hello");
        return -1;
    }
    if (memcmp(hello_buf, DXP_HELLO, DXP_HELLO_LEN) != 0) {
        log_msg(LOG_WARNING, "connection: invalid hello message");
        return -1;
    }

    log_msg(LOG_DEBUG, "connection: hello received");

    /* Send sequence number (DxP handshake) */
    initial_seq = generate_seq();
    dxp_write_seq(seq_buf, initial_seq);
    if (send_all(conn_fd, seq_buf, DXP_SEQ_LEN) < 0) {
        log_msg(LOG_WARNING, "connection: failed to send sequence");
        return -1;
    }

    log_msg(LOG_DEBUG, "connection: sent seq %u", (unsigned)initial_seq);

    /* Accumulate all data from iBoot (up to 1024 bytes).
       First read waits 10s, subsequent reads drain with 3s timeout. */
    raw_len = recv_accumulate(conn_fd, raw, (int)sizeof(raw) - 1,
                              RECV_TIMEOUT_S, 3);
    if (raw_len <= 0) {
        log_msg(LOG_WARNING, "connection: no data after hello/seq");
        return -1;
    }

    /* Null-terminate for safe string operations */
    raw[raw_len] = '\0';

    log_msg(LOG_INFO, "connection: received %d bytes total", raw_len);

    /* Hex dump first 128 bytes */
    {
        int dumplen;
        dumplen = (raw_len > 128) ? 128 : raw_len;
        hexdump(raw, dumplen, dump, (int)sizeof(dump));
        log_msg(LOG_DEBUG, "connection: hex[0..%d]: %s",
                dumplen - 1, dump);
    }

    /* Find the JSON payload (skip any prefix bytes before '{') */
    json_start = -1;
    {
        int i;
        for (i = 0; i < raw_len; i++) {
            if (raw[i] == '{') {
                json_start = i;
                break;
            }
        }
    }

    if (json_start < 0) {
        log_msg(LOG_WARNING,
                "connection: no JSON found in %d bytes", raw_len);
        return -1;
    }

    if (json_start > 0) {
        log_msg(LOG_DEBUG,
                "connection: %d prefix byte(s) before JSON: "
                "first=0x%02x",
                json_start, raw[0]);
    }

    json_len = raw_len - json_start;
    log_msg(LOG_INFO, "connection: JSON payload (%d bytes): %.*s",
            json_len, json_len, (char *)(raw + json_start));

    /* Dump any trailing bytes after the first 128 if message is long */
    if (raw_len > 128) {
        int tail;
        tail = raw_len - 128;
        if (tail > 128) tail = 128;
        hexdump(raw + 128, tail, dump, (int)sizeof(dump));
        log_msg(LOG_DEBUG, "connection: hex[128..%d]: %s",
                128 + tail - 1, dump);
    }

    /*
     * TODO: parse JSON to extract command, credentials, and
     * shutdown signal. For now, log the full payload for protocol
     * analysis. The agent does not act on this data yet.
     */
    log_msg(LOG_INFO,
            "connection: JSON protocol detected; capture complete. "
            "No action taken (protocol handler not yet implemented).");

    return -1;
}

/* ------------------------------------------------------------------ */
/* Mechanism A: heartbeat reply flag (stub)                           */
/* ------------------------------------------------------------------ */

int signal_source_check_reply(const unsigned char *response, int len,
                              shutdown_event_t *event)
{
    (void)response;
    (void)len;
    (void)event;
    return -1;
}
