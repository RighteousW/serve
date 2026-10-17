#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char title[128];
    char description[512];
    char root[1024];
    char server[32];
    char exposure[16];
    unsigned port;
} ServeConfig;

static void help(const char *program)
{
    printf("Usage:\n"
           "  %s --discover [directory]\n"
           "  %s <file.serve>\n"
           "  %s --help\n", program, program, program);
}

static int discover(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        perror(path);
        return 1;
    }
    struct dirent *entry;
    int error = 0;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) { perror("readdir"); error = 1; }
            break;
        }
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".serve") != 0) continue;
        struct stat st;
        if (fstatat(dirfd(dir), entry->d_name, &st, 0) == 0 &&
            S_ISREG(st.st_mode))
            printf("%s/%s\n", path, entry->d_name);
    }
    if (closedir(dir) != 0) { perror("closedir"); error = 1; }
    return error;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static int copy_value(char *dst, size_t cap, const char *value)
{
    size_t length = strlen(value);
    if (length == 0 || length >= cap) return -1;
    memcpy(dst, value, length + 1);
    return 0;
}

static int parse(const char *filename, ServeConfig *cfg)
{
    static const char *keys[] = {
        "title", "description", "root", "server", "exposure", "port"
    };
    unsigned seen = 0;
    memset(cfg, 0, sizeof *cfg);
    FILE *file = fopen(filename, "r");
    if (!file) { perror(filename); return 1; }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t count;
    unsigned lineno = 0;
    int error = 0;
    while ((count = getline(&line, &capacity, file)) != -1) {
        (void)count;
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *equal = strchr(s, '=');
        if (!equal) { error = 1; break; }
        *equal = '\0';
        char *key = trim(s);
        char *value = trim(equal + 1);
        int index = -1;
        for (int i = 0; i < 6; i++) {
            if (strcmp(key, keys[i]) == 0) { index = i; break; }
        }
        if (index < 0 || (seen & (1u << index)) || !*value) {
            error = 1;
            break;
        }
        seen |= 1u << index;
        switch (index) {
            case 0: error = copy_value(cfg->title, sizeof cfg->title, value); break;
            case 1: error = copy_value(cfg->description, sizeof cfg->description, value); break;
            case 2: error = copy_value(cfg->root, sizeof cfg->root, value); break;
            case 3: error = copy_value(cfg->server, sizeof cfg->server, value); break;
            case 4: error = copy_value(cfg->exposure, sizeof cfg->exposure, value); break;
            case 5: {
                char *end;
                errno = 0;
                long n = strtol(value, &end, 10);
                if (errno || *end || n < 1 || n > 65535) error = 1;
                else cfg->port = (unsigned)n;
                break;
            }
        }
        if (error) break;
    }
    if (!error && ferror(file)) error = 1;
    if (fclose(file) != 0) error = 1;
    free(line);
    if (error) {
        fprintf(stderr, "Invalid .serve file at line %u\n", lineno);
        return 1;
    }
    if (seen != (1u << 6) - 1u) {
        fprintf(stderr, "Missing required field(s) in %s\n", filename);
        return 1;
    }
    if (strcmp(cfg->server, "npm-dev") &&
        strcmp(cfg->server, "npm-preview") &&
        strcmp(cfg->server, "python-http")) {
        fprintf(stderr, "Unsupported server: %s\n", cfg->server);
        return 1;
    }
    if (strcmp(cfg->exposure, "local") &&
        strcmp(cfg->exposure, "lan") &&
        strcmp(cfg->exposure, "wan") &&
        strcmp(cfg->exposure, "internet")) {
        fprintf(stderr, "Unsupported exposure: %s\n", cfg->exposure);
        return 1;
    }
    return 0;
}

static int serve(const char *filename)
{
    ServeConfig cfg;
    if (parse(filename, &cfg)) return 1;

    char *absolute = realpath(filename, NULL);
    if (!absolute) { perror(filename); return 1; }
    char *slash = strrchr(absolute, '/');
    if (slash == absolute) slash[1] = '\0';
    else *slash = '\0';
    int result = chdir(absolute);
    free(absolute);
    if (result != 0 || chdir(cfg.root) != 0) {
        perror("root");
        return 1;
    }

    const char *host = strcmp(cfg.exposure, "local") == 0
                       ? "127.0.0.1" : "0.0.0.0";
    if (strcmp(cfg.exposure, "local") != 0)
        fprintf(stderr, "WARNING: Binding all IPv4 interfaces; %s-only access is not enforced.\n"
                        "Use firewall/router/proxy rules to control actual reachability.\n",
                cfg.exposure);

    char port[6];
    snprintf(port, sizeof port, "%u", cfg.port);
    printf("Serving %s (%s) on %s:%s\n", cfg.title, cfg.description, host, port);
    fflush(stdout);

    if (strcmp(cfg.server, "python-http") == 0) {
        char *const args[] = {"python3", "-m", "http.server", port,
                              "--bind", (char *)host, NULL};
        execvp(args[0], args);
    } else {
        char *const args[] = {"npm", "run",
                              strcmp(cfg.server, "npm-dev") == 0 ? "dev" : "preview",
                              "--", "--host", (char *)host,
                              "--port", port, "--strictPort", NULL};
        execvp(args[0], args);
    }
    perror("execvp");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        help(argv[0]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--discover") == 0) {
        if (argc > 3 || (argc == 3 && argv[2][0] == '-')) {
            fprintf(stderr, "Invalid discover arguments.\n");
            help(argv[0]);
            return 1;
        }
        return discover(argc == 3 ? argv[2] : ".");
    }
    if (argc == 2 && argv[1][0] != '-') {
        const char *ext = strrchr(argv[1], '.');
        if (ext && strcmp(ext, ".serve") == 0) return serve(argv[1]);
    }
    fprintf(stderr, "Invalid arguments.\n");
    help(argv[0]);
    return 1;
}
