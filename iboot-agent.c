/*
 * iboot-agent.c - Graceful-shutdown agent for iBoot-G2+
 *
 * Listens for DxP shutdown commands from a Dataprobe iBoot-G2+
 * network power switch and performs a clean local halt before
 * the iBoot cuts AC power.
 *
 * Build: cc -ansi -pedantic -Wall -Wextra -O2
 *            -D_POSIX_C_SOURCE=200112L -o iboot-agent iboot-agent.c
 *
 * See CLAUDE.md for the full requirements specification.
 */

#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#include "dxp.h"

/* ------------------------------------------------------------------ */
/* Version and program name                                           */
/* ------------------------------------------------------------------ */

#define PROGRAM_NAME    "iboot-agent"
#define PROGRAM_VERSION "0.1.0"
#define DEFAULT_CONFIG  "/etc/iboot-agent/agent.conf"
#define RECV_TIMEOUT_S  10
#define MAX_LINE        512
#define MAX_PATH_LEN    256
#define MAX_CMD_LEN     256

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

struct config {
    char iboot_address[MAX_PATH_LEN];
    int  iboot_port;
    char shared_username[DXP_USERNAME_FIELD];
    char shared_password[DXP_PASSWORD_FIELD];
    int  heartbeat_interval;
    char listen_address[MAX_PATH_LEN];
    int  listen_port;
    char halt_command[MAX_CMD_LEN];
    int  log_level;
    struct in_addr iboot_addr_resolved;
};

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_reload_requested   = 0;
static int g_halt_initiated = 0;
static int g_dry_run        = 0;
static int g_verbose        = 0;
static int g_foreground     = 1;
static char g_config_path[MAX_PATH_LEN];

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void usage(void);
static void version(void);
static int  parse_args(int argc, char **argv);
static int  load_config(const char *path, struct config *cfg);
static int  check_config_perms(const char *path);
static int  resolve_address(const char *host, struct in_addr *out);
static void setup_signals(void);
static void sig_handler(int signo);
static int  create_listen_socket(const char *addr, int port);
static void handle_connection(int fd, const struct config *cfg);
static int  send_heartbeat(const struct config *cfg);
static void execute_halt(const struct config *cfg);
static int  recv_exact(int fd, unsigned char *buf, int len);
static int  send_all(int fd, const unsigned char *buf, int len);
static void set_recv_timeout(int fd, int seconds);
static void dxp_parse_header(const unsigned char *wire, dxp_header_t *hdr);
static void dxp_build_header(unsigned char *wire, const dxp_header_t *hdr);
static unsigned short dxp_read_seq(const unsigned char *buf);
static void dxp_write_seq(unsigned char *buf, unsigned short seq);
static unsigned short generate_seq(void);
static int  is_shutdown_command(const dxp_header_t *hdr);
static const char *command_name(unsigned char cmd);
static int  log_level_from_string(const char *s);
static void agent_log(int priority, const char *fmt, ...);

/* stdarg for agent_log */
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Logging helper                                                     */
/* ------------------------------------------------------------------ */

/*
 * agent_log: log to syslog and optionally to stderr.
 * Inputs: syslog priority, printf-style format and args.
 * Output: none.  Cannot fail in a meaningful way.
 */
static void agent_log(int priority, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);

    if (g_foreground && (g_verbose || priority <= LOG_WARNING)) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

/* ------------------------------------------------------------------ */
/* Usage and version                                                  */
/* ------------------------------------------------------------------ */

/*
 * usage: print help text and exit.
 * Inputs: none.  Output: text to stdout.  Does not return.
 */
static void usage(void)
{
    printf("Usage: %s [OPTIONS]\n\n", PROGRAM_NAME);
    printf("Options:\n");
    printf("  --config <path>   Configuration file (default: %s)\n",
           DEFAULT_CONFIG);
    printf("  --dry-run         Log shutdown signals without halting\n");
    printf("  --foreground      Run in foreground (default)\n");
    printf("  --verbose         Increase log verbosity to debug\n");
    printf("  --version         Print version and exit\n");
    printf("  --help            Print this help and exit\n");
    exit(0);
}

/*
 * version: print version string and exit.
 * Inputs: none.  Output: text to stdout.  Does not return.
 */
static void version(void)
{
    printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                   */
/* ------------------------------------------------------------------ */

/*
 * parse_args: parse command-line arguments into global state.
 * Inputs: argc, argv from main.
 * Returns: 0 on success, -1 on error (message already printed).
 */
static int parse_args(int argc, char **argv)
{
    int i;

    strncpy(g_config_path, DEFAULT_CONFIG, sizeof(g_config_path) - 1);
    g_config_path[sizeof(g_config_path) - 1] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
        } else if (strcmp(argv[i], "--version") == 0) {
            version();
        } else if (strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--config requires an argument\n");
                return -1;
            }
            strncpy(g_config_path, argv[i], sizeof(g_config_path) - 1);
            g_config_path[sizeof(g_config_path) - 1] = '\0';
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            g_dry_run = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            g_foreground = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Configuration file                                                 */
/* ------------------------------------------------------------------ */

/*
 * trim: strip leading and trailing whitespace in place.
 * Inputs: null-terminated string.
 * Returns: pointer to first non-whitespace character.
 * Failure: returns pointer to empty string if input is all whitespace.
 */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' ||
                       *end == '\n' || *end == '\r'))
        end--;
    *(end + 1) = '\0';
    return s;
}

/*
 * log_level_from_string: convert a log level name to syslog priority.
 * Inputs: level name string.
 * Returns: syslog priority, or -1 on unrecognized input.
 */
static int log_level_from_string(const char *s)
{
    if (strcmp(s, "error") == 0) return LOG_ERR;
    if (strcmp(s, "warn")  == 0) return LOG_WARNING;
    if (strcmp(s, "info")  == 0) return LOG_INFO;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    return -1;
}

/*
 * set_config_defaults: initialize a config struct with default values.
 * Inputs: pointer to config struct.
 * Output: struct filled with defaults.
 */
static void set_config_defaults(struct config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->iboot_port = DXP_DEFAULT_PORT;
    cfg->listen_port = DXP_DEFAULT_PORT;
    cfg->heartbeat_interval = 0;
    strncpy(cfg->halt_command, "/sbin/poweroff", MAX_CMD_LEN - 1);
    cfg->log_level = LOG_INFO;
}

/*
 * load_config: parse a key=value configuration file.
 * Inputs: file path, pointer to config struct (pre-filled with defaults).
 * Returns: 0 on success, -1 on error (message printed to stderr).
 */
static int load_config(const char *path, struct config *cfg)
{
    FILE *fp;
    char line[MAX_LINE];
    char *p;
    char *eq;
    char *key;
    char *val;
    int lineno;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "cannot open config %s: %s\n", path, strerror(errno));
        return -1;
    }

    lineno = 0;
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        lineno++;

        p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        eq = strchr(p, '=');
        if (eq == NULL) {
            fprintf(stderr, "%s:%d: missing '=' in line\n", path, lineno);
            fclose(fp);
            return -1;
        }

        *eq = '\0';
        key = trim(p);
        val = trim(eq + 1);

        if (strcmp(key, "iboot_address") == 0) {
            strncpy(cfg->iboot_address, val, MAX_PATH_LEN - 1);
        } else if (strcmp(key, "iboot_port") == 0) {
            cfg->iboot_port = atoi(val);
        } else if (strcmp(key, "shared_username") == 0) {
            if (strlen(val) > DXP_USERNAME_MAX) {
                fprintf(stderr, "%s:%d: username exceeds %d characters\n",
                        path, lineno, DXP_USERNAME_MAX);
                fclose(fp);
                return -1;
            }
            strncpy(cfg->shared_username, val, DXP_USERNAME_FIELD - 1);
        } else if (strcmp(key, "shared_password") == 0) {
            if (strlen(val) > DXP_PASSWORD_MAX) {
                fprintf(stderr, "%s:%d: password exceeds %d characters\n",
                        path, lineno, DXP_PASSWORD_MAX);
                fclose(fp);
                return -1;
            }
            strncpy(cfg->shared_password, val, DXP_PASSWORD_FIELD - 1);
        } else if (strcmp(key, "heartbeat_interval_seconds") == 0) {
            cfg->heartbeat_interval = atoi(val);
        } else if (strcmp(key, "listen_address") == 0) {
            strncpy(cfg->listen_address, val, MAX_PATH_LEN - 1);
        } else if (strcmp(key, "listen_port") == 0) {
            cfg->listen_port = atoi(val);
        } else if (strcmp(key, "halt_command") == 0) {
            strncpy(cfg->halt_command, val, MAX_CMD_LEN - 1);
        } else if (strcmp(key, "log_level") == 0) {
            cfg->log_level = log_level_from_string(val);
            if (cfg->log_level < 0) {
                fprintf(stderr, "%s:%d: unknown log level '%s'\n",
                        path, lineno, val);
                fclose(fp);
                return -1;
            }
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    /* Validate required fields */
    if (cfg->iboot_address[0] == '\0') {
        fprintf(stderr, "%s: iboot_address is required\n", path);
        return -1;
    }
    if (cfg->shared_username[0] == '\0') {
        fprintf(stderr, "%s: shared_username is required\n", path);
        return -1;
    }
    if (cfg->shared_password[0] == '\0') {
        fprintf(stderr, "%s: shared_password is required\n", path);
        return -1;
    }
    if (cfg->listen_address[0] == '\0') {
        fprintf(stderr, "%s: listen_address is required\n", path);
        return -1;
    }
    if (cfg->iboot_port <= 0 || cfg->iboot_port > 65535) {
        fprintf(stderr, "%s: iboot_port out of range\n", path);
        return -1;
    }
    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
        fprintf(stderr, "%s: listen_port out of range\n", path);
        return -1;
    }

    return 0;
}

/*
 * check_config_perms: verify the config file is mode 0600 owned by root.
 * Inputs: file path.
 * Returns: 0 if permissions are acceptable, -1 otherwise.
 */
static int check_config_perms(const char *path)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "cannot stat %s: %s\n", path, strerror(errno));
        return -1;
    }
    if ((st.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) != 0) {
        fprintf(stderr,
                "%s: config file must not be group- or world-readable "
                "(mode 0600 required); current mode %04o\n",
                path, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Address resolution                                                 */
/* ------------------------------------------------------------------ */

/*
 * resolve_address: resolve an IPv4 address or hostname to in_addr.
 * Inputs: hostname or dotted-quad string, output in_addr pointer.
 * Returns: 0 on success, -1 on failure (message logged).
 */
static int resolve_address(const char *host, struct in_addr *out)
{
    struct addrinfo hints;
    struct addrinfo *res;
    struct sockaddr_in *sa;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cannot resolve '%s': %s\n", host, gai_strerror(rc));
        return -1;
    }

    sa = (struct sockaddr_in *)res->ai_addr;
    *out = sa->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                    */
/* ------------------------------------------------------------------ */

/*
 * sig_handler: handle SIGTERM, SIGINT, SIGHUP.
 * Sets global flags; async-signal-safe.
 */
static void sig_handler(int signo)
{
    if (signo == SIGTERM || signo == SIGINT) {
        g_shutdown_requested = 1;
    } else if (signo == SIGHUP) {
        g_reload_requested = 1;
    }
}

/*
 * setup_signals: install signal handlers for SIGTERM, SIGINT,
 * SIGHUP and ignore SIGPIPE.
 */
static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * set_recv_timeout: set SO_RCVTIMEO on a socket.
 * Inputs: socket fd, timeout in seconds.
 */
static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/*
 * recv_exact: receive exactly len bytes from fd.
 * Inputs: socket fd, buffer, byte count.
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
 * send_all: send exactly len bytes to fd.
 * Inputs: socket fd, buffer, byte count.
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
 * create_listen_socket: create, bind, and listen on a TCP socket.
 * Inputs: bind address string (IPv4), port number.
 * Returns: listening socket fd, or -1 on error.
 */
static int create_listen_socket(const char *addr, int port)
{
    struct sockaddr_in sa;
    int fd;
    int opt;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        agent_log(LOG_ERR, "socket: %s", strerror(errno));
        return -1;
    }

    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        agent_log(LOG_ERR, "invalid listen address: %s", addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        agent_log(LOG_ERR, "bind %s:%d: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        agent_log(LOG_ERR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/* DxP protocol helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * dxp_read_seq: read a little-endian 16-bit sequence number.
 * Inputs: 2-byte buffer.
 * Returns: the sequence number.
 */
static unsigned short dxp_read_seq(const unsigned char *buf)
{
    return (unsigned short)((unsigned short)buf[0] |
                            ((unsigned short)buf[1] << 8));
}

/*
 * dxp_write_seq: write a little-endian 16-bit sequence number.
 * Inputs: 2-byte buffer, sequence value.
 */
static void dxp_write_seq(unsigned char *buf, unsigned short seq)
{
    buf[0] = (unsigned char)(seq & 0xFF);
    buf[1] = (unsigned char)((seq >> 8) & 0xFF);
}

/*
 * dxp_parse_header: parse 47 wire bytes into a dxp_header_t.
 * Inputs: 47-byte wire buffer, output header struct.
 * Ensures username and password are null-terminated.
 */
static void dxp_parse_header(const unsigned char *wire, dxp_header_t *hdr)
{
    hdr->command = wire[DXP_HDR_OFF_COMMAND];
    memcpy(hdr->username, &wire[DXP_HDR_OFF_USERNAME], DXP_USERNAME_FIELD);
    hdr->username[DXP_USERNAME_MAX] = '\0';
    memcpy(hdr->password, &wire[DXP_HDR_OFF_PASSWORD], DXP_PASSWORD_FIELD);
    hdr->password[DXP_PASSWORD_MAX] = '\0';
    hdr->desc = wire[DXP_HDR_OFF_DESC];
    hdr->param = wire[DXP_HDR_OFF_PARAM];
    hdr->seq = dxp_read_seq(&wire[DXP_HDR_OFF_SEQ]);
}

/*
 * dxp_build_header: serialize a dxp_header_t into 47 wire bytes.
 * Inputs: 47-byte wire buffer (zeroed by caller), header struct.
 */
static void dxp_build_header(unsigned char *wire, const dxp_header_t *hdr)
{
    memset(wire, 0, DXP_HDR_SIZE);
    wire[DXP_HDR_OFF_COMMAND] = hdr->command;
    memcpy(&wire[DXP_HDR_OFF_USERNAME], hdr->username, DXP_USERNAME_FIELD);
    memcpy(&wire[DXP_HDR_OFF_PASSWORD], hdr->password, DXP_PASSWORD_FIELD);
    wire[DXP_HDR_OFF_DESC] = hdr->desc;
    wire[DXP_HDR_OFF_PARAM] = hdr->param;
    dxp_write_seq(&wire[DXP_HDR_OFF_SEQ], hdr->seq);
}

/*
 * generate_seq: produce a random 16-bit sequence number.
 * Returns: random unsigned short.  Falls back to time/pid if
 *          /dev/urandom is unavailable.
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

/*
 * command_name: return a human-readable name for a DxP command class.
 * Inputs: command byte.
 * Returns: static string (never NULL).
 */
static const char *command_name(unsigned char cmd)
{
    switch (cmd) {
    case DXP_CMND_NULL:      return "null";
    case DXP_CMND_SET:       return "set";
    case DXP_CMND_GET:       return "get";
    case DXP_CMND_IO:        return "io";
    case DXP_CMND_KEEPALIVE: return "keepAlive";
    case DXP_CMND_RSS:       return "rss";
    case DXP_CMND_RCU:       return "rcu";
    case DXP_CMND_PCCONTROL: return "pcControl";
    default:                 return "unknown";
    }
}

/*
 * is_shutdown_command: determine if a parsed DxP header represents
 * a shutdown signal.
 *
 * Accepts:
 *   - eCmnd_pcControl (7) with any descriptor
 *   - eCmnd_io (3) with eIO_changeRelay (1), any relay, state=RELAX
 *
 * These cover the most likely graceful-shutdown signaling methods.
 * Tighten after validating against a packet capture (see AC1).
 *
 * Inputs: parsed header.
 * Returns: 1 if shutdown command, 0 otherwise.
 */
static int is_shutdown_command(const dxp_header_t *hdr)
{
    if (hdr->command == DXP_CMND_PCCONTROL) {
        return 1;
    }
    if (hdr->command == DXP_CMND_IO &&
        hdr->desc == DXP_IO_CHANGE_RELAY) {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DxP connection handler (server side)                               */
/* ------------------------------------------------------------------ */

/*
 * handle_connection: process one inbound DxP session from the iBoot.
 *
 * Protocol (agent is server, iBoot is client):
 *   1. Receive "hello-000" (10 bytes)
 *   2. Send 2-byte sequence number (little-endian)
 *   3. Receive 47-byte DxP header
 *   4. Validate sequence (must be initial_seq + 1)
 *   5. Validate credentials
 *   6. If shutdown command: ACK and halt
 *   7. Otherwise: ACK and ignore
 *
 * Inputs: accepted socket fd, configuration.
 * Output: may trigger halt.  Closes fd before returning only when
 *         the caller is not expected to close it; here the caller
 *         always closes, so this function does not close fd.
 */
static void handle_connection(int fd, const struct config *cfg)
{
    unsigned char hello_buf[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char hdr_buf[DXP_HDR_SIZE];
    unsigned char resp;
    unsigned short initial_seq;
    dxp_header_t hdr;

    set_recv_timeout(fd, RECV_TIMEOUT_S);

    /* Step 1: receive hello */
    if (recv_exact(fd, hello_buf, DXP_HELLO_LEN) < 0) {
        agent_log(LOG_WARNING, "connection: failed to receive hello");
        return;
    }
    if (memcmp(hello_buf, DXP_HELLO, DXP_HELLO_LEN) != 0) {
        agent_log(LOG_WARNING, "connection: invalid hello message");
        return;
    }

    agent_log(LOG_DEBUG, "connection: hello received");

    /* Step 2: send sequence number */
    initial_seq = generate_seq();
    dxp_write_seq(seq_buf, initial_seq);
    if (send_all(fd, seq_buf, DXP_SEQ_LEN) < 0) {
        agent_log(LOG_WARNING, "connection: failed to send sequence");
        return;
    }

    agent_log(LOG_DEBUG, "connection: sent seq %u", (unsigned)initial_seq);

    /* Step 3: receive header */
    if (recv_exact(fd, hdr_buf, DXP_HDR_SIZE) < 0) {
        agent_log(LOG_WARNING, "connection: failed to receive header");
        return;
    }

    dxp_parse_header(hdr_buf, &hdr);

    agent_log(LOG_DEBUG,
              "connection: command=%s(%u) desc=%u param=%u seq=%u",
              command_name(hdr.command), (unsigned)hdr.command,
              (unsigned)hdr.desc, (unsigned)hdr.param, (unsigned)hdr.seq);

    /* Step 4: validate sequence number */
    if (hdr.seq != (unsigned short)(initial_seq + 1)) {
        agent_log(LOG_WARNING,
                  "connection: bad sequence: expected %u, got %u",
                  (unsigned)(unsigned short)(initial_seq + 1),
                  (unsigned)hdr.seq);
        resp = DXP_RESP_ERROR;
        send_all(fd, &resp, 1);
        return;
    }

    /* Step 5: validate credentials */
    if (strcmp(hdr.username, cfg->shared_username) != 0 ||
        strcmp(hdr.password, cfg->shared_password) != 0) {
        agent_log(LOG_WARNING,
                  "connection: authentication failure (user='%s')",
                  hdr.username);
        resp = DXP_RESP_ERROR;
        send_all(fd, &resp, 1);
        return;
    }

    agent_log(LOG_DEBUG, "connection: credentials validated");

    /* Step 6: process command */
    if (is_shutdown_command(&hdr)) {
        agent_log(LOG_INFO,
                  "received authenticated shutdown signal: "
                  "command=%s(%u) desc=%u",
                  command_name(hdr.command), (unsigned)hdr.command,
                  (unsigned)hdr.desc);

        resp = DXP_RESP_OK;
        send_all(fd, &resp, 1);

        execute_halt(cfg);
    } else {
        agent_log(LOG_INFO,
                  "received authenticated non-shutdown command: "
                  "command=%s(%u) desc=%u (ignored)",
                  command_name(hdr.command), (unsigned)hdr.command,
                  (unsigned)hdr.desc);
        resp = DXP_RESP_OK;
        send_all(fd, &resp, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Heartbeat (client side)                                            */
/* ------------------------------------------------------------------ */

/*
 * send_heartbeat: connect to the iBoot and send a DxP keepAlive.
 * Inputs: configuration with resolved iBoot address.
 * Returns: 0 on success, -1 on failure (logged).
 */
static int send_heartbeat(const struct config *cfg)
{
    unsigned char hello[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char hdr_buf[DXP_HDR_SIZE];
    unsigned char resp;
    unsigned short seq;
    struct sockaddr_in addr;
    dxp_header_t hdr;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        agent_log(LOG_WARNING, "heartbeat: socket: %s", strerror(errno));
        return -1;
    }

    set_recv_timeout(fd, RECV_TIMEOUT_S);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr = cfg->iboot_addr_resolved;
    addr.sin_port = htons((unsigned short)cfg->iboot_port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        agent_log(LOG_WARNING, "heartbeat: connect to %s:%d: %s",
                  cfg->iboot_address, cfg->iboot_port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Send hello */
    memcpy(hello, DXP_HELLO, DXP_HELLO_LEN);
    if (send_all(fd, hello, DXP_HELLO_LEN) < 0) {
        agent_log(LOG_WARNING, "heartbeat: send hello: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Receive sequence */
    if (recv_exact(fd, seq_buf, DXP_SEQ_LEN) < 0) {
        agent_log(LOG_WARNING, "heartbeat: recv sequence: %s",
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
        agent_log(LOG_WARNING, "heartbeat: send command: %s",
                  strerror(errno));
        close(fd);
        return -1;
    }

    /* Receive response */
    if (recv_exact(fd, &resp, 1) < 0) {
        agent_log(LOG_WARNING, "heartbeat: recv response: %s",
                  strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);

    if (resp == DXP_RESP_OK) {
        agent_log(LOG_DEBUG, "heartbeat: OK");
    } else {
        agent_log(LOG_WARNING, "heartbeat: iBoot returned error");
    }

    return (resp == DXP_RESP_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Halt execution                                                     */
/* ------------------------------------------------------------------ */

/*
 * execute_halt: invoke the configured halt command or log in dry-run.
 * Inputs: configuration.
 * Output: sets g_halt_initiated latch.  In non-dry-run mode, the
 *         system will shut down.  Subsequent calls are no-ops.
 */
static void execute_halt(const struct config *cfg)
{
    int rc;

    if (g_halt_initiated) {
        agent_log(LOG_INFO, "shutdown already initiated, ignoring signal");
        return;
    }
    g_halt_initiated = 1;

    if (g_dry_run) {
        agent_log(LOG_INFO,
                  "DRY RUN: would execute halt command: %s",
                  cfg->halt_command);
        return;
    }

    agent_log(LOG_INFO, "executing halt: %s", cfg->halt_command);
    rc = system(cfg->halt_command);
    if (rc != 0) {
        agent_log(LOG_ERR, "halt command returned %d", rc);
    }
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                    */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct config cfg;
    struct pollfd pfd;
    struct sockaddr_in peer;
    socklen_t peerlen;
    char peer_ip[INET_ADDRSTRLEN];
    char iboot_ip[INET_ADDRSTRLEN];
    int listen_fd;
    int conn_fd;
    int nready;
    int poll_timeout_ms;
    time_t last_heartbeat;
    time_t now;
    time_t elapsed;

    /* Parse arguments */
    if (parse_args(argc, argv) < 0) {
        fprintf(stderr, "try '%s --help'\n", PROGRAM_NAME);
        return 1;
    }

    /* Check config permissions */
    if (check_config_perms(g_config_path) < 0) {
        return 1;
    }

    /* Load configuration */
    set_config_defaults(&cfg);
    if (load_config(g_config_path, &cfg) < 0) {
        return 1;
    }

    /* Resolve iBoot address */
    if (resolve_address(cfg.iboot_address, &cfg.iboot_addr_resolved) < 0) {
        return 1;
    }
    inet_ntop(AF_INET, &cfg.iboot_addr_resolved, iboot_ip, sizeof(iboot_ip));

    /* Open syslog */
    openlog(PROGRAM_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    setlogmask(LOG_UPTO(g_verbose ? LOG_DEBUG : cfg.log_level));

    /* Install signal handlers */
    setup_signals();

    /* Create listen socket */
    listen_fd = create_listen_socket(cfg.listen_address, cfg.listen_port);
    if (listen_fd < 0) {
        agent_log(LOG_ERR, "failed to create listen socket");
        closelog();
        return 1;
    }

    agent_log(LOG_INFO,
              "%s %s starting: listening on %s:%d, "
              "iboot at %s (%s:%d)%s%s",
              PROGRAM_NAME, PROGRAM_VERSION,
              cfg.listen_address, cfg.listen_port,
              iboot_ip, cfg.iboot_address, cfg.iboot_port,
              g_dry_run ? ", DRY RUN mode" : "",
              cfg.heartbeat_interval > 0 ? ", heartbeat enabled" : "");

    /* Initialize heartbeat timer */
    last_heartbeat = time(NULL);

    /* Main event loop */
    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    while (!g_shutdown_requested) {
        /* Handle SIGHUP reload */
        if (g_reload_requested) {
            struct config new_cfg;

            g_reload_requested = 0;
            agent_log(LOG_INFO, "reloading configuration from %s",
                      g_config_path);

            if (check_config_perms(g_config_path) < 0) {
                agent_log(LOG_ERR, "config reload failed: permissions");
            } else {
                set_config_defaults(&new_cfg);
                if (load_config(g_config_path, &new_cfg) < 0) {
                    agent_log(LOG_ERR, "config reload failed: parse error");
                } else if (resolve_address(new_cfg.iboot_address,
                                  &new_cfg.iboot_addr_resolved) < 0) {
                    agent_log(LOG_ERR,
                              "config reload failed: address resolution");
                } else {
                    if (strcmp(new_cfg.listen_address,
                              cfg.listen_address) != 0 ||
                        new_cfg.listen_port != cfg.listen_port) {
                        agent_log(LOG_WARNING,
                                  "listen_address or listen_port changed; "
                                  "restart required for this to take effect");
                    }
                    cfg = new_cfg;
                    inet_ntop(AF_INET, &cfg.iboot_addr_resolved,
                              iboot_ip, sizeof(iboot_ip));
                    setlogmask(
                        LOG_UPTO(g_verbose ? LOG_DEBUG : cfg.log_level));
                    agent_log(LOG_INFO, "configuration reloaded");
                }
            }
        }

        /* Calculate poll timeout for heartbeat */
        if (cfg.heartbeat_interval > 0) {
            now = time(NULL);
            elapsed = now - last_heartbeat;
            if (elapsed >= (time_t)cfg.heartbeat_interval) {
                send_heartbeat(&cfg);
                last_heartbeat = time(NULL);
                elapsed = 0;
            }
            poll_timeout_ms =
                (int)((time_t)cfg.heartbeat_interval - elapsed) * 1000;
            if (poll_timeout_ms <= 0) poll_timeout_ms = 1000;
        } else {
            poll_timeout_ms = -1;
        }

        nready = poll(&pfd, 1, poll_timeout_ms);

        if (nready < 0) {
            if (errno == EINTR) continue;
            agent_log(LOG_ERR, "poll: %s", strerror(errno));
            break;
        }

        if (nready == 0) {
            /* Timeout: heartbeat interval elapsed */
            if (cfg.heartbeat_interval > 0) {
                send_heartbeat(&cfg);
                last_heartbeat = time(NULL);
            }
            continue;
        }

        if (!(pfd.revents & POLLIN)) continue;

        /* Accept inbound connection */
        peerlen = sizeof(peer);
        conn_fd = accept(listen_fd, (struct sockaddr *)&peer, &peerlen);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            agent_log(LOG_WARNING, "accept: %s", strerror(errno));
            continue;
        }

        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));

        /* Verify source address */
        if (peer.sin_addr.s_addr != cfg.iboot_addr_resolved.s_addr) {
            agent_log(LOG_WARNING,
                      "rejected connection from %s (expected %s)",
                      peer_ip, iboot_ip);
            close(conn_fd);
            continue;
        }

        agent_log(LOG_INFO, "accepted connection from %s", peer_ip);
        handle_connection(conn_fd, &cfg);
        close(conn_fd);
    }

    /* Clean shutdown */
    agent_log(LOG_INFO, "shutting down");
    close(listen_fd);
    closelog();
    return 0;
}
