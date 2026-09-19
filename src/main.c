#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "lab.h"

void print_usage() {
    printf("Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port] [-H helo-host] <server>\n");
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        print_usage();
        return 0; // Exigence : exit 0 quand lancé sans argument
    }

    char *from = NULL;
    char *to = NULL;
    char *subject = "";
    char *body = NULL;
    char *port = "25";
    char *helo_host = "localhost";
    int body_allocated = 0;

    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:")) != -1) {
        switch (opt) {
            case 'f': from = optarg; break;
            case 't': to = optarg; break;
            case 's': subject = optarg; break;
            case 'b': body = optarg; break;
            case 'p': port = optarg; break;
            case 'H': helo_host = optarg; break;
            default:
                print_usage();
                return 1;
        }
    }

    if (optind >= argc || !from || !to) {
        fprintf(stderr, "Erreur : Paramètres manquants.\n");
        print_usage();
        return 1;
    }
    char *server = argv[optind];

    /* Task 1: Rejeter bare CR ou LF dans les arguments (Injection Check) */
    if (check_injection(from) || check_injection(to) || check_injection(subject)) {
        fprintf(stderr, "Erreur : Tentative d'injection (CR/LF détecté dans un header).\n");
        return 1;
    }

    /* Si pas de body, lecture depuis stdin (Task 1) */
    if (!body) {
        size_t cap = 1024, len = 0;
        body = malloc(cap);
        int c;
        while ((c = fgetc(stdin)) != EOF) {
            if (len + 1 >= cap) {
                cap *= 2;
                body = realloc(body, cap);
            }
            body[len++] = c;
        }
        body[len] = '\0';
        body_allocated = 1;
    }

    int fd = socket_connect(server, port);
    if (fd < 0) {
        fprintf(stderr, "Erreur de connexion a %s:%s\n", server, port);
        if (body_allocated) free(body);
        return 2;
    }

    struct io_context io;
    io_context_init(&io, socket_read_cb, socket_write_cb, &fd);

    int status = session_run(&io, from, to, subject, body, helo_host);

    close(fd);
    if (body_allocated) free(body);

    return status; // 0 succès, 2 erreur réseau/SMTP
}