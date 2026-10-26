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
 * dxp_is_shutdown_command: determine if a parsed header represents
 * a shutdown signal.
 * Accepts eCmnd_pcControl (7) with any descriptor, and
 * eCmnd_io (3) with eIO_changeRelay (1).
 * Returns: 1 if shutdown, 0 otherwise.
 */
int dxp_is_shutdown_command(const dxp_header_t *hdr);

/*
 * dxp_command_name: return a human-readable name for a command class.
 * Inputs: command byte.
 * Returns: static string (never NULL).
 */
const char *dxp_command_name(unsigned char cmd);

#endif /* DXP_H */
