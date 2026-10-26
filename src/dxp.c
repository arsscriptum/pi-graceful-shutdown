/*
 * dxp.c - DxP protocol framing (pure functions, no I/O)
 */

#include <string.h>

#include "dxp.h"

unsigned short dxp_read_seq(const unsigned char *buf)
{
    return (unsigned short)((unsigned short)buf[0] |
                            ((unsigned short)buf[1] << 8));
}

void dxp_write_seq(unsigned char *buf, unsigned short seq)
{
    buf[0] = (unsigned char)(seq & 0xFF);
    buf[1] = (unsigned char)((seq >> 8) & 0xFF);
}

void dxp_parse_header(const unsigned char *wire, dxp_header_t *hdr)
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

void dxp_build_header(unsigned char *wire, const dxp_header_t *hdr)
{
    memset(wire, 0, DXP_HDR_SIZE);
    wire[DXP_HDR_OFF_COMMAND] = hdr->command;
    memcpy(&wire[DXP_HDR_OFF_USERNAME], hdr->username, DXP_USERNAME_FIELD);
    memcpy(&wire[DXP_HDR_OFF_PASSWORD], hdr->password, DXP_PASSWORD_FIELD);
    wire[DXP_HDR_OFF_DESC] = hdr->desc;
    wire[DXP_HDR_OFF_PARAM] = hdr->param;
    dxp_write_seq(&wire[DXP_HDR_OFF_SEQ], hdr->seq);
}

int dxp_validate_credentials(const dxp_header_t *hdr,
                             const char *username,
                             const char *password)
{
    if (strcmp(hdr->username, username) != 0) return 0;
    if (strcmp(hdr->password, password) != 0) return 0;
    return 1;
}

int dxp_is_shutdown_command(const dxp_header_t *hdr)
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

const char *dxp_command_name(unsigned char cmd)
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
 * JSON envelope constants.  The tag is checked after the opening
 * brace; the offset is from '{' to the first byte of the embedded
 * 47-byte DxP header.
 */
#define DXP_JSON_TAG         "\"hdr\":"
#define DXP_JSON_TAG_LEN     6
#define DXP_JSON_HDR_OFFSET  21

int dxp_parse_json_envelope(const unsigned char *buf, int len,
                            dxp_header_t *hdr)
{
    int brace;
    int i;

    brace = -1;
    for (i = 0; i < len; i++) {
        if (buf[i] == '{') {
            brace = i;
            break;
        }
    }
    if (brace < 0) return -1;

    if (len - brace < DXP_JSON_HDR_OFFSET + DXP_HDR_SIZE) return -1;

    if (memcmp(&buf[brace + 1], DXP_JSON_TAG, DXP_JSON_TAG_LEN) != 0)
        return -1;

    dxp_parse_header(&buf[brace + DXP_JSON_HDR_OFFSET], hdr);
    return 0;
}
