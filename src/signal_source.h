/*
 * signal_source.h - Shutdown signal detection abstraction
 *
 * Decides whether a DxP exchange constitutes a shutdown command.
 * Two candidate mechanisms:
 *
 *   Mechanism A (heartbeat reply flag): the agent sends periodic
 *   heartbeats; the iBoot's reply carries a shutdown-pending flag.
 *
 *   Mechanism B (inbound command): the iBoot connects to the agent
 *   and pushes a shutdown command.
 *
 * Only one is active once the packet capture resolves which the
 * firmware uses.  Both share the shutdown_event_t output type.
 */

#ifndef SIGNAL_SOURCE_H
#define SIGNAL_SOURCE_H

#include "config.h"

/* Result of a successful shutdown detection */
typedef struct {
    unsigned char command;
    unsigned char desc;
    char source_ip[16];
} shutdown_event_t;

/*
 * Mechanism B: handle a DxP exchange on an accepted connection.
 *
 * Performs the full server-side DxP protocol: receive hello, send
 * sequence, receive header, validate sequence and credentials,
 * detect shutdown command, and send response.
 *
 * Inputs: accepted socket fd (caller closes it after return),
 *         configuration with shared credentials.
 * Outputs: if a shutdown command is detected, fills event and returns 0.
 * Returns: 0 if a valid shutdown command was received,
 *          1 if a valid non-shutdown command was received,
 *         -1 on protocol error, auth failure, or I/O error.
 */
int signal_source_handle_connection(int conn_fd,
                                    const struct config *cfg,
                                    shutdown_event_t *event);

/*
 * Mechanism A: check a heartbeat response for a shutdown flag.
 * Stub -- not yet implemented pending packet capture analysis.
 *
 * Inputs: response buffer, response length.
 * Outputs: if a shutdown flag is detected, fills event and returns 0.
 * Returns: 0 if shutdown detected, -1 otherwise.
 */
int signal_source_check_reply(const unsigned char *response, int len,
                              shutdown_event_t *event);

#endif /* SIGNAL_SOURCE_H */
