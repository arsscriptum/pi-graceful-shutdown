/*
 * dxp.h - Dataprobe Exchange Protocol (DxP) v1.2+ definitions
 *
 * Wire format constants and types derived from the DxP Protocol
 * Documentation v1.2 and the iBoot Control Utility C source.
 * Credential fields are plaintext, 20 characters max, null-terminated
 * in 21-byte wire fields.  The protocol has no transport security;
 * the deployment assumes a trusted LAN segment.
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
 *   offset  1: username      (21 bytes, null-terminated string)
 *   offset 22: password      (21 bytes, null-terminated string)
 *   offset 43: desc          (1 byte, command descriptor)
 *   offset 44: param         (1 byte, extra parameter)
 *   offset 45: seq           (2 bytes, little-endian uint16)
 *
 * The struct in the reference C code is 47 bytes on the original
 * compiler (Watcom).  This code parses the wire format byte-by-byte
 * to avoid struct-packing portability issues.
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
#define DXP_CMND_PCCONTROL   7   /* PC Control -- graceful shutdown */

/* I/O command descriptors (eCmndIoDesc enum values) */
#define DXP_IO_NULL           0
#define DXP_IO_CHANGE_RELAY   1
#define DXP_IO_CHANGE_RELAYS  2
#define DXP_IO_GET_RELAY      3
#define DXP_IO_GET_RELAYS     4
#define DXP_IO_GET_INPUT      5
#define DXP_IO_GET_INPUTS     6
#define DXP_IO_PULSE_RELAY    7

/* Relay states for ChangeRelay payload */
#define DXP_RELAY_NO_CHANGE   0
#define DXP_RELAY_ENERGIZE    1
#define DXP_RELAY_RELAX       2

/* KeepAlive descriptor */
#define DXP_KEEPALIVE_NULL    0

/* Response codes (single byte) */
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

#endif /* DXP_H */
