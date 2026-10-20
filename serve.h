#ifndef SERVE_H
#define SERVE_H

typedef struct {
    char title[128];
    char description[512];
    char root[1024];
    char server[32];
    char command[2048];
    char exposure[16];
    unsigned port;
} ServeConfig;

int parse_serve(const char *filename, ServeConfig *cfg);
int run_serve(const char *filename);

#endif
