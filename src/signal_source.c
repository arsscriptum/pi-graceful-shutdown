/*
 * signal_source.c - Shutdown signal detection
 *
 * Mechanism B (inbound command) is the primary implementation.
 * Mechanism A (heartbeat reply flag) is stubbed pending capture.
 */

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
/* Mechanism B: inbound command                                       */
/* ------------------------------------------------------------------ */

int signal_source_handle_connection(int conn_fd,
                                    const struct config *cfg,
                                    shutdown_event_t *event)
{
    unsigned char hello_buf[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char hdr_buf[DXP_HDR_SIZE];
    unsigned char resp;
    unsigned short initial_seq;
    dxp_header_t hdr;

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

    /* Send sequence number */
    initial_seq = generate_seq();
    dxp_write_seq(seq_buf, initial_seq);
    if (send_all(conn_fd, seq_buf, DXP_SEQ_LEN) < 0) {
        log_msg(LOG_WARNING, "connection: failed to send sequence");
        return -1;
    }

    log_msg(LOG_DEBUG, "connection: sent seq %u", (unsigned)initial_seq);

    /* Receive header */
    if (recv_exact(conn_fd, hdr_buf, DXP_HDR_SIZE) < 0) {
        log_msg(LOG_WARNING, "connection: failed to receive header");
        return -1;
    }

    dxp_parse_header(hdr_buf, &hdr);

    log_msg(LOG_DEBUG,
            "connection: command=%s(%u) desc=%u param=%u seq=%u",
            dxp_command_name(hdr.command), (unsigned)hdr.command,
            (unsigned)hdr.desc, (unsigned)hdr.param, (unsigned)hdr.seq);

    /* Validate sequence */
    if (hdr.seq != (unsigned short)(initial_seq + 1)) {
        log_msg(LOG_WARNING,
                "connection: bad sequence: expected %u, got %u",
                (unsigned)(unsigned short)(initial_seq + 1),
                (unsigned)hdr.seq);
        resp = DXP_RESP_ERROR;
        send_all(conn_fd, &resp, 1);
        return -1;
    }

    /* Validate credentials */
    if (!dxp_validate_credentials(&hdr, cfg->shared_username,
                                  cfg->shared_password)) {
        log_msg(LOG_WARNING,
                "connection: authentication failure (user='%s')",
                hdr.username);
        resp = DXP_RESP_ERROR;
        send_all(conn_fd, &resp, 1);
        return -1;
    }

    log_msg(LOG_DEBUG, "connection: credentials validated");

    /* Send ACK */
    resp = DXP_RESP_OK;
    send_all(conn_fd, &resp, 1);

    /* Check for shutdown command */
    if (dxp_is_shutdown_command(&hdr)) {
        log_msg(LOG_INFO,
                "received authenticated shutdown signal: "
                "command=%s(%u) desc=%u",
                dxp_command_name(hdr.command), (unsigned)hdr.command,
                (unsigned)hdr.desc);

        if (event != NULL) {
            event->command = hdr.command;
            event->desc = hdr.desc;
            event->source_ip[0] = '\0';
        }
        return 0;
    }

    log_msg(LOG_INFO,
            "received authenticated non-shutdown command: "
            "command=%s(%u) desc=%u (ignored)",
            dxp_command_name(hdr.command), (unsigned)hdr.command,
            (unsigned)hdr.desc);
    return 1;
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
