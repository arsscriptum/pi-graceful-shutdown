/*
 * session.h - Connection management and state machine
 *
 * Owns the listener socket (Mechanism B), the heartbeat client,
 * the reconnect-with-backoff logic, and the agent state machine.
 */

#ifndef SESSION_H
#define SESSION_H

#include <time.h>
#include <netinet/in.h>
#include <poll.h>

#include "config.h"

/* Agent states */
typedef enum {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_READY,
    STATE_SHUTTING_DOWN,
    STATE_STOPPING
} agent_state_t;

#define SESSION_BACKOFF_INITIAL  1
#define SESSION_BACKOFF_MAX      60

struct session {
    agent_state_t state;
    int           listen_fd;
    int           backoff_seconds;
    time_t        last_heartbeat;
    time_t        last_connect_attempt;
};

/*
 * session_init: initialize session struct.
 * Inputs: session pointer.
 */
void session_init(struct session *s);

/*
 * session_setup: create the listener socket and transition to READY.
 * Inputs: session, configuration.
 * Returns: 0 on success, -1 on failure.
 *          On failure, the state moves to CONNECTING for retry.
 */
int session_setup(struct session *s, const struct config *cfg);

/*
 * session_destroy: close all sockets.
 * Inputs: session pointer.
 */
void session_destroy(struct session *s);

/*
 * session_get_listen_fd: return the listener fd for polling.
 * Returns: fd, or -1 if not listening.
 */
int session_get_listen_fd(const struct session *s);

/*
 * session_accept: accept an inbound connection, validate source IP.
 * Inputs: session, configuration.
 * Outputs: fills peer_ip string if provided (at least 16 bytes).
 * Returns: accepted socket fd, or -1 on error or rejected source.
 *          Caller must close the returned fd.
 */
int session_accept(const struct session *s, const struct config *cfg,
                   char *peer_ip);

/*
 * session_heartbeat_due: check whether a heartbeat should be sent.
 * Inputs: session, heartbeat interval from config.
 * Returns: 1 if due, 0 otherwise.  Always 0 if interval <= 0.
 */
int session_heartbeat_due(const struct session *s, int interval);

/*
 * session_heartbeat_timeout_ms: milliseconds until the next heartbeat.
 * Inputs: session, heartbeat interval from config.
 * Returns: timeout in ms for poll(), or -1 if heartbeat is disabled.
 */
int session_heartbeat_timeout_ms(const struct session *s, int interval);

/*
 * session_send_heartbeat: connect to the iBoot and send a keepAlive.
 * Inputs: session (heartbeat timer updated), configuration.
 * Returns: 0 on success, -1 on failure (logged).
 */
int session_send_heartbeat(struct session *s, const struct config *cfg);

/*
 * session_get_state / session_set_state: access the agent state.
 */
agent_state_t session_get_state(const struct session *s);
void session_set_state(struct session *s, agent_state_t state);

/*
 * session_state_name: return a human-readable state name.
 */
const char *session_state_name(agent_state_t state);

#endif /* SESSION_H */
