/*
 * dxp_decode_test.c - Unit tests for the DxP protocol decoder
 *
 * Tests dxp_parse_header, dxp_build_header, dxp_read_seq,
 * dxp_write_seq, dxp_validate_credentials, dxp_classify_shutdown,
 * and dxp_command_name against known-good wire data.
 *
 * Build: cc -ansi -pedantic -Wall -Wextra -I../src -o dxp_decode_test \
 *           dxp_decode_test.c ../src/dxp.c
 * Run:   ./dxp_decode_test
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dxp.h"

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ_INT(msg, expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            g_pass++; \
        } else { \
            g_fail++; \
            printf("FAIL: %s: expected %d, got %d\n", \
                   (msg), (int)(expected), (int)(actual)); \
        } \
    } while (0)

#define ASSERT_EQ_UINT(msg, expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            g_pass++; \
        } else { \
            g_fail++; \
            printf("FAIL: %s: expected %u, got %u\n", \
                   (msg), (unsigned)(expected), (unsigned)(actual)); \
        } \
    } while (0)

#define ASSERT_EQ_STR(msg, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) == 0) { \
            g_pass++; \
        } else { \
            g_fail++; \
            printf("FAIL: %s: expected '%s', got '%s'\n", \
                   (msg), (expected), (actual)); \
        } \
    } while (0)

#define ASSERT_TRUE(msg, cond) \
    do { \
        if ((cond)) { \
            g_pass++; \
        } else { \
            g_fail++; \
            printf("FAIL: %s\n", (msg)); \
        } \
    } while (0)

/* ------------------------------------------------------------------ */
/* Helper: build a 47-byte wire frame with given fields                */
/* ------------------------------------------------------------------ */

static void build_wire(unsigned char *wire,
                       unsigned char cmd,
                       const char *user,
                       const char *pass,
                       unsigned char desc,
                       unsigned char param,
                       unsigned short seq)
{
    memset(wire, 0, DXP_HDR_SIZE);
    wire[DXP_HDR_OFF_COMMAND] = cmd;
    if (user != NULL) {
        strncpy((char *)&wire[DXP_HDR_OFF_USERNAME], user,
                DXP_USERNAME_FIELD);
    }
    if (pass != NULL) {
        strncpy((char *)&wire[DXP_HDR_OFF_PASSWORD], pass,
                DXP_PASSWORD_FIELD);
    }
    wire[DXP_HDR_OFF_DESC] = desc;
    wire[DXP_HDR_OFF_PARAM] = param;
    wire[DXP_HDR_OFF_SEQ] = (unsigned char)(seq & 0xFF);
    wire[DXP_HDR_OFF_SEQ + 1] = (unsigned char)((seq >> 8) & 0xFF);
}

/* ------------------------------------------------------------------ */
/* Test groups                                                        */
/* ------------------------------------------------------------------ */

static void test_seq_read_write(void)
{
    unsigned char buf[2];
    unsigned short seq;

    printf("--- dxp_read_seq / dxp_write_seq ---\n");

    /* Zero */
    buf[0] = 0x00; buf[1] = 0x00;
    seq = dxp_read_seq(buf);
    ASSERT_EQ_UINT("seq zero", 0, seq);

    /* 0x1234 little-endian: low byte first */
    buf[0] = 0x34; buf[1] = 0x12;
    seq = dxp_read_seq(buf);
    ASSERT_EQ_UINT("seq 0x1234", 0x1234, seq);

    /* 0xFFFF */
    buf[0] = 0xFF; buf[1] = 0xFF;
    seq = dxp_read_seq(buf);
    ASSERT_EQ_UINT("seq 0xFFFF", 0xFFFF, seq);

    /* Round-trip */
    dxp_write_seq(buf, 0xABCD);
    seq = dxp_read_seq(buf);
    ASSERT_EQ_UINT("seq round-trip 0xABCD", 0xABCD, seq);

    /* Verify byte order */
    dxp_write_seq(buf, 0x0102);
    ASSERT_EQ_UINT("seq write low byte", 0x02, buf[0]);
    ASSERT_EQ_UINT("seq write high byte", 0x01, buf[1]);
}

static void test_parse_header(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;

    printf("--- dxp_parse_header ---\n");

    /* Standard pcControl frame */
    build_wire(wire, DXP_CMND_PCCONTROL, "admin", "pass123",
               0x01, 0x00, 42);
    dxp_parse_header(wire, &hdr);

    ASSERT_EQ_UINT("parse cmd", DXP_CMND_PCCONTROL, hdr.command);
    ASSERT_EQ_STR("parse user", "admin", hdr.username);
    ASSERT_EQ_STR("parse pass", "pass123", hdr.password);
    ASSERT_EQ_UINT("parse desc", 0x01, hdr.desc);
    ASSERT_EQ_UINT("parse param", 0x00, hdr.param);
    ASSERT_EQ_UINT("parse seq", 42, hdr.seq);

    /* Max-length username (20 chars) */
    build_wire(wire, DXP_CMND_IO, "12345678901234567890", "pw",
               DXP_IO_CHANGE_RELAY, 1, 1000);
    dxp_parse_header(wire, &hdr);
    ASSERT_EQ_STR("parse max user", "12345678901234567890", hdr.username);
    ASSERT_EQ_UINT("parse max user len", 20,
                   (unsigned)strlen(hdr.username));

    /* All zeros frame */
    memset(wire, 0, DXP_HDR_SIZE);
    dxp_parse_header(wire, &hdr);
    ASSERT_EQ_UINT("parse zero cmd", 0, hdr.command);
    ASSERT_EQ_STR("parse zero user", "", hdr.username);
    ASSERT_EQ_STR("parse zero pass", "", hdr.password);
    ASSERT_EQ_UINT("parse zero seq", 0, hdr.seq);
}

static void test_build_header(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;
    dxp_header_t parsed;

    printf("--- dxp_build_header ---\n");

    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_KEEPALIVE;
    strncpy(hdr.username, "testuser", DXP_USERNAME_FIELD);
    strncpy(hdr.password, "testpass", DXP_PASSWORD_FIELD);
    hdr.desc = DXP_KEEPALIVE_NULL;
    hdr.param = 0;
    hdr.seq = 9999;

    dxp_build_header(wire, &hdr);

    /* Verify wire bytes directly */
    ASSERT_EQ_UINT("build cmd byte", DXP_CMND_KEEPALIVE,
                   wire[DXP_HDR_OFF_COMMAND]);
    ASSERT_EQ_UINT("build seq low", 9999 & 0xFF,
                   wire[DXP_HDR_OFF_SEQ]);
    ASSERT_EQ_UINT("build seq high", (9999 >> 8) & 0xFF,
                   wire[DXP_HDR_OFF_SEQ + 1]);

    /* Round-trip: build then parse */
    dxp_parse_header(wire, &parsed);
    ASSERT_EQ_UINT("roundtrip cmd", hdr.command, parsed.command);
    ASSERT_EQ_STR("roundtrip user", hdr.username, parsed.username);
    ASSERT_EQ_STR("roundtrip pass", hdr.password, parsed.password);
    ASSERT_EQ_UINT("roundtrip desc", hdr.desc, parsed.desc);
    ASSERT_EQ_UINT("roundtrip param", hdr.param, parsed.param);
    ASSERT_EQ_UINT("roundtrip seq", hdr.seq, parsed.seq);
}

static void test_validate_credentials(void)
{
    dxp_header_t hdr;

    printf("--- dxp_validate_credentials ---\n");

    memset(&hdr, 0, sizeof(hdr));
    strncpy(hdr.username, "admin", DXP_USERNAME_FIELD);
    strncpy(hdr.password, "secret", DXP_PASSWORD_FIELD);

    ASSERT_EQ_INT("cred match", 1,
                  dxp_validate_credentials(&hdr, "admin", "secret"));
    ASSERT_EQ_INT("cred bad user", 0,
                  dxp_validate_credentials(&hdr, "root", "secret"));
    ASSERT_EQ_INT("cred bad pass", 0,
                  dxp_validate_credentials(&hdr, "admin", "wrong"));
    ASSERT_EQ_INT("cred both bad", 0,
                  dxp_validate_credentials(&hdr, "root", "wrong"));
    ASSERT_EQ_INT("cred empty vs non-empty", 0,
                  dxp_validate_credentials(&hdr, "", ""));

    /* Empty credentials on both sides */
    memset(&hdr, 0, sizeof(hdr));
    ASSERT_EQ_INT("cred both empty", 1,
                  dxp_validate_credentials(&hdr, "", ""));

    /* Case sensitivity */
    strncpy(hdr.username, "Admin", DXP_USERNAME_FIELD);
    strncpy(hdr.password, "Secret", DXP_PASSWORD_FIELD);
    ASSERT_EQ_INT("cred case sensitive user", 0,
                  dxp_validate_credentials(&hdr, "admin", "Secret"));
    ASSERT_EQ_INT("cred case sensitive pass", 0,
                  dxp_validate_credentials(&hdr, "Admin", "secret"));
    ASSERT_EQ_INT("cred case exact", 1,
                  dxp_validate_credentials(&hdr, "Admin", "Secret"));
}

static void test_classify_shutdown(void)
{
    unsigned char cr[DXP_CHANGERELAY_PAYLOAD_LEN];
    dxp_header_t hdr;

    printf("--- dxp_classify_shutdown ---\n");

    memset(&hdr, 0, sizeof(hdr));

    /* pcControl (command 7) is always a shutdown, payload or not */
    hdr.command = DXP_CMND_PCCONTROL;
    hdr.desc = 0;
    ASSERT_EQ_INT("pcControl desc=0", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, NULL, 0));

    hdr.desc = 1;
    ASSERT_EQ_INT("pcControl desc=1", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, NULL, 0));

    hdr.desc = 2;
    ASSERT_EQ_INT("pcControl desc=2 (capture)", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, NULL, 0));

    hdr.desc = 0xFF;
    ASSERT_EQ_INT("pcControl desc=0xFF", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, NULL, 0));

    /* io/changeRelay with state RELAX: power dropping, do halt */
    hdr.command = DXP_CMND_IO;
    hdr.desc = DXP_IO_CHANGE_RELAY;
    cr[DXP_CR_OFF_RELAY] = 0;
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_RELAX;
    ASSERT_EQ_INT("io/changeRelay RELAX", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /*
     * io/changeRelay with state ENERGIZE: the outlet coming ON.
     * This is the regression.  The old header-only classifier halted
     * here, so an outlet-on command reset the Pi from a distance.
     */
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_ENERGIZE;
    ASSERT_EQ_INT("io/changeRelay ENERGIZE not shutdown",
                  DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /* io/changeRelay with state NO_CHANGE is not a shutdown either */
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_NO_CHANGE;
    ASSERT_EQ_INT("io/changeRelay NO_CHANGE not shutdown",
                  DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /* Unknown relay state is not a shutdown */
    cr[DXP_CR_OFF_STATE] = 0x7F;
    ASSERT_EQ_INT("io/changeRelay unknown state not shutdown",
                  DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /* Relay index must not affect the verdict, only the state does */
    cr[DXP_CR_OFF_RELAY] = 3;
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_RELAX;
    ASSERT_EQ_INT("io/changeRelay relay=3 RELAX", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /* changeRelay with a truncated or absent payload: no verdict */
    ASSERT_EQ_INT("io/changeRelay no payload", DXP_SHUTDOWN_NO_STATE,
                  dxp_classify_shutdown(&hdr, NULL, 0));
    ASSERT_EQ_INT("io/changeRelay 1-byte payload",
                  DXP_SHUTDOWN_NO_STATE,
                  dxp_classify_shutdown(&hdr, cr, 1));

    /* A payload longer than 2 bytes still reads the state at offset 1 */
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_ENERGIZE;
    ASSERT_EQ_INT("io/changeRelay long payload ENERGIZE",
                  DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr, 8));

    /* io with other descriptors is not a shutdown */
    hdr.desc = DXP_IO_GET_RELAY;
    ASSERT_EQ_INT("io/getRelay not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    hdr.desc = DXP_IO_NULL;
    ASSERT_EQ_INT("io/null not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    /* Other commands are not shutdowns, even carrying a RELAX byte */
    cr[DXP_CR_OFF_STATE] = DXP_RELAY_RELAX;
    hdr.command = DXP_CMND_KEEPALIVE;
    hdr.desc = 0;
    ASSERT_EQ_INT("keepAlive not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    hdr.command = DXP_CMND_GET;
    ASSERT_EQ_INT("get not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    hdr.command = DXP_CMND_SET;
    ASSERT_EQ_INT("set not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));

    hdr.command = DXP_CMND_NULL;
    ASSERT_EQ_INT("null not shutdown", DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr, cr,
                                        DXP_CHANGERELAY_PAYLOAD_LEN));
}

static void test_command_name(void)
{
    printf("--- dxp_command_name ---\n");

    ASSERT_EQ_STR("name null", "null",
                  dxp_command_name(DXP_CMND_NULL));
    ASSERT_EQ_STR("name set", "set",
                  dxp_command_name(DXP_CMND_SET));
    ASSERT_EQ_STR("name get", "get",
                  dxp_command_name(DXP_CMND_GET));
    ASSERT_EQ_STR("name io", "io",
                  dxp_command_name(DXP_CMND_IO));
    ASSERT_EQ_STR("name keepAlive", "keepAlive",
                  dxp_command_name(DXP_CMND_KEEPALIVE));
    ASSERT_EQ_STR("name rss", "rss",
                  dxp_command_name(DXP_CMND_RSS));
    ASSERT_EQ_STR("name rcu", "rcu",
                  dxp_command_name(DXP_CMND_RCU));
    ASSERT_EQ_STR("name pcControl", "pcControl",
                  dxp_command_name(DXP_CMND_PCCONTROL));
    ASSERT_EQ_STR("name unknown", "unknown",
                  dxp_command_name(0xFF));
}

static void test_wire_size_47(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;

    printf("--- wire size exactly 47 ---\n");

    ASSERT_EQ_INT("DXP_HDR_SIZE is 47", 47, DXP_HDR_SIZE);

    /* Verify field offsets do not overlap */
    ASSERT_EQ_INT("username follows command",
                  1, DXP_HDR_OFF_USERNAME);
    ASSERT_EQ_INT("password follows username",
                  22, DXP_HDR_OFF_PASSWORD);
    ASSERT_EQ_INT("desc follows password",
                  43, DXP_HDR_OFF_DESC);
    ASSERT_EQ_INT("param follows desc",
                  44, DXP_HDR_OFF_PARAM);
    ASSERT_EQ_INT("seq follows param",
                  45, DXP_HDR_OFF_SEQ);
    ASSERT_EQ_INT("seq end is HDR_SIZE",
                  47, DXP_HDR_OFF_SEQ + DXP_SEQ_LEN);

    /* Build a frame and verify all 47 bytes are touched/settable */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = 0xAA;
    memset(hdr.username, 'U', DXP_USERNAME_MAX);
    hdr.username[DXP_USERNAME_MAX] = '\0';
    memset(hdr.password, 'P', DXP_PASSWORD_MAX);
    hdr.password[DXP_PASSWORD_MAX] = '\0';
    hdr.desc = 0xBB;
    hdr.param = 0xCC;
    hdr.seq = 0xDDEE;

    dxp_build_header(wire, &hdr);

    ASSERT_EQ_UINT("wire[0] cmd", 0xAA, wire[0]);
    ASSERT_EQ_UINT("wire[1] first U", 'U', wire[1]);
    ASSERT_EQ_UINT("wire[20] last U", 'U', wire[20]);
    ASSERT_EQ_UINT("wire[21] null term user", 0, wire[21]);
    ASSERT_EQ_UINT("wire[22] first P", 'P', wire[22]);
    ASSERT_EQ_UINT("wire[41] last P", 'P', wire[41]);
    ASSERT_EQ_UINT("wire[42] null term pass", 0, wire[42]);
    ASSERT_EQ_UINT("wire[43] desc", 0xBB, wire[43]);
    ASSERT_EQ_UINT("wire[44] param", 0xCC, wire[44]);
    ASSERT_EQ_UINT("wire[45] seq low", 0xEE, wire[45]);
    ASSERT_EQ_UINT("wire[46] seq high", 0xDD, wire[46]);
}

/* ------------------------------------------------------------------ */
/* Reference capture: 69-byte JSON-envelope frame from iBoot v1.40.60 */
/* ------------------------------------------------------------------ */

static const unsigned char capture_frame[69] = {
    /* 0x00 prefix */
    0x00,
    /* JSON envelope: {"hdr":{"uuid": "<?d  */
    0x7b, 0x22, 0x68, 0x64, 0x72, 0x22, 0x3a, 0x7b,
    0x22, 0x75, 0x75, 0x69, 0x64, 0x22, 0x3a, 0x20,
    0x22, 0x3c, 0x3f, 0x64, 0x20,
    /* DxP header: command=pcControl(7) */
    0x07,
    /* username: "admin" + null padding (21 bytes) */
    0x61, 0x64, 0x6d, 0x69, 0x6e, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    /* password: test credential + null padding (21 bytes) */
    0x53, 0x65, 0x63, 0x72, 0x65, 0x74, 0x54, 0x65,
    0x73, 0x74, 0x32, 0x33, 0x5f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    /* desc=2, param=0, seq=0x3A3B (LE) */
    0x02, 0x00, 0x3b, 0x3a
};

static void test_json_envelope_capture(void)
{
    dxp_header_t hdr;
    int rc;
    int hdr_off;

    printf("--- dxp_parse_json_envelope (reference capture) ---\n");

    hdr_off = -1;
    rc = dxp_parse_json_envelope(capture_frame, 69, &hdr, &hdr_off);
    ASSERT_EQ_INT("envelope parse ok", 0, rc);
    ASSERT_EQ_INT("envelope header offset", 22, hdr_off);
    /* The capture is header-only: 22 prefix + 47 header = 69 bytes */
    ASSERT_EQ_INT("envelope payload empty", 0,
                  69 - hdr_off - DXP_HDR_SIZE);
    ASSERT_EQ_UINT("envelope cmd", DXP_CMND_PCCONTROL, hdr.command);
    ASSERT_EQ_STR("envelope user", "admin", hdr.username);
    ASSERT_EQ_STR("envelope pass", "SecretTest23_", hdr.password);
    ASSERT_EQ_UINT("envelope desc", 2, hdr.desc);
    ASSERT_EQ_UINT("envelope param", 0, hdr.param);
    ASSERT_EQ_UINT("envelope seq", 0x3A3B, hdr.seq);

    ASSERT_EQ_INT("envelope is shutdown", DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr, capture_frame + 69, 0));
    ASSERT_EQ_INT("envelope cred match", 1,
                  dxp_validate_credentials(&hdr, "admin",
                                           "SecretTest23_"));
    ASSERT_EQ_INT("envelope cred reject", 0,
                  dxp_validate_credentials(&hdr, "admin", "wrong"));
}

static void test_json_envelope_edge_cases(void)
{
    unsigned char short_buf[40];
    unsigned char no_json[47];
    unsigned char bad_tag[69];
    dxp_header_t hdr;
    int hdr_off;

    printf("--- dxp_parse_json_envelope (edge cases) ---\n");

    /* Truncated frame: too short for envelope + DxP header */
    memcpy(short_buf, capture_frame, 40);
    ASSERT_EQ_INT("truncated rejected", -1,
                  dxp_parse_json_envelope(short_buf, 40, &hdr,
                                          &hdr_off));

    /* No JSON brace at all */
    memset(no_json, 0x41, sizeof(no_json));
    ASSERT_EQ_INT("no brace rejected", -1,
                  dxp_parse_json_envelope(no_json, 47, &hdr,
                                          &hdr_off));

    /* Wrong envelope tag (correct brace but bad field name) */
    memcpy(bad_tag, capture_frame, 69);
    bad_tag[2] = 'x';
    ASSERT_EQ_INT("bad tag rejected", -1,
                  dxp_parse_json_envelope(bad_tag, 69, &hdr,
                                          &hdr_off));

    /* Empty buffer */
    ASSERT_EQ_INT("empty rejected", -1,
                  dxp_parse_json_envelope(no_json, 0, &hdr,
                                          &hdr_off));

    /* A NULL offset argument is allowed */
    ASSERT_EQ_INT("null offset arg accepted", 0,
                  dxp_parse_json_envelope(capture_frame, 69, &hdr,
                                          NULL));
}

/*
 * test_envelope_changerelay: a synthetic JSON-envelope changeRelay
 * frame, built on the real capture prefix, with the 2-byte relay
 * payload appended.  Exercises the full decode path the daemon uses:
 * locate the header inside the envelope, then read the relay state
 * from the bytes after it.
 */
static void test_envelope_changerelay(void)
{
    unsigned char frame[69 + DXP_CHANGERELAY_PAYLOAD_LEN];
    dxp_header_t hdr;
    int hdr_off;
    int payload_len;

    printf("--- envelope changeRelay (relay state decides) ---\n");

    memcpy(frame, capture_frame, 69);
    frame[22 + DXP_HDR_OFF_COMMAND] = DXP_CMND_IO;
    frame[22 + DXP_HDR_OFF_DESC] = DXP_IO_CHANGE_RELAY;
    frame[69 + DXP_CR_OFF_RELAY] = 0;

    hdr_off = -1;
    ASSERT_EQ_INT("cr envelope parse ok", 0,
                  dxp_parse_json_envelope(frame, (int)sizeof(frame),
                                          &hdr, &hdr_off));
    ASSERT_EQ_INT("cr header offset", 22, hdr_off);

    payload_len = (int)sizeof(frame) - hdr_off - DXP_HDR_SIZE;
    ASSERT_EQ_INT("cr payload length", DXP_CHANGERELAY_PAYLOAD_LEN,
                  payload_len);

    /* ENERGIZE: outlet turning on, must not halt */
    frame[69 + DXP_CR_OFF_STATE] = DXP_RELAY_ENERGIZE;
    ASSERT_EQ_INT("cr envelope ENERGIZE not shutdown",
                  DXP_SHUTDOWN_NO,
                  dxp_classify_shutdown(&hdr,
                                        frame + hdr_off + DXP_HDR_SIZE,
                                        payload_len));

    /* RELAX: outlet dropping, must halt */
    frame[69 + DXP_CR_OFF_STATE] = DXP_RELAY_RELAX;
    ASSERT_EQ_INT("cr envelope RELAX is shutdown",
                  DXP_SHUTDOWN_YES,
                  dxp_classify_shutdown(&hdr,
                                        frame + hdr_off + DXP_HDR_SIZE,
                                        payload_len));

    /* Same frame truncated to the header: no state, so no halt */
    ASSERT_EQ_INT("cr envelope truncated payload",
                  DXP_SHUTDOWN_NO_STATE,
                  dxp_classify_shutdown(&hdr,
                                        frame + hdr_off + DXP_HDR_SIZE,
                                        0));

    /* Credentials from the capture survive the header edit */
    ASSERT_EQ_INT("cr envelope cred match", 1,
                  dxp_validate_credentials(&hdr, "admin",
                                           "SecretTest23_"));
}

static void test_hello_constant(void)
{
    printf("--- hello constant ---\n");

    ASSERT_EQ_INT("hello length includes null", 10, DXP_HELLO_LEN);
    ASSERT_EQ_INT("hello string length", 9, (int)strlen(DXP_HELLO));
    ASSERT_EQ_STR("hello value", "hello-000", DXP_HELLO);
}

static void test_seq_wraparound(void)
{
    unsigned char buf[2];
    unsigned short seq;

    printf("--- sequence number wraparound ---\n");

    /* Seq 0xFFFF + 1 wraps to 0 */
    seq = 0xFFFF;
    seq++;
    ASSERT_EQ_UINT("seq wrap 0xFFFF+1", 0, seq);

    /* Verify via wire */
    dxp_write_seq(buf, 0xFFFF);
    seq = dxp_read_seq(buf);
    seq++;
    ASSERT_EQ_UINT("seq wire wrap", 0, seq);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("DxP protocol decode tests\n\n");

    test_seq_read_write();
    test_parse_header();
    test_build_header();
    test_validate_credentials();
    test_classify_shutdown();
    test_command_name();
    test_wire_size_47();
    test_hello_constant();
    test_seq_wraparound();
    test_json_envelope_capture();
    test_json_envelope_edge_cases();
    test_envelope_changerelay();

    printf("\n%d passed, %d failed, %d total\n",
           g_pass, g_fail, g_pass + g_fail);

    return (g_fail > 0) ? 1 : 0;
}
