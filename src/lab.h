#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <sys/types.h>

/* ======================================================================
 * Layer 1: Pure protocol helpers (No I/O)
 * ====================================================================== */

/**
 * Analyse une ligne de réponse SMTP.
 * @return 0 en cas de succès, -1 si malformé.
 */
int parse_reply_line(const char *line, int *code, int *is_final);

/**
 * Vérifie l'absence de retours à la ligne (CR/LF) dans une chaîne
 * pour prévenir les attaques par injection SMTP.
 * @return 1 si vulnérable, 0 si sûr.
 */
int check_injection(const char *str);

/**
 * Construit dynamiquement une commande (ex: MAIL FROM:<...>).
 */
char *build_command(const char *cmd, const char *arg);

/**
 * Construit le bloc entier de données (headers + body dot-stuffed).
 * Gère correctement les CRLF et le dot-stuffing (double point).
 */
char *build_data_payload(const char *from, const char *to, const char *subject, const char *body);


/* ======================================================================
 * Layer 2: The session (Transport-agnostic)
 * ====================================================================== */

typedef ssize_t (*read_cb)(void *ctx, char *buf, size_t len);
typedef ssize_t (*write_cb)(void *ctx, const char *buf, size_t len);

/* Contexte d'E/S avec un buffer interne pour la lecture par lignes */
struct io_context {
    read_cb read_fn;
    write_cb write_fn;
    void *ctx;           /* Contexte utilisateur (ex: pointeur vers fd) */
    char buf[4096];
    size_t buf_pos;
    size_t buf_len;
};

void io_context_init(struct io_context *io, read_cb r, write_cb w, void *ctx);

/* Lit une ligne complète se terminant par \n */
int session_read_line(struct io_context *io, char *line_out, size_t max_len);

/* Lit une réponse SMTP complète (gère les lignes multiples). Retourne le code. */
int session_read_reply(struct io_context *io, int *out_code);

/* Écrit exactement len octets */
int session_write_exact(struct io_context *io, const char *data);

/* Envoie une commande et ajoute le CRLF */
int session_send_command(struct io_context *io, const char *cmd);

/* Exécute la session SMTP complète selon le workflow défini */
int session_run(struct io_context *io, const char *from, const char *to, 
                const char *subject, const char *body, const char *helo_host);


/* ======================================================================
 * Layer 3: Socket Transport
 * ====================================================================== */

/* Connecte un socket à l'hôte et port fournis */
int socket_connect(const char *host, const char *port);

/* Callbacks compatibles avec io_context */
ssize_t socket_read_cb(void *ctx, char *buf, size_t len);
ssize_t socket_write_cb(void *ctx, const char *buf, size_t len);

#endif // LAB_H