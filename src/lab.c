#include "lab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

/* ======================================================================
 * Layer 1
 * ====================================================================== */

int parse_reply_line(const char *line, int *code, int *is_final) {
    if (strlen(line) < 3) return -1;
    if (line[0] < '0' || line[0] > '9' ||
        line[1] < '0' || line[1] > '9' ||
        line[2] < '0' || line[2] > '9') return -1;
    
    int c = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
    if (code) *code = c;
    
    if (strlen(line) >= 4) {
        *is_final = (line[3] == ' ') ? 1 : 0;
    } else {
        *is_final = 1;
    }
    return 0;
}

int check_injection(const char *str) {
    if (!str) return 0;
    if (strchr(str, '\r') || strchr(str, '\n')) return 1;
    return 0;
}

char *build_command(const char *cmd, const char *arg) {
    size_t len = strlen(cmd) + (arg ? strlen(arg) : 0) + 5;
    char *res = malloc(len);
    if (!res) return NULL;
    
    if (arg) sprintf(res, "%s %s", cmd, arg);
    else sprintf(res, "%s", cmd);
    
    return res;
}

char *build_data_payload(const char *from, const char *to, const char *subject, const char *body) {
    size_t est_len = 1024 + (body ? strlen(body)*2 : 0);
    char *payload = malloc(est_len);
    if (!payload) return NULL;

    payload[0] = '\0';
    strcat(payload, "From: ");
    strcat(payload, from);
    strcat(payload, "\r\nTo: ");
    strcat(payload, to);
    if (subject && strlen(subject) > 0) {
        strcat(payload, "\r\nSubject: ");
        strcat(payload, subject);
    }
    strcat(payload, "\r\n\r\n"); 

    if (body) {
        char *curr = payload + strlen(payload);
        int is_sol = 1; /* Beginning of a line (Start Of Line) */
        for (size_t i = 0; i < strlen(body); i++) {
            if (is_sol && body[i] == '.') {
                *curr++ = '.'; /* Dot stuffing */
            }
            
            if (body[i] == '\r') {
                if (body[i+1] == '\n') {
                    *curr++ = '\r'; *curr++ = '\n';
                    is_sol = 1; i++; 
                } else {
                    *curr++ = body[i];
                    is_sol = 0;
                }
            } else if (body[i] == '\n') {
                *curr++ = '\r'; *curr++ = '\n';
                is_sol = 1;
            } else {
                *curr++ = body[i];
                is_sol = 0;
            }
        }
        *curr = '\0';
        if (!is_sol) {
            strcat(payload, "\r\n");
        }
    }
    return payload;
}


/* ======================================================================
 * Layer 2
 * ====================================================================== */

void io_context_init(struct io_context *io, read_cb r, write_cb w, void *ctx) {
    io->read_fn = r;
    io->write_fn = w;
    io->ctx = ctx;
    io->buf_pos = 0;
    io->buf_len = 0;
}

int session_read_line(struct io_context *io, char *line_out, size_t max_len) {
    size_t copied = 0;
    while (1) {
        if (io->buf_pos >= io->buf_len) {
            ssize_t n = io->read_fn(io->ctx, io->buf, sizeof(io->buf));
            if (n <= 0) return -1;
            io->buf_pos = 0;
            io->buf_len = (size_t)n;
        }

        char c = io->buf[io->buf_pos++];
        if (copied < max_len - 1) {
            line_out[copied++] = c;
        }

        if (c == '\n') {
            line_out[copied] = '\0';
            return (int)copied;
        }
    }
}

int session_read_reply(struct io_context *io, int *out_code) {
    char line[1024];
    int code = 0;
    int is_final = 0;

    while (!is_final) {
        if (session_read_line(io, line, sizeof(line)) < 0) {
            return -1; 
        }
        if (parse_reply_line(line, &code, &is_final) < 0) {
            return -1;
        }
    }
    if (out_code) *out_code = code;
    return 0;
}

int session_write_exact(struct io_context *io, const char *data) {
    size_t len = strlen(data);
    size_t written = 0;
    while (written < len) {
        ssize_t n = io->write_fn(io->ctx, data + written, len - written);
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

int session_send_command(struct io_context *io, const char *cmd) {
    if (session_write_exact(io, cmd) < 0) return -1;
    if (session_write_exact(io, "\r\n") < 0) return -1;
    return 0;
}

/* Macro to simplify response checking without memory leaks */
#define CHECK_REPLY(expected, step_name) \
    do { \
        if (session_read_reply(io, &code) < 0) { \
            fprintf(stderr, "Erreur de lecture (%s)\n", step_name); \
            goto error; \
        } \
        if (code != (expected)) { \
            fprintf(stderr, "Erreur %s: attendu %d, reçu %d\n", step_name, (expected), code); \
            goto error; \
        } \
    } while (0)

#define SEND_AND_CHECK(cmd_str, expected, step_name) \
    do { \
        if (session_send_command(io, cmd_str) < 0) { \
            fprintf(stderr, "Erreur d'envoi (%s)\n", step_name); \
            goto error; \
        } \
        CHECK_REPLY(expected, step_name); \
    } while (0)

int session_run(struct io_context *io, const char *from, const char *to, const char *subject, const char *body, const char *helo_host) {
    int code;
    char *cmd = NULL;
    char *payload = NULL;

    CHECK_REPLY(220, "GREETING");

    cmd = build_command("HELO", helo_host);
    if (!cmd) goto error;
    SEND_AND_CHECK(cmd, 250, "HELO");
    free(cmd);
    cmd = NULL;

    char from_arg[512];
    snprintf(from_arg, sizeof(from_arg), "<%s>", from);
    cmd = build_command("MAIL FROM:", from_arg);
    if (!cmd) goto error;
    SEND_AND_CHECK(cmd, 250, "MAIL FROM");
    free(cmd);
    cmd = NULL;

    char to_arg[512];
    snprintf(to_arg, sizeof(to_arg), "<%s>", to);
    cmd = build_command("RCPT TO:", to_arg);
    if (!cmd) goto error;
    SEND_AND_CHECK(cmd, 250, "RCPT TO");
    free(cmd);
    cmd = NULL;

    SEND_AND_CHECK("DATA", 354, "DATA");

    payload = build_data_payload(from, to, subject, body);
    if (!payload || session_write_exact(io, payload) < 0) {
        fprintf(stderr, "Erreur lors de l'envoi du payload\n");
        goto error;
    }
    free(payload);
    payload = NULL;

    SEND_AND_CHECK(".", 250, "END_DATA_DOT");
    SEND_AND_CHECK("QUIT", 221, "QUIT");

    return 0; 

error:
    if (cmd) free(cmd);
    if (payload) free(payload);
    return 2;
}


/* ======================================================================
 * Layer 3
 * ====================================================================== */

int socket_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }

    int sockfd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            break; 
        }
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;
}

ssize_t socket_read_cb(void *ctx, char *buf, size_t len) {
    int fd = *(int*)ctx;
    return recv(fd, buf, len, 0);
}

ssize_t socket_write_cb(void *ctx, const char *buf, size_t len) {
    int fd = *(int*)ctx;
    return send(fd, buf, len, 0);
}