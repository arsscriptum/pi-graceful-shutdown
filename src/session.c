/*
 * session.c - Connection management and state machine
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
#include <netinet/in.h>
#include <arpa/inet.h>

#include "session.h"
#include "dxp.h"
#include "log.h"

#define RECV_TIMEOUT_S 10

/* ------------------------------------------------------------------ */
/* Socket helpers (local to session)                                  */
/* ------------------------------------------------------------------ */

static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

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

/* ------------------------------------------------------------------ */
/* Listener                                                           */
/* ------------------------------------------------------------------ */

/*
 * create_listen_socket: create, bind, and listen on a TCP socket.
 * Returns: fd on success, -1 on error (logged).
 */
static int create_listen_socket(const char *addr, int port)
{
    struct sockaddr_in sa;
    int fd;
    int opt;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg(LOG_ERR, "socket: %s", strerror(errno));
        return -1;
    }

    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        log_msg(LOG_ERR, "invalid listen address: %s", addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_msg(LOG_ERR, "bind %s:%d: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        log_msg(LOG_ERR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/* Session lifecycle                                                  */
/* ------------------------------------------------------------------ */

void session_init(struct session *s)
{
    memset(s, 0, sizeof(*s));
    s->state = STATE_INIT;
    s->listen_fd = -1;
    s->backoff_seconds = SESSION_BACKOFF_INITIAL;
    s->last_heartbeat = time(NULL);
    s->last_connect_attempt = 0;
}

int session_setup(struct session *s, const struct config *cfg)
{
    s->listen_fd = create_listen_socket(cfg->listen_address,
                                        cfg->listen_port);
    if (s->listen_fd < 0) {
        log_msg(LOG_ERR, "failed to create listener");
        s->state = STATE_CONNECTING;
        return -1;
    }

    s->state = STATE_READY;
    s->backoff_seconds = SESSION_BACKOFF_INITIAL;
    return 0;
}

void session_destroy(struct session *s)
{
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}

int session_get_listen_fd(const struct session *s)
{
    return s->listen_fd;
}

int session_accept(const struct session *s, const struct config *cfg,
                   char *peer_ip)
{
    struct sockaddr_in peer;
    socklen_t peerlen;
    char ip_buf[INET_ADDRSTRLEN];
    int conn_fd;

    peerlen = sizeof(peer);
    conn_fd = accept(s->listen_fd, (struct sockaddr *)&peer, &peerlen);
    if (conn_fd < 0) {
        if (errno != EINTR) {
            log_msg(LOG_WARNING, "accept: %s", strerror(errno));
        }
        return -1;
    }

    inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));

    /* Source IP validation (defense in depth) */
    if (peer.sin_addr.s_addr != cfg->iboot_addr_resolved.s_addr) {
        log_msg(LOG_WARNING, "rejected connection from %s", ip_buf);
        close(conn_fd);
        return -1;
    }

    log_msg(LOG_INFO, "accepted connection from %s", ip_buf);

    if (peer_ip != NULL) {
        strncpy(peer_ip, ip_buf, 15);
        peer_ip[15] = '\0';
    }

    return conn_fd;
}

/* ------------------------------------------------------------------ */
/* Heartbeat                                                          */
/* ------------------------------------------------------------------ */

int session_heartbeat_due(const struct session *s, int interval)
{
    time_t now;
    time_t elapsed;

    if (interval <= 0) return 0;

    now = time(NULL);
    elapsed = now - s->last_heartbeat;
    return elapsed >= (time_t)interval;
}

int session_heartbeat_timeout_ms(const struct session *s, int interval)
{
    time_t now;
    time_t elapsed;
    time_t remaining;

    if (interval <= 0) return -1;

    now = time(NULL);
    elapsed = now - s->last_heartbeat;
    remaining = (time_t)interval - elapsed;
    if (remaining <= 0) return 0;
    return (int)(remaining * 1000);
}

int session_send_heartbeat(struct session *s, const struct config *cfg)
{
    unsigned char hello[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char hdr_buf[DXP_HDR_SIZE];
    unsigned char resp;
    unsigned short seq;
    struct sockaddr_in addr;
    dxp_header_t hdr;
    int fd;

    s->last_heartbeat = time(NULL);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_msg(LOG_WARNING, "heartbeat: socket: %s", strerror(errno));
        return -1;
    }

    set_recv_timeout(fd, RECV_TIMEOUT_S);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = cfg->iboot_addr_resolved;
    addr.sin_port = htons((unsigned short)cfg->iboot_port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg(LOG_WARNING, "heartbeat: connect to %s:%d: %s",
                cfg->iboot_address, cfg->iboot_port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Send hello */
    memcpy(hello, DXP_HELLO, DXP_HELLO_LEN);
    if (send_all(fd, hello, DXP_HELLO_LEN) < 0) {
        log_msg(LOG_WARNING, "heartbeat: send hello: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Receive sequence */
    if (recv_exact(fd, seq_buf, DXP_SEQ_LEN) < 0) {
        log_msg(LOG_WARNING, "heartbeat: recv sequence: %s",
                strerror(errno));
        close(fd);
        return -1;
    }

    seq = dxp_read_seq(seq_buf);
    seq++;

    /* Build keepAlive header */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_KEEPALIVE;
    memcpy(hdr.username, cfg->shared_username, DXP_USERNAME_FIELD);
    memcpy(hdr.password, cfg->shared_password, DXP_PASSWORD_FIELD);
    hdr.desc = DXP_KEEPALIVE_NULL;
    hdr.param = 0;
    hdr.seq = seq;

    dxp_build_header(hdr_buf, &hdr);

    if (send_all(fd, hdr_buf, DXP_HDR_SIZE) < 0) {
        log_msg(LOG_WARNING, "heartbeat: send command: %s",
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Receive response */
    if (recv_exact(fd, &resp, 1) < 0) {
        log_msg(LOG_WARNING, "heartbeat: recv response: %s",
                strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);

    if (resp == DXP_RESP_OK) {
        log_msg(LOG_DEBUG, "heartbeat: OK");
    } else {
        log_msg(LOG_WARNING, "heartbeat: iBoot returned error");
    }

    return (resp == DXP_RESP_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* State accessors                                                    */
/* ------------------------------------------------------------------ */

agent_state_t session_get_state(const struct session *s)
{
    return s->state;
}

void session_set_state(struct session *s, agent_state_t state)
{
    if (s->state != state) {
        log_msg(LOG_DEBUG, "state: %s -> %s",
                session_state_name(s->state),
                session_state_name(state));
    }
    s->state = state;
}

const char *session_state_name(agent_state_t state)
{
    switch (state) {
    case STATE_INIT:          return "INIT";
    case STATE_CONNECTING:    return "CONNECTING";
    case STATE_READY:         return "READY";
    case STATE_SHUTTING_DOWN: return "SHUTTING_DOWN";
    case STATE_STOPPING:      return "STOPPING";
    default:                  return "UNKNOWN";
    }
}
