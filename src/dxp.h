/*
 * dxp.h - Dataprobe Exchange Protocol (DxP) v1.2+ definitions
 *
 * Pure protocol layer: wire format constants, parse and build functions
 * over buffers.  No I/O, no state.  Unit-tested against reference
 * traffic captures.
 *
 * Wire format derived from the DxP Protocol Documentation v1.2 and
 * the iBoot Control Utility C source in /references.  Credential
 * fields are plaintext, 20 characters max, null-terminated in 21-byte
 * wire fields.
 */

#ifndef DXP_H
#define DXP_H

/* Hello handshake */
#define DXP_HELLO          "hello-000"
#define DXP_HELLO_LEN      10
#define DXP_SEQ_LEN        2

/* Default TCP port */
#define DXP_DEFAULT_PORT   9100

/* Credential field sizes */
#define DXP_USERNAME_MAX   20
#define DXP_PASSWORD_MAX   20
#define DXP_USERNAME_FIELD 21
#define DXP_PASSWORD_FIELD 21

/*
 * Header wire layout (47 bytes, no padding):
 *   offset  0: command       (1 byte, unsigned)
 *   offset  1: username      (21 bytes, null-terminated)
 *   offset 22: password      (21 bytes, null-terminated)
 *   offset 43: desc          (1 byte, command descriptor)
 *   offset 44: param         (1 byte, extra parameter)
 *   offset 45: seq           (2 bytes, little-endian uint16)
 */
#define DXP_HDR_OFF_COMMAND   0
#define DXP_HDR_OFF_USERNAME  1
#define DXP_HDR_OFF_PASSWORD  22
#define DXP_HDR_OFF_DESC      43
#define DXP_HDR_OFF_PARAM     44
#define DXP_HDR_OFF_SEQ       45
#define DXP_HDR_SIZE          47

/* Command classes (eCmnd enum values) */
#define DXP_CMND_NULL        0
#define DXP_CMND_SET         1
#define DXP_CMND_GET         2
#define DXP_CMND_IO          3
#define DXP_CMND_KEEPALIVE   4
#define DXP_CMND_RSS         5
#define DXP_CMND_RCU         6
#define DXP_CMND_PCCONTROL   7

/* I/O command descriptors */
#define DXP_IO_NULL           0
#define DXP_IO_CHANGE_RELAY   1
#define DXP_IO_CHANGE_RELAYS  2
#define DXP_IO_GET_RELAY      3
#define DXP_IO_GET_RELAYS     4
#define DXP_IO_GET_INPUT      5
#define DXP_IO_GET_INPUTS     6
#define DXP_IO_PULSE_RELAY    7

/* Relay states */
#define DXP_RELAY_NO_CHANGE   0
#define DXP_RELAY_ENERGIZE    1
#define DXP_RELAY_RELAX       2

/*
 * ChangeRelay payload (follows the 47-byte header):
 *   offset 0: relay index (1 byte)
 *   offset 1: new state   (1 byte, one of the DXP_RELAY_* values)
 * Total changeRelay frame is 47 + 2 = 49 bytes.
 */
#define DXP_CHANGERELAY_PAYLOAD_LEN 2
#define DXP_CR_OFF_RELAY            0
#define DXP_CR_OFF_STATE            1

/* dxp_classify_shutdown results */
#define DXP_SHUTDOWN_NO        0
#define DXP_SHUTDOWN_YES       1
#define DXP_SHUTDOWN_NO_STATE  (-1)

/* KeepAlive descriptor */
#define DXP_KEEPALIVE_NULL    0

/* Response codes */
#define DXP_RESP_OK           0x00
#define DXP_RESP_ERROR        0x01

/* Maximum payload after header */
#define DXP_MAX_PAYLOAD       64

/* Parsed DxP header (host-side representation) */
typedef struct {
    unsigned char command;
    char          username[DXP_USERNAME_FIELD];
    char          password[DXP_PASSWORD_FIELD];
    unsigned char desc;
    unsigned char param;
    unsigned short seq;
} dxp_header_t;

/*
 * dxp_read_seq: read a little-endian 16-bit sequence number.
 * Inputs: 2-byte buffer.
 * Returns: the sequence number.
 */
unsigned short dxp_read_seq(const unsigned char *buf);

/*
 * dxp_write_seq: write a little-endian 16-bit sequence number.
 * Inputs: 2-byte buffer, sequence value.
 */
void dxp_write_seq(unsigned char *buf, unsigned short seq);

/*
 * dxp_parse_header: parse 47 wire bytes into a dxp_header_t.
 * Inputs: 47-byte wire buffer, output header struct.
 * Ensures username and password are null-terminated regardless
 * of wire content.
 */
void dxp_parse_header(const unsigned char *wire, dxp_header_t *hdr);

/*
 * dxp_build_header: serialize a dxp_header_t into 47 wire bytes.
 * Inputs: 47-byte wire buffer, header struct.
 * Zeroes the buffer before writing.
 */
void dxp_build_header(unsigned char *wire, const dxp_header_t *hdr);

/*
 * dxp_validate_credentials: compare parsed header credentials
 * against expected values.
 * Inputs: parsed header, expected username, expected password.
 * Returns: 1 if both match, 0 otherwise.
 */
int dxp_validate_credentials(const dxp_header_t *hdr,
                             const char *username,
                             const char *password);

/*
 * dxp_classify_shutdown: decide whether a parsed frame is a
 * shutdown signal.
 *
 * Only two things halt the host:
 *   - eCmnd_pcControl (7): the graceful-shutdown command, which the
 *     reference capture shows arriving header-only.
 *   - eCmnd_io (3) with eIO_changeRelay (1) carrying state RELAX,
 *     i.e. the outlet relay de-energizing, so power is about to drop.
 *
 * The relay state is not in the header.  It lives in the 2-byte
 * changeRelay payload [relay index, state] that follows the header,
 * so that payload must be supplied for a changeRelay verdict.  A
 * changeRelay carrying ENERGIZE (the outlet coming on) or NO_CHANGE
 * is not a shutdown and must never halt the host.
 *
 * Inputs: parsed header; the payload bytes following the header (may
 *         be NULL when payload_len is 0); payload length in bytes.
 * Returns: DXP_SHUTDOWN_YES if the frame is a shutdown signal,
 *          DXP_SHUTDOWN_NO if it is not, or DXP_SHUTDOWN_NO_STATE
 *          when a changeRelay arrived without a readable state
 *          payload.  NO_STATE must be treated as not-a-shutdown by
 *          the caller, but is distinct so it can be logged.
 */
int dxp_classify_shutdown(const dxp_header_t *hdr,
                          const unsigned char *payload,
                          int payload_len);

/*
 * dxp_command_name: return a human-readable name for a command class.
 * Inputs: command byte.
 * Returns: static string (never NULL).
 */
const char *dxp_command_name(unsigned char cmd);

/*
 * dxp_parse_json_envelope: extract a DxP header from a JSON-wrapped frame.
 *
 * iBoot firmware v1.40.60+ wraps the 47-byte DxP binary header inside
 * a JSON-like envelope.  The wire layout is:
 *   [0x00 prefix] {"hdr":{"uuid": "XXXX [47-byte DxP header]
 * where XXXX is a 4-byte device marker.  The DxP header starts 21
 * bytes after the opening brace.
 *
 * Inputs: raw frame buffer (as received after hello/seq handshake),
 *         buffer length, output header struct, and optional output
 *         offset of the embedded header within the frame (may be
 *         NULL).  The caller needs that offset to locate any command
 *         payload following the header.
 * Returns: 0 on success (header and hdr_off populated), -1 if the
 *          frame does not contain a recognizable JSON envelope.
 */
int dxp_parse_json_envelope(const unsigned char *buf, int len,
                            dxp_header_t *hdr, int *hdr_off);

#endif /* DXP_H */
