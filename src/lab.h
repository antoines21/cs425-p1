#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <sys/types.h>

/* ======================================================================
 * Layer 1: Pure protocol helpers (No I/O)
 * ====================================================================== */

/**
 * Parses an SMTP response line.
 * @return 0 on success, -1 if malformed.
 */
int parse_reply_line(const char *line, int *code, int *is_final);

/**
 * Checks for the absence of line breaks (CR/LF) in a string
 * to prevent SMTP injection attacks.
 * @return 1 if vulnerable, 0 if safe.
 */
int check_injection(const char *str);

/**
 * Dynamically builds a command (e.g., MAIL FROM:<...>).
 */
char *build_command(const char *cmd, const char *arg);

/**
 * Builds the full data block (headers + dot-stuffed body).
 * Correctly handles CRLF and dot-stuffing (double dot).
 */
char *build_data_payload(const char *from, const char *to, const char *subject, const char *body);


/* ======================================================================
 * Layer 2: The session (Transport-agnostic)
 * ====================================================================== */

typedef ssize_t (*read_cb)(void *ctx, char *buf, size_t len);
typedef ssize_t (*write_cb)(void *ctx, const char *buf, size_t len);

/* I/O context with an internal buffer for line-based reads */
struct io_context {
    read_cb read_fn;
    write_cb write_fn;
    void *ctx;           /* User context (e.g., pointer to fd) */
    char buf[4096];
    size_t buf_pos;
    size_t buf_len;
};

void io_context_init(struct io_context *io, read_cb r, write_cb w, void *ctx);

/* Reads a full line ending with \n */
int session_read_line(struct io_context *io, char *line_out, size_t max_len);

/* Reads a full SMTP response (handles multiple lines). Returns the code. */
int session_read_reply(struct io_context *io, int *out_code);

/* Writes exactly len bytes */
int session_write_exact(struct io_context *io, const char *data);

/* Sends a command and appends CRLF */
int session_send_command(struct io_context *io, const char *cmd);

/* Executes the full SMTP session according to the defined workflow */
int session_run(struct io_context *io, const char *from, const char *to, 
                const char *subject, const char *body, const char *helo_host);


/* ======================================================================
 * Layer 3: Socket Transport
 * ====================================================================== */

/* Connects a socket to the provided host and port */
int socket_connect(const char *host, const char *port);

/* Callbacks compatible with io_context */
ssize_t socket_read_cb(void *ctx, char *buf, size_t len);
ssize_t socket_write_cb(void *ctx, const char *buf, size_t len);

#endif // LAB_H