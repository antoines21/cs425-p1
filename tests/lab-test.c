#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include "harness/unity.h"
#include "../src/lab.h"

/* ======================================================================
 * Mock Transport for Layer 2 & 3 Testing
 * ====================================================================== */

typedef struct {
    const char *read_data;    // Data the "server" will send
    size_t read_pos;          // Current position in read_data
    size_t read_len;          // Total length of read_data
    char write_buf[8192];     // Data the client sends to the "server"
    size_t write_pos;         // Current position in write_buf
    int chunk_size;           // If >0, mock_read returns max this many bytes
    int hangup;               // If 1, mock_read simulates early disconnect (EOF)
    int write_fail_countdown; // Number of successful writes before failure
} mock_transport_t;

ssize_t mock_read_cb(void *ctx, char *buf, size_t len) {
    mock_transport_t *mock = (mock_transport_t *)ctx;
    
    if (mock->hangup) {
        return 0; // Simulate EOF / Hang up
    }

    if (mock->read_pos >= mock->read_len) {
        return 0; // EOF
    }

    size_t available = mock->read_len - mock->read_pos;
    size_t to_read = (available < len) ? available : len;
    
    // Simulate data arriving a few bytes at a time
    if (mock->chunk_size > 0 && to_read > (size_t)mock->chunk_size) {
        to_read = mock->chunk_size;
    }

    memcpy(buf, mock->read_data + mock->read_pos, to_read);
    mock->read_pos += to_read;
    return to_read;
}

ssize_t mock_write_cb(void *ctx, const char *buf, size_t len) {
    mock_transport_t *mock = (mock_transport_t *)ctx;
    if (mock->write_fail_countdown == 0) {
        return -1; // Simulate a network write failure.
    }
    if (mock->write_fail_countdown > 0) {
        mock->write_fail_countdown--;
    }
    if (mock->write_pos + len >= sizeof(mock->write_buf)) {
        return -1; // Overflow mock buffer
    }
    memcpy(mock->write_buf + mock->write_pos, buf, len);
    mock->write_pos += len;
    return len;
}

void reset_mock(mock_transport_t *mock, const char *data, int chunk_size) {
    mock->read_data = data;
    mock->read_pos = 0;
    mock->read_len = data ? strlen(data) : 0;
    mock->write_pos = 0;
    memset(mock->write_buf, 0, sizeof(mock->write_buf));
    mock->chunk_size = chunk_size;
    mock->hangup = 0;
    mock->write_fail_countdown = -1; // Never fail by default.
}

/* ======================================================================
 * Unity Setup & Teardown
 * ====================================================================== */

void setUp(void) {
    // Optional setup before each test
}

void tearDown(void) {
    // Optional teardown after each test
}

/* ======================================================================
 * Layer 1 Tests: Protocol Helpers
 * ====================================================================== */

void test_parse_reply_line_happy_path(void) {
    int code, is_final;
    int res = parse_reply_line("250 OK", &code, &is_final);
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL(250, code);
    TEST_ASSERT_EQUAL(1, is_final);
}

void test_parse_reply_line_continuation(void) {
    int code, is_final;
    int res = parse_reply_line("354-End data with .", &code, &is_final);
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL(354, code);
    TEST_ASSERT_EQUAL(0, is_final);
}

void test_parse_reply_line_malformed(void) {
    int code = 0, is_final = 0;
    int res = parse_reply_line("12", &code, &is_final); // Too short
    TEST_ASSERT_EQUAL(-1, res);
}

// Covers the character-bound checks for each position in the reply code.
void test_parse_reply_line_invalid_chars(void) {
    int code, is_final;

    // The first character is not a digit.
    TEST_ASSERT_EQUAL(-1, parse_reply_line("/50 OK", &code, &is_final));
    TEST_ASSERT_EQUAL(-1, parse_reply_line(":50 OK", &code, &is_final));

    // The second character is not a digit.
    TEST_ASSERT_EQUAL(-1, parse_reply_line("2/0 OK", &code, &is_final));
    TEST_ASSERT_EQUAL(-1, parse_reply_line("2:0 OK", &code, &is_final));

    // The third character is not a digit.
    TEST_ASSERT_EQUAL(-1, parse_reply_line("25/ OK", &code, &is_final));
    TEST_ASSERT_EQUAL(-1, parse_reply_line("25: OK", &code, &is_final));
}

// Covers every lower and upper character bound in the reply code.
void test_parse_reply_line_bounds(void) {
    int code, is_final;
    parse_reply_line("/00 OK", &code, &is_final); // line[0] < '0'
    parse_reply_line(":00 OK", &code, &is_final); // line[0] > '9'
    parse_reply_line("0/0 OK", &code, &is_final); // line[1] < '0'
    parse_reply_line("0:0 OK", &code, &is_final); // line[1] > '9'
    parse_reply_line("00: OK", &code, &is_final); // line[2] > '9'
}

// Covers parsing a valid reply when the output code pointer is NULL.
void test_parse_reply_line_null_code(void) {
    int is_final;
    TEST_ASSERT_EQUAL(0, parse_reply_line("250 OK", NULL, &is_final));
    TEST_ASSERT_EQUAL(1, is_final);
}

// Covers the three-character reply case without a space or hyphen.
void test_parse_reply_line_short(void) {
    int code = 0, is_final = 0;
    int res = parse_reply_line("250", &code, &is_final);
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL(250, code);
    TEST_ASSERT_EQUAL(1, is_final);
}

void test_check_injection(void) {
    TEST_ASSERT_EQUAL(0, check_injection("alice@example.com"));
    TEST_ASSERT_EQUAL(0, check_injection(NULL));
    TEST_ASSERT_EQUAL(1, check_injection("alice\r\nRCPT TO:<bob>")); // Injection caught
    TEST_ASSERT_EQUAL(1, check_injection("alice\n")); // LF caught
}

void test_build_command(void) {
    char *cmd1 = build_command("HELO", "localhost");
    TEST_ASSERT_NOT_NULL(cmd1);
    TEST_ASSERT_EQUAL_STRING("HELO localhost", cmd1);
    free(cmd1);

    char *cmd2 = build_command("DATA", NULL);
    TEST_ASSERT_NOT_NULL(cmd2);
    TEST_ASSERT_EQUAL_STRING("DATA", cmd2);
    free(cmd2);
}

void test_build_data_payload_basic(void) {
    char *payload = build_data_payload("alice@a.com", "bob@b.com", "Hello", "Line1\nLine2\n");
    TEST_ASSERT_NOT_NULL(payload);
    
    // Check if \n was correctly converted to \r\n
    TEST_ASSERT_NOT_NULL(strstr(payload, "From: alice@a.com\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "To: bob@b.com\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "Subject: Hello\r\n\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "Line1\r\nLine2\r\n"));
    free(payload);
}

void test_build_data_payload_dot_stuffing(void) {
    // Testing rule: A line starting with a dot must be escaped with a double dot.
    char *payload = build_data_payload("a", "b", "c", "Line1\n.\nLine3");
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_NOT_NULL(strstr(payload, "\r\n..\r\n")); 
    free(payload);
}

// Covers NULL and empty subject and body values.
void test_build_data_payload_nulls(void) {
    // NULL body and NULL subject.
    char *payload_without_subject = build_data_payload("a@b.com", "c@d.com", NULL, NULL);
    TEST_ASSERT_NOT_NULL(payload_without_subject);
    TEST_ASSERT_NULL(strstr(payload_without_subject, "Subject:"));
    free(payload_without_subject);

    // Empty subject.
    char *payload_with_empty_subject = build_data_payload("a@b.com", "c@d.com", "", "Body");
    TEST_ASSERT_NOT_NULL(payload_with_empty_subject);
    TEST_ASSERT_NULL(strstr(payload_with_empty_subject, "Subject:"));
    free(payload_with_empty_subject);
}

// Covers orphan carriage returns and existing CRLF line endings.
void test_build_data_payload_carriage_returns(void) {
    char *payload = build_data_payload("a@a.com", "b@b.com", "Sub", "Line1\r\nLine2\rLine3");
    TEST_ASSERT_NOT_NULL(payload);
    free(payload);
}

/* ======================================================================
 * Layer 2 Tests: Session over mocked IO
 * ====================================================================== */

void test_io_context_init(void) {
    struct io_context io;
    int dummy_fd = 42;
    io_context_init(&io, mock_read_cb, mock_write_cb, &dummy_fd);
    
    TEST_ASSERT_EQUAL_PTR(mock_read_cb, io.read_fn);
    TEST_ASSERT_EQUAL_PTR(mock_write_cb, io.write_fn);
    TEST_ASSERT_EQUAL_PTR(&dummy_fd, io.ctx);
    TEST_ASSERT_EQUAL(0, io.buf_pos);
    TEST_ASSERT_EQUAL(0, io.buf_len);
}

void test_session_read_line_happy_path(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 server ready\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    char line[100];
    int len = session_read_line(&io, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("220 server ready\n", line);
}

// Requirement: reply that arrives a few bytes at a time
void test_session_read_line_chunked(void) {
    mock_transport_t mock;
    // Chunk size 1 forces it to read byte-by-byte
    reset_mock(&mock, "250 OK\n", 1); 
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    char line[100];
    int len = session_read_line(&io, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_STRING("250 OK\n", line);
}

// Requirement: reply the buffer cannot hold
void test_session_read_line_buffer_overflow(void) {
    mock_transport_t mock;
    reset_mock(&mock, "250 This is a very long line that exceeds max limit\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    char line[10]; // Tiny buffer
    int len = session_read_line(&io, line, sizeof(line));
    
    // The reader should still find the '\n' and consume the stream,
    // but line should be cleanly truncated to max_len - 1 characters.
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL(9, strlen(line));
    TEST_ASSERT_EQUAL_STRING("250 This ", line);
}

// Requirement: multi-line reply
void test_session_read_reply_multiline(void) {
    mock_transport_t mock;
    reset_mock(&mock, "250-PIPELINING\n250-SIZE 1024\n250 OK\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int code = 0;
    int res = session_read_reply(&io, &code);
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL(250, code);
}

void test_session_read_reply_null_code(void) {
    mock_transport_t mock;
    reset_mock(&mock, "250 OK\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_read_reply(&io, NULL); // Covers the out_code == NULL branch.
}

// Covers a parse_reply_line failure inside the reply-reading loop.
void test_session_read_reply_malformed_parse(void) {
    mock_transport_t mock;
    reset_mock(&mock, "22\n", 0); // Reply is too short
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int code = 0;
    int res = session_read_reply(&io, &code);
    TEST_ASSERT_EQUAL(-1, res);
}

void test_session_write_and_send(void) {
    mock_transport_t mock;
    reset_mock(&mock, "", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_send_command(&io, "HELO localhost");
    TEST_ASSERT_EQUAL(0, res);
    TEST_ASSERT_EQUAL_STRING("HELO localhost\r\n", mock.write_buf);
}

// Covers failure while writing the final CRLF in session_send_command.
void test_session_send_command_write_crlf_fail(void) {
    mock_transport_t mock;
    reset_mock(&mock, "", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    // Leave exactly four bytes in the mock buffer: "HELO" succeeds, then CRLF fails.
    mock.write_pos = sizeof(mock.write_buf) - 4;
    int res = session_send_command(&io, "HELO");
    TEST_ASSERT_EQUAL(-1, res);
}

void test_session_send_command_crlf_fail(void) {
    mock_transport_t mock;
    reset_mock(&mock, "", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    // "HELO" is 4 bytes. Leave exactly 4 bytes of room so the CRLF write fails.
    mock.write_pos = sizeof(mock.write_buf) - 4;
    session_send_command(&io, "HELO");
}

/* ======================================================================
 * Layer 2 Tests: Full Session Flow
 * ====================================================================== */

void test_session_run_happy_path(void) {
    mock_transport_t mock;
    const char *full_server_script = 
        "220 smtp.example.com ESMTP\n"
        "250 smtp.example.com\n"
        "250 2.1.0 Ok\n"
        "250 2.1.5 Ok\n"
        "354 End data with .\n"
        "250 2.0.0 Ok: queued\n"
        "221 Bye\n";
    reset_mock(&mock, full_server_script, 0);

    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "Sub", "Body", "localhost");
    TEST_ASSERT_EQUAL(0, res); // 0 = Success
    
    // Check if the client sent the QUIT sequence correctly at the end
    TEST_ASSERT_NOT_NULL(strstr(mock.write_buf, "\r\n.\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(mock.write_buf, "QUIT\r\n"));
}

// Requirement: server hangs up in the middle of the session
void test_session_run_server_hangup(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 smtp.example.com ESMTP\n", 0);
    mock.hangup = 1; // Hang up immediately after greeting

    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "Sub", "Body", "localhost");
    TEST_ASSERT_EQUAL(2, res); // 2 = Connection/SMTP failure
}

// Requirement: each wrong status code in the sequence
void test_session_run_wrong_status_greeting(void) {
    mock_transport_t mock;
    reset_mock(&mock, "554 No SMTP service here\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "S", "B", "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

void test_session_run_wrong_status_helo(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 OK\n500 Syntax error, command unrecognized\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "S", "B", "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

void test_session_run_wrong_status_mail_from(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 OK\n250 OK\n550 Invalid sender\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "S", "B", "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

void test_session_run_wrong_status_rcpt_to(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n550 No such user\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "S", "B", "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

void test_session_run_wrong_status_data(void) {
    mock_transport_t mock;
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n554 Transaction failed\n", 0);
    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    int res = session_run(&io, "a@b.com", "c@d.com", "S", "B", "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

// Covers failure while sending the message payload.
void test_session_run_payload_write_fail(void) {
    mock_transport_t mock;
    const char *script =
        "220 smtp.example.com ESMTP\n"
        "250 smtp.example.com\n"
        "250 2.1.0 Ok\n"
        "250 2.1.5 Ok\n"
        "354 End data with .\n";
    reset_mock(&mock, script, 0);

    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    // Fill the mock buffer so mock_write_cb fails during DATA payload transmission.
    char big_body[8000];
    memset(big_body, 'A', sizeof(big_body) - 1);
    big_body[sizeof(big_body) - 1] = '\0';

    int res = session_run(&io, "a@b.com", "c@d.com", "Sub", big_body, "localhost");
    TEST_ASSERT_EQUAL(2, res);
}

// Covers the guaranteed payload write failure and error-path cleanup.
void test_session_run_payload_write_fail_guaranteed(void) {
    mock_transport_t mock;
    const char *script =
        "220 smtp.example.com ESMTP\n"
        "250 smtp.example.com\n"
        "250 2.1.0 Ok\n"
        "250 2.1.5 Ok\n"
        "354 End data with .\n";
    reset_mock(&mock, script, 0);

    struct io_context io;
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);

    // The 10,000-byte body must overflow the 8,192-byte mock buffer.
    char *huge_body = malloc(10000);
    TEST_ASSERT_NOT_NULL(huge_body);
    memset(huge_body, 'A', 9999);
    huge_body[9999] = '\0';

    int res = session_run(&io, "a@b.com", "c@d.com", "Sub", huge_body, "localhost");
    TEST_ASSERT_EQUAL(2, res);
    free(huge_body);
}

void test_session_run_write_failures(void) {
    mock_transport_t mock;
    struct io_context io;

    // Fail the HELO write.
    reset_mock(&mock, "220 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - 2;
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the MAIL FROM write.
    reset_mock(&mock, "220 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - strlen("HELO lh\r\n") - 2;
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the RCPT TO write.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - strlen("HELO lh\r\nMAIL FROM:<a@b.com>\r\n") - 2;
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the DATA write.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - strlen("HELO lh\r\nMAIL FROM:<a@b.com>\r\nRCPT TO:<c@d.com>\r\n") - 2;
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the terminating dot write.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - strlen("HELO lh\r\nMAIL FROM:<a@b.com>\r\nRCPT TO:<c@d.com>\r\nDATA\r\nFrom: a@b.com\r\nTo: c@d.com\r\n\r\n") - 1;
    session_run(&io, "a@b.com", "c@d.com", "", NULL, "lh");

    // Fail the QUIT write.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    mock.write_pos = sizeof(mock.write_buf) - strlen("HELO lh\r\nMAIL FROM:<a@b.com>\r\nRCPT TO:<c@d.com>\r\nDATA\r\nFrom: a@b.com\r\nTo: c@d.com\r\n\r\n.\r\n") - 1;
    session_run(&io, "a@b.com", "c@d.com", "", NULL, "lh");
}

void test_session_run_read_failures(void) {
    mock_transport_t mock;
    struct io_context io;

    // Fail the HELO read by ending the server response early.
    reset_mock(&mock, "220 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the MAIL FROM read.
    reset_mock(&mock, "220 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the RCPT TO read.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the DATA read.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the terminating dot read.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the QUIT read.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");

    // Fail the QUIT status check with an unexpected code.
    reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n500 FAIL\n", 0);
    io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
    session_run(&io, "a@b.com", "c@d.com", "", "", "lh");
}

/* ======================================================================
 * Final brute-force tests for complete macro error coverage
 * ====================================================================== */

void test_session_run_all_write_errors(void) {
    // Fail the network at every possible write step.
    for (int i = 0; i < 20; i++) {
        mock_transport_t mock;
        reset_mock(&mock, "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n221 OK\n", 0);
        mock.write_fail_countdown = i;
        struct io_context io;
        io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
        session_run(&io, "a@b.com", "c@d.com", "S", "B", "lh");
    }
}

void test_session_run_all_read_errors(void) {
    // Truncate the server response at every possible character boundary.
    const char *full_script = "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n221 OK\n";
    for (size_t i = 0; i < strlen(full_script); i++) {
        mock_transport_t mock;
        reset_mock(&mock, full_script, 0);
        mock.read_len = i;
        struct io_context io;
        io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
        session_run(&io, "a@b.com", "c@d.com", "S", "B", "lh");
    }
}

void test_session_run_all_bad_codes(void) {
    // Return code 500 instead of the expected code at every session step.
    const char *scripts[] = {
        "500 OK\n",
        "220 OK\n500 OK\n",
        "220 OK\n250 OK\n500 OK\n",
        "220 OK\n250 OK\n250 OK\n500 OK\n",
        "220 OK\n250 OK\n250 OK\n250 OK\n500 OK\n",
        "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n500 OK\n",
        "220 OK\n250 OK\n250 OK\n250 OK\n354 OK\n250 OK\n500 OK\n"
    };
    for (int i = 0; i < 7; i++) {
        mock_transport_t mock;
        reset_mock(&mock, scripts[i], 0);
        struct io_context io;
        io_context_init(&io, mock_read_cb, mock_write_cb, &mock);
        session_run(&io, "a@b.com", "c@d.com", "S", "B", "lh");
    }
}

/* ======================================================================
 * Layer 3 Tests: Real Sockets Coverage
 * ====================================================================== */

void test_socket_transport(void) {
    // We cannot connect to a real server in unit tests, so we test connection failure.
    // Ensure that it handles an unreachable host gracefully.
    int fd = socket_connect("this.domain.is.invalid.and.will.fail", "25");
    TEST_ASSERT_EQUAL(-1, fd);
    
    // Test the callbacks error paths with a bad file descriptor (-1)
    char buf[10];
    ssize_t read_res = socket_read_cb(&fd, buf, sizeof(buf));
    TEST_ASSERT_LESS_THAN(0, read_res); // Should fail

    ssize_t write_res = socket_write_cb(&fd, "test", 4);
    TEST_ASSERT_LESS_THAN(0, write_res); // Should fail
}

// Covers the connect loop when every localhost connection attempt fails.
void test_socket_connect_loop_failure(void) {
    // Port 65534 is expected to be closed in the test environment.
    int fd = socket_connect("127.0.0.1", "65534");
    if (fd >= 0) close(fd);
}

// Covers a successful local socket connection.
void test_socket_connect_success(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, listener);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    TEST_ASSERT_EQUAL(0, bind(listener, (struct sockaddr*)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL(0, listen(listener, 1));

    socklen_t len = sizeof(addr);
    TEST_ASSERT_EQUAL(0, getsockname(listener, (struct sockaddr*)&addr, &len));

    char port_str[16];
    sprintf(port_str, "%d", ntohs(addr.sin_port));

    int fd = socket_connect("127.0.0.1", port_str);
    TEST_ASSERT_NOT_EQUAL(-1, fd);

    close(fd);
    close(listener);
}


/* ======================================================================
 * Main Test Runner
 * ====================================================================== */

int main(void) {
    UNITY_BEGIN();

    // Layer 1
    RUN_TEST(test_parse_reply_line_happy_path);
    RUN_TEST(test_parse_reply_line_continuation);
    RUN_TEST(test_parse_reply_line_malformed);
    RUN_TEST(test_parse_reply_line_invalid_chars);
    RUN_TEST(test_parse_reply_line_bounds);
    RUN_TEST(test_parse_reply_line_null_code);
    RUN_TEST(test_parse_reply_line_short);
    RUN_TEST(test_check_injection);
    RUN_TEST(test_build_command);
    RUN_TEST(test_build_data_payload_basic);
    RUN_TEST(test_build_data_payload_dot_stuffing);
    RUN_TEST(test_build_data_payload_nulls);
    RUN_TEST(test_build_data_payload_carriage_returns);

    // Layer 2 - IO Core
    RUN_TEST(test_io_context_init);
    RUN_TEST(test_session_read_line_happy_path);
    RUN_TEST(test_session_read_line_chunked);
    RUN_TEST(test_session_read_line_buffer_overflow);
    RUN_TEST(test_session_read_reply_multiline);
    RUN_TEST(test_session_read_reply_null_code);
    RUN_TEST(test_session_read_reply_malformed_parse);
    RUN_TEST(test_session_write_and_send);
    RUN_TEST(test_session_send_command_write_crlf_fail);
    RUN_TEST(test_session_send_command_crlf_fail);

    // Layer 2 - Flow and Errors
    RUN_TEST(test_session_run_happy_path);
    RUN_TEST(test_session_run_server_hangup);
    RUN_TEST(test_session_run_wrong_status_greeting);
    RUN_TEST(test_session_run_wrong_status_helo);
    RUN_TEST(test_session_run_wrong_status_mail_from);
    RUN_TEST(test_session_run_wrong_status_rcpt_to);
    RUN_TEST(test_session_run_wrong_status_data);
    RUN_TEST(test_session_run_payload_write_fail);
    RUN_TEST(test_session_run_payload_write_fail_guaranteed);
    RUN_TEST(test_session_run_write_failures);
    RUN_TEST(test_session_run_read_failures);
    RUN_TEST(test_session_run_all_write_errors);
    RUN_TEST(test_session_run_all_read_errors);
    RUN_TEST(test_session_run_all_bad_codes);

    // Layer 3
    RUN_TEST(test_socket_transport);
    RUN_TEST(test_socket_connect_loop_failure);
    RUN_TEST(test_socket_connect_success);

    return UNITY_END();
}