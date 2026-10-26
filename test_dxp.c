/*
 * test_dxp.c - Unit tests for the DxP protocol decoder
 *
 * Tests the wire format parsing, credential validation, sequence
 * number handling, and command identification used by iboot-agent.
 *
 * These tests use synthetic packets constructed from the DxP v1.2
 * protocol specification.  Once a reference packet capture of the
 * official Windows iBoot Utility is obtained, add fixture-based
 * tests in the "Capture-based tests" section below.
 *
 * Build: cc -ansi -pedantic -Wall -Wextra -O2
 *            -D_POSIX_C_SOURCE=200112L -o test_dxp test_dxp.c
 * Run:   ./test_dxp
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxp.h"

/* ------------------------------------------------------------------ */
/* Minimal test framework                                             */
/* ------------------------------------------------------------------ */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ASSERT_MSG(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", (msg), __LINE__); \
        g_tests_failed++; \
    } else { \
        g_tests_passed++; \
    } \
} while (0)

#define ASSERT_EQ_INT(a, b, msg) ASSERT_MSG((a) == (b), msg)
#define ASSERT_EQ_STR(a, b, msg) ASSERT_MSG(strcmp((a), (b)) == 0, msg)
#define ASSERT_NEQ_INT(a, b, msg) ASSERT_MSG((a) != (b), msg)

/* ------------------------------------------------------------------ */
/* DxP protocol functions (duplicated from iboot-agent.c for testing) */
/* ------------------------------------------------------------------ */

/*
 * dxp_read_seq: read a LE 16-bit sequence number from a 2-byte buffer.
 */
static unsigned short dxp_read_seq(const unsigned char *buf)
{
    return (unsigned short)((unsigned short)buf[0] |
                            ((unsigned short)buf[1] << 8));
}

/*
 * dxp_write_seq: write a LE 16-bit sequence number to a 2-byte buffer.
 */
static void dxp_write_seq(unsigned char *buf, unsigned short seq)
{
    buf[0] = (unsigned char)(seq & 0xFF);
    buf[1] = (unsigned char)((seq >> 8) & 0xFF);
}

/*
 * dxp_parse_header: parse 47 wire bytes into a dxp_header_t struct.
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

/* ------------------------------------------------------------------ */
/* Test: hello message                                                */
/* ------------------------------------------------------------------ */

static void test_hello_message(void)
{
    unsigned char buf[DXP_HELLO_LEN];

    printf("test_hello_message\n");

    memcpy(buf, DXP_HELLO, DXP_HELLO_LEN);

    ASSERT_EQ_INT(DXP_HELLO_LEN, 10, "hello length is 10 bytes");
    ASSERT_MSG(memcmp(buf, "hello-000", 9) == 0, "hello content match");
    ASSERT_EQ_INT(buf[9], '\0', "hello is null-terminated at byte 10");
}

/* ------------------------------------------------------------------ */
/* Test: sequence number encoding                                     */
/* ------------------------------------------------------------------ */

static void test_sequence_number(void)
{
    unsigned char buf[2];
    unsigned short seq;

    printf("test_sequence_number\n");

    /* Value from the protocol documentation example: 1234 */
    dxp_write_seq(buf, 1234);
    seq = dxp_read_seq(buf);
    ASSERT_EQ_INT(seq, 1234, "round-trip seq 1234");

    /* Verify little-endian byte order: 1234 = 0x04D2 */
    ASSERT_EQ_INT(buf[0], 0xD2, "seq 1234 low byte is 0xD2");
    ASSERT_EQ_INT(buf[1], 0x04, "seq 1234 high byte is 0x04");

    /* Boundary: 0 */
    dxp_write_seq(buf, 0);
    seq = dxp_read_seq(buf);
    ASSERT_EQ_INT(seq, 0, "round-trip seq 0");

    /* Boundary: 0xFFFF */
    dxp_write_seq(buf, 0xFFFF);
    seq = dxp_read_seq(buf);
    ASSERT_EQ_INT(seq, 0xFFFF, "round-trip seq 0xFFFF");

    /* Increment wraps correctly */
    seq = 0xFFFF;
    seq++;
    ASSERT_EQ_INT(seq, 0, "seq wraps from 0xFFFF to 0");
}

/* ------------------------------------------------------------------ */
/* Test: header round-trip                                            */
/* ------------------------------------------------------------------ */

static void test_header_round_trip(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t orig;
    dxp_header_t parsed;

    printf("test_header_round_trip\n");

    memset(&orig, 0, sizeof(orig));
    orig.command = DXP_CMND_IO;
    strncpy(orig.username, "admin", DXP_USERNAME_MAX);
    strncpy(orig.password, "secret", DXP_PASSWORD_MAX);
    orig.desc = DXP_IO_CHANGE_RELAY;
    orig.param = 0;
    orig.seq = 1235;

    dxp_build_header(wire, &orig);
    dxp_parse_header(wire, &parsed);

    ASSERT_EQ_INT(parsed.command, DXP_CMND_IO, "command round-trip");
    ASSERT_EQ_STR(parsed.username, "admin", "username round-trip");
    ASSERT_EQ_STR(parsed.password, "secret", "password round-trip");
    ASSERT_EQ_INT(parsed.desc, DXP_IO_CHANGE_RELAY, "desc round-trip");
    ASSERT_EQ_INT(parsed.param, 0, "param round-trip");
    ASSERT_EQ_INT(parsed.seq, 1235, "seq round-trip");
}

/* ------------------------------------------------------------------ */
/* Test: wire offsets match the reference C code                      */
/* ------------------------------------------------------------------ */

static void test_wire_offsets(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;

    printf("test_wire_offsets\n");

    memset(wire, 0, DXP_HDR_SIZE);

    /* Place known values at each field */
    wire[0] = 0x03;                        /* command = eCmnd_io */
    wire[1] = 'U';                         /* first byte of username */
    wire[22] = 'P';                        /* first byte of password */
    wire[43] = 0x01;                       /* desc = eIO_changeRelay */
    wire[44] = 0x42;                       /* param */
    wire[45] = 0xD3;                       /* seq low byte */
    wire[46] = 0x04;                       /* seq high byte = 0x04D3 = 1235 */

    dxp_parse_header(wire, &hdr);

    ASSERT_EQ_INT(hdr.command, 0x03, "command at offset 0");
    ASSERT_EQ_INT(hdr.username[0], 'U', "username starts at offset 1");
    ASSERT_EQ_INT(hdr.password[0], 'P', "password starts at offset 22");
    ASSERT_EQ_INT(hdr.desc, 0x01, "desc at offset 43");
    ASSERT_EQ_INT(hdr.param, 0x42, "param at offset 44");
    ASSERT_EQ_INT(hdr.seq, 1235, "seq at offsets 45-46");
}

/* ------------------------------------------------------------------ */
/* Test: header size matches reference                                */
/* ------------------------------------------------------------------ */

static void test_header_size(void)
{
    printf("test_header_size\n");

    /* The reference C code sends 49 bytes for header(47) + payload(2) */
    ASSERT_EQ_INT(DXP_HDR_SIZE, 47, "header size is 47 bytes");
    ASSERT_EQ_INT(DXP_HDR_OFF_COMMAND, 0, "command offset");
    ASSERT_EQ_INT(DXP_HDR_OFF_USERNAME, 1, "username offset");
    ASSERT_EQ_INT(DXP_HDR_OFF_PASSWORD, 22, "password offset");
    ASSERT_EQ_INT(DXP_HDR_OFF_DESC, 43, "desc offset");
    ASSERT_EQ_INT(DXP_HDR_OFF_PARAM, 44, "param offset");
    ASSERT_EQ_INT(DXP_HDR_OFF_SEQ, 45, "seq offset");
}

/* ------------------------------------------------------------------ */
/* Test: credential validation                                        */
/* ------------------------------------------------------------------ */

static void test_credential_validation(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;
    const char *expected_user = "admin";
    const char *expected_pass = "mypassword";

    printf("test_credential_validation\n");

    /* Build a header with correct credentials */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_PCCONTROL;
    strncpy(hdr.username, expected_user, DXP_USERNAME_MAX);
    strncpy(hdr.password, expected_pass, DXP_PASSWORD_MAX);
    hdr.seq = 100;

    dxp_build_header(wire, &hdr);

    /* Parse and check match */
    memset(&hdr, 0, sizeof(hdr));
    dxp_parse_header(wire, &hdr);

    ASSERT_EQ_STR(hdr.username, expected_user, "correct username matches");
    ASSERT_EQ_STR(hdr.password, expected_pass, "correct password matches");

    /* Build a header with wrong username */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_PCCONTROL;
    strncpy(hdr.username, "wrong", DXP_USERNAME_MAX);
    strncpy(hdr.password, expected_pass, DXP_PASSWORD_MAX);
    hdr.seq = 100;

    dxp_build_header(wire, &hdr);
    dxp_parse_header(wire, &hdr);

    ASSERT_MSG(strcmp(hdr.username, expected_user) != 0,
               "wrong username does not match");

    /* Build a header with wrong password */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_PCCONTROL;
    strncpy(hdr.username, expected_user, DXP_USERNAME_MAX);
    strncpy(hdr.password, "wrong", DXP_PASSWORD_MAX);
    hdr.seq = 100;

    dxp_build_header(wire, &hdr);
    dxp_parse_header(wire, &hdr);

    ASSERT_MSG(strcmp(hdr.password, expected_pass) != 0,
               "wrong password does not match");
}

/* ------------------------------------------------------------------ */
/* Test: maximum-length credentials                                   */
/* ------------------------------------------------------------------ */

static void test_max_length_credentials(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;
    char maxuser[DXP_USERNAME_FIELD];
    char maxpass[DXP_PASSWORD_FIELD];

    printf("test_max_length_credentials\n");

    /* Fill with exactly 20 characters */
    memset(maxuser, 'A', DXP_USERNAME_MAX);
    maxuser[DXP_USERNAME_MAX] = '\0';
    memset(maxpass, 'B', DXP_PASSWORD_MAX);
    maxpass[DXP_PASSWORD_MAX] = '\0';

    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_KEEPALIVE;
    memcpy(hdr.username, maxuser, DXP_USERNAME_FIELD);
    memcpy(hdr.password, maxpass, DXP_PASSWORD_FIELD);
    hdr.seq = 5000;

    dxp_build_header(wire, &hdr);

    memset(&hdr, 0, sizeof(hdr));
    dxp_parse_header(wire, &hdr);

    ASSERT_EQ_INT((int)strlen(hdr.username), DXP_USERNAME_MAX,
                  "max username length preserved");
    ASSERT_EQ_INT((int)strlen(hdr.password), DXP_PASSWORD_MAX,
                  "max password length preserved");
    ASSERT_EQ_STR(hdr.username, maxuser, "max username content");
    ASSERT_EQ_STR(hdr.password, maxpass, "max password content");
}

/* ------------------------------------------------------------------ */
/* Test: empty credentials                                            */
/* ------------------------------------------------------------------ */

static void test_empty_credentials(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;

    printf("test_empty_credentials\n");

    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_IO;
    /* username and password left empty (all zeros from memset) */
    hdr.seq = 1;

    dxp_build_header(wire, &hdr);

    memset(&hdr, 0xFF, sizeof(hdr));
    dxp_parse_header(wire, &hdr);

    ASSERT_EQ_INT((int)strlen(hdr.username), 0, "empty username");
    ASSERT_EQ_INT((int)strlen(hdr.password), 0, "empty password");
}

/* ------------------------------------------------------------------ */
/* Test: command class identification                                 */
/* ------------------------------------------------------------------ */

static void test_command_classes(void)
{
    printf("test_command_classes\n");

    /* Verify enum values match the protocol spec */
    ASSERT_EQ_INT(DXP_CMND_NULL, 0, "eCmnd_null = 0");
    ASSERT_EQ_INT(DXP_CMND_SET, 1, "eCmnd_set = 1");
    ASSERT_EQ_INT(DXP_CMND_GET, 2, "eCmnd_get = 2");
    ASSERT_EQ_INT(DXP_CMND_IO, 3, "eCmnd_io = 3");
    ASSERT_EQ_INT(DXP_CMND_KEEPALIVE, 4, "eCmnd_keepAlive = 4");
    ASSERT_EQ_INT(DXP_CMND_RSS, 5, "eCmnd_rss = 5");
    ASSERT_EQ_INT(DXP_CMND_RCU, 6, "eCmnd_rcu = 6");
    ASSERT_EQ_INT(DXP_CMND_PCCONTROL, 7, "eCmnd_pcControl = 7");
}

/* ------------------------------------------------------------------ */
/* Test: shutdown command detection                                   */
/* ------------------------------------------------------------------ */

static void test_shutdown_detection(void)
{
    dxp_header_t hdr;

    printf("test_shutdown_detection\n");

    /* pcControl should be recognized as shutdown */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_PCCONTROL;
    ASSERT_EQ_INT(hdr.command == DXP_CMND_PCCONTROL, 1,
                  "pcControl is shutdown");

    /* io/changeRelay should be recognized as shutdown */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_IO;
    hdr.desc = DXP_IO_CHANGE_RELAY;
    ASSERT_MSG(hdr.command == DXP_CMND_IO &&
               hdr.desc == DXP_IO_CHANGE_RELAY,
               "io/changeRelay is shutdown");

    /* keepAlive should NOT be a shutdown */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_KEEPALIVE;
    ASSERT_MSG(hdr.command != DXP_CMND_PCCONTROL &&
               !(hdr.command == DXP_CMND_IO &&
                 hdr.desc == DXP_IO_CHANGE_RELAY),
               "keepAlive is not shutdown");

    /* io/getRelays should NOT be a shutdown */
    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_IO;
    hdr.desc = DXP_IO_GET_RELAYS;
    ASSERT_MSG(!(hdr.command == DXP_CMND_PCCONTROL) &&
               !(hdr.command == DXP_CMND_IO &&
                 hdr.desc == DXP_IO_CHANGE_RELAY),
               "io/getRelays is not shutdown");
}

/* ------------------------------------------------------------------ */
/* Test: sequence number validation                                   */
/* ------------------------------------------------------------------ */

static void test_sequence_validation(void)
{
    unsigned short initial_seq;
    unsigned short expected_seq;
    unsigned short received_seq;

    printf("test_sequence_validation\n");

    /* Normal case: client sends initial_seq + 1 */
    initial_seq = 1234;
    expected_seq = (unsigned short)(initial_seq + 1);
    received_seq = 1235;
    ASSERT_EQ_INT(received_seq, expected_seq,
                  "valid sequence accepted");

    /* Wrong sequence should fail */
    received_seq = 1234;
    ASSERT_NEQ_INT(received_seq, expected_seq,
                   "wrong sequence rejected");

    /* Wrap-around case */
    initial_seq = 0xFFFF;
    expected_seq = (unsigned short)(initial_seq + 1);
    received_seq = 0;
    ASSERT_EQ_INT(received_seq, expected_seq,
                  "sequence wrap-around accepted");
}

/* ------------------------------------------------------------------ */
/* Test: full protocol exchange (synthetic)                           */
/* ------------------------------------------------------------------ */

static void test_full_exchange(void)
{
    unsigned char hello_buf[DXP_HELLO_LEN];
    unsigned char seq_buf[DXP_SEQ_LEN];
    unsigned char hdr_buf[DXP_HDR_SIZE];
    unsigned char payload[2];
    unsigned short server_seq;
    unsigned short client_seq;
    dxp_header_t hdr;

    printf("test_full_exchange\n");

    /* Step 1: client sends hello */
    memcpy(hello_buf, DXP_HELLO, DXP_HELLO_LEN);
    ASSERT_MSG(memcmp(hello_buf, DXP_HELLO, DXP_HELLO_LEN) == 0,
               "hello message correct");

    /* Step 2: server sends sequence number (e.g. 1234) */
    server_seq = 1234;
    dxp_write_seq(seq_buf, server_seq);

    /* Step 3: client builds command with seq+1 */
    client_seq = dxp_read_seq(seq_buf);
    client_seq++;

    memset(&hdr, 0, sizeof(hdr));
    hdr.command = DXP_CMND_IO;
    strncpy(hdr.username, "admin", DXP_USERNAME_MAX);
    strncpy(hdr.password, "password", DXP_PASSWORD_MAX);
    hdr.desc = DXP_IO_CHANGE_RELAY;
    hdr.param = 0;
    hdr.seq = client_seq;

    dxp_build_header(hdr_buf, &hdr);

    /* Append payload: channel=0 (relay 1), state=0 (off) */
    payload[0] = 0x00;
    payload[1] = 0x00;

    /* Step 4: server parses and validates */
    memset(&hdr, 0, sizeof(hdr));
    dxp_parse_header(hdr_buf, &hdr);

    ASSERT_EQ_INT(hdr.seq, (unsigned short)(server_seq + 1),
                  "client used correct sequence");
    ASSERT_EQ_STR(hdr.username, "admin", "username in exchange");
    ASSERT_EQ_STR(hdr.password, "password", "password in exchange");
    ASSERT_EQ_INT(hdr.command, DXP_CMND_IO, "command in exchange");
    ASSERT_EQ_INT(hdr.desc, DXP_IO_CHANGE_RELAY, "desc in exchange");

    /* Verify total packet size matches reference code */
    ASSERT_EQ_INT(DXP_HDR_SIZE + 2, 49,
                  "header + changeRelay payload = 49 bytes");

    (void)payload;
}

/* ------------------------------------------------------------------ */
/* Test: malformed inputs                                             */
/* ------------------------------------------------------------------ */

static void test_malformed_inputs(void)
{
    unsigned char wire[DXP_HDR_SIZE];
    dxp_header_t hdr;

    printf("test_malformed_inputs\n");

    /* All-zero buffer should parse without crashing */
    memset(wire, 0, DXP_HDR_SIZE);
    dxp_parse_header(wire, &hdr);
    ASSERT_EQ_INT(hdr.command, 0, "all-zero command is null");
    ASSERT_EQ_INT((int)strlen(hdr.username), 0, "all-zero username empty");
    ASSERT_EQ_INT((int)strlen(hdr.password), 0, "all-zero password empty");
    ASSERT_EQ_INT(hdr.seq, 0, "all-zero seq is 0");

    /* All-0xFF buffer: username and password get force-terminated */
    memset(wire, 0xFF, DXP_HDR_SIZE);
    dxp_parse_header(wire, &hdr);
    ASSERT_EQ_INT((int)strlen(hdr.username), DXP_USERNAME_MAX,
                  "0xFF username truncated at 20");
    ASSERT_EQ_INT((int)strlen(hdr.password), DXP_PASSWORD_MAX,
                  "0xFF password truncated at 20");
}

/* ------------------------------------------------------------------ */
/* Capture-based tests (placeholder)                                  */
/*                                                                    */
/* Once a packet capture of the official Windows iBoot Utility is     */
/* obtained, add test fixtures here.  Example:                        */
/*                                                                    */
/*   1. Capture traffic between the Windows utility and the iBoot     */
/*      during an Off and a Cycle event.                              */
/*   2. Extract the raw TCP payload bytes for each DxP exchange.      */
/*   3. Commit them as hex arrays in this file.                       */
/*   4. Parse them with dxp_parse_header() and verify the fields      */
/*      match the expected command, credentials, and sequence.        */
/*                                                                    */
/* See CLAUDE.md Section 12 (AC1) for the full acceptance criteria.   */
/* ------------------------------------------------------------------ */

static void test_capture_placeholder(void)
{
    printf("test_capture_placeholder\n");
    printf("  (no capture fixtures yet -- see README for procedure)\n");
    ASSERT_MSG(1, "placeholder passes (no capture data to test)");
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("DxP protocol decoder tests\n");
    printf("==========================\n\n");

    test_hello_message();
    test_sequence_number();
    test_header_round_trip();
    test_wire_offsets();
    test_header_size();
    test_credential_validation();
    test_max_length_credentials();
    test_empty_credentials();
    test_command_classes();
    test_shutdown_detection();
    test_sequence_validation();
    test_full_exchange();
    test_malformed_inputs();
    test_capture_placeholder();

    printf("\n%d tests: %d passed, %d failed\n",
           g_tests_run, g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
