/*
 * main.c - iboot-agent entry point
 *
 * Argument parsing, self-pipe signal handling, poll loop that wires
 * session, signal_source, halt, config, and log modules together.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <poll.h>

#include "config.h"
#include "dxp.h"
#include "halt.h"
#include "log.h"
#include "session.h"
#include "signal_source.h"

#define PROGRAM_NAME    "iboot-agent"
#define PROGRAM_VERSION "0.1.0"
#define DEFAULT_CONFIG  "/etc/iboot-agent/agent.conf"

/* Self-pipe for signal delivery into the poll loop */
static int g_signal_pipe[2] = { -1, -1 };

/* ------------------------------------------------------------------ */
/* Self-pipe signal handler                                           */
/* ------------------------------------------------------------------ */

/*
 * signal_handler: write the signal number into the pipe.
 * Inputs: signal number.
 * Async-signal-safe: uses write() only.
 */
static void signal_handler(int signo)
{
    unsigned char s;
    int saved_errno;
    ssize_t wr;

    saved_errno = errno;
    s = (unsigned char)signo;
    wr = write(g_signal_pipe[1], &s, 1);
    (void)wr;
    errno = saved_errno;
}

/*
 * set_nonblock: set O_NONBLOCK on a file descriptor.
 * Returns: 0 on success, -1 on failure.
 */
static int set_nonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * setup_signal_pipe: create the self-pipe and install signal handlers.
 * Returns: 0 on success, -1 on failure.
 */
static int setup_signal_pipe(void)
{
    struct sigaction sa;

    if (pipe(g_signal_pipe) < 0) return -1;
    if (set_nonblock(g_signal_pipe[0]) < 0) return -1;
    if (set_nonblock(g_signal_pipe[1]) < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0) return -1;
    if (sigaction(SIGHUP, &sa, NULL) < 0) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                   */
/* ------------------------------------------------------------------ */

struct options {
    const char *config_path;
    int dry_run;
    int foreground;
    int verbose;
};

static void print_usage(void)
{
    printf("Usage: %s [OPTIONS]\n"
           "\n"
           "Options:\n"
           "  --config <path>  Config file (default %s)\n"
           "  --dry-run        Log shutdown instead of executing it\n"
           "  --foreground     Run in foreground (default; for systemd)\n"
           "  --verbose        Force debug-level logging to stderr\n"
           "  --version        Print version and exit\n"
           "  --help           Print this message and exit\n",
           PROGRAM_NAME, DEFAULT_CONFIG);
}

/*
 * parse_args: parse command-line arguments into options struct.
 * Returns: 0 on success, -1 on error, 1 if program should exit (help/version).
 */
static int parse_args(int argc, char **argv, struct options *opts)
{
    int i;

    opts->config_path = DEFAULT_CONFIG;
    opts->dry_run = 0;
    opts->foreground = 1;
    opts->verbose = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--config requires an argument\n");
                return -1;
            }
            i++;
            opts->config_path = argv[i];
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            opts->dry_run = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            opts->foreground = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
            return 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage();
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Config reload (SIGHUP)                                             */
/* ------------------------------------------------------------------ */

/*
 * reload_config: re-read and apply the config file.
 * Inputs: config path, config struct, session (for listener rebuild).
 * Returns: 0 on success, -1 on failure (old config remains active).
 */
static int reload_config(const char *path, struct config *cfg,
                         struct session *sess)
{
    struct config new_cfg;

    log_msg(LOG_INFO, "reloading configuration from %s", path);

    config_set_defaults(&new_cfg);

    if (config_check_perms(path) < 0) {
        log_msg(LOG_ERR, "config reload failed: permission check");
        return -1;
    }
    if (config_load(path, &new_cfg) < 0) {
        log_msg(LOG_ERR, "config reload failed: parse error");
        return -1;
    }
    if (config_resolve_addresses(&new_cfg) < 0) {
        log_msg(LOG_ERR, "config reload failed: address resolution");
        return -1;
    }

    /* Update log level and redaction secret */
    log_set_level(new_cfg.log_level);
    log_set_redact(new_cfg.shared_password);

    /* Rebuild listener if listen address or port changed */
    if (strcmp(new_cfg.listen_address, cfg->listen_address) != 0 ||
        new_cfg.listen_port != cfg->listen_port) {
        log_msg(LOG_INFO, "listen address changed, rebuilding listener");
        session_destroy(sess);
        *cfg = new_cfg;
        if (session_setup(sess, cfg) < 0) {
            log_msg(LOG_ERR, "failed to rebind listener after reload");
            return -1;
        }
    } else {
        *cfg = new_cfg;
    }

    log_msg(LOG_INFO, "configuration reloaded successfully");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                    */
/* ------------------------------------------------------------------ */

/*
 * drain_signal_pipe: read and dispatch all pending signal bytes.
 * Inputs: options (for config path), config, session.
 * Returns: 1 if the loop should stop, 0 otherwise.
 */
static int drain_signal_pipe(const struct options *opts,
                             struct config *cfg,
                             struct session *sess)
{
    unsigned char signo;
    ssize_t n;

    for (;;) {
        n = read(g_signal_pipe[0], &signo, 1);
        if (n <= 0) break;

        switch (signo) {
        case SIGTERM:
            /* fall through */
        case SIGINT:
            log_msg(LOG_INFO, "received signal %d, shutting down",
                    (int)signo);
            session_set_state(sess, STATE_STOPPING);
            return 1;

        case SIGHUP:
            reload_config(opts->config_path, cfg, sess);
            break;

        default:
            log_msg(LOG_DEBUG, "unexpected signal %d", (int)signo);
            break;
        }
    }
    return 0;
}

/*
 * handle_incoming: accept and process one inbound DxP connection.
 * Inputs: session, config, options.
 * Handles shutdown detection and halt execution.
 */
static void handle_incoming(struct session *sess,
                            const struct config *cfg,
                            const struct options *opts)
{
    shutdown_event_t event;
    char peer_ip[16];
    int conn_fd;
    int rc;

    conn_fd = session_accept(sess, cfg, peer_ip);
    if (conn_fd < 0) return;

    memset(&event, 0, sizeof(event));
    rc = signal_source_handle_connection(conn_fd, cfg, &event);
    close(conn_fd);

    if (rc == 0) {
        /* Valid shutdown command received */
        if (halt_is_latched()) {
            log_msg(LOG_INFO, "shutdown already in progress, ignoring");
            return;
        }
        strncpy(event.source_ip, peer_ip, 15);
        event.source_ip[15] = '\0';
        log_msg(LOG_INFO,
                "initiating halt (source=%s command=%s(%u) desc=%u)",
                event.source_ip,
                dxp_command_name(event.command),
                (unsigned)event.command,
                (unsigned)event.desc);
        session_set_state(sess, STATE_SHUTTING_DOWN);
        halt_execute(cfg->halt_command, opts->dry_run);
    }
}

/*
 * run_loop: main poll loop.
 * Inputs: options, config, session.
 * Returns: exit code.
 */
static int run_loop(const struct options *opts,
                    struct config *cfg,
                    struct session *sess)
{
    struct pollfd fds[2];
    int timeout_ms;
    int nfds;
    int rc;
    int listen_fd;

    log_msg(LOG_INFO, "%s %s starting (dry_run=%d)",
            PROGRAM_NAME, PROGRAM_VERSION, opts->dry_run);
    log_msg(LOG_INFO, "listening on %s:%d for iBoot at %s:%d",
            cfg->listen_address, cfg->listen_port,
            cfg->iboot_address, cfg->iboot_port);

    while (session_get_state(sess) != STATE_STOPPING) {
        listen_fd = session_get_listen_fd(sess);

        /* If listener is down (CONNECTING state), try to rebuild it */
        if (listen_fd < 0) {
            if (session_setup(sess, cfg) == 0) {
                listen_fd = session_get_listen_fd(sess);
                log_msg(LOG_INFO, "listener re-established on %s:%d",
                        cfg->listen_address, cfg->listen_port);
            } else {
                /* Back off before retrying */
                log_msg(LOG_WARNING,
                        "listener setup failed, retrying in %d seconds",
                        sess->backoff_seconds);
                sleep((unsigned)sess->backoff_seconds);
                if (sess->backoff_seconds < SESSION_BACKOFF_MAX) {
                    sess->backoff_seconds *= 2;
                    if (sess->backoff_seconds > SESSION_BACKOFF_MAX) {
                        sess->backoff_seconds = SESSION_BACKOFF_MAX;
                    }
                }
                continue;
            }
        }

        /* Build poll set: signal pipe + listener */
        nfds = 0;

        fds[nfds].fd = g_signal_pipe[0];
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        if (listen_fd >= 0) {
            fds[nfds].fd = listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }

        /* Compute poll timeout from heartbeat schedule */
        timeout_ms = session_heartbeat_timeout_ms(sess,
                                                  cfg->heartbeat_interval);
        if (timeout_ms < 0) {
            timeout_ms = 30000;
        }

        rc = poll(fds, (nfds_t)nfds, timeout_ms);

        if (rc < 0) {
            if (errno == EINTR) continue;
            log_msg(LOG_ERR, "poll: %s", strerror(errno));
            break;
        }

        /* Check signal pipe */
        if (fds[0].revents & POLLIN) {
            if (drain_signal_pipe(opts, cfg, sess)) {
                break;
            }
        }

        /* Check listener */
        if (nfds > 1 && (fds[1].revents & POLLIN)) {
            handle_incoming(sess, cfg, opts);
        }

        /* Check heartbeat timer */
        if (session_heartbeat_due(sess, cfg->heartbeat_interval)) {
            session_send_heartbeat(sess, cfg);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct options opts;
    struct config cfg;
    struct session sess;
    int rc;

    rc = parse_args(argc, argv, &opts);
    if (rc != 0) return (rc > 0) ? 0 : 1;

    /* Load configuration */
    config_set_defaults(&cfg);
    if (config_check_perms(opts.config_path) < 0) return 1;
    if (config_load(opts.config_path, &cfg) < 0) return 1;
    if (config_resolve_addresses(&cfg) < 0) return 1;

    /* Initialize logging */
    log_init(PROGRAM_NAME, opts.foreground, opts.verbose, cfg.log_level);
    log_set_redact(cfg.shared_password);

    /* Set up self-pipe for signals */
    if (setup_signal_pipe() < 0) {
        log_msg(LOG_ERR, "failed to set up signal pipe: %s",
                strerror(errno));
        log_close();
        return 1;
    }

    /* Initialize and start session */
    session_init(&sess);
    if (session_setup(&sess, &cfg) < 0) {
        log_msg(LOG_ERR, "initial listener setup failed");
        session_set_state(&sess, STATE_CONNECTING);
    }

    /* Run the event loop */
    rc = run_loop(&opts, &cfg, &sess);

    /* Clean shutdown */
    session_destroy(&sess);
    if (g_signal_pipe[0] >= 0) close(g_signal_pipe[0]);
    if (g_signal_pipe[1] >= 0) close(g_signal_pipe[1]);
    log_msg(LOG_INFO, "%s exiting", PROGRAM_NAME);
    log_close();

    return rc;
}
