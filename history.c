#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int history_path(char *dest, size_t cap)
{
    const char *base = getenv("XDG_STATE_HOME");
    char fallback[PATH_MAX];
    if (!base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            fputs("HOME must be an absolute directory\n", stderr);
            return 1;
        }
        int n = snprintf(fallback, sizeof fallback, "%s/.local/state", home);
        if (n < 0 || (size_t)n >= sizeof fallback) return 1;
        base = fallback;
    }
    int n = snprintf(dest, cap, "%s/serve/history.tsv", base);
    return n < 0 || (size_t)n >= cap;
}

static int ensure_parent(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash) return 1;
    *slash = '\0';
    for (char *p = path + 1; ; ++p) {
        if (*p != '/' && *p != '\0') continue;
        char save = *p;
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            perror(path);
            *p = save;
            return 1;
        }
        struct stat st;
        if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "Not a directory: %s\n", path);
            *p = save;
            return 1;
        }
        *p = save;
        if (!save) break;
    }
    *slash = '/';
    return 0;
}

/* Keep the journal as one physical line per event. */
static void clean_field(char *dest, size_t cap, const char *src)
{
    size_t i = 0;
    if (!src || !*src) src = "-";
    while (*src && i + 1 < cap) {
        unsigned char c = (unsigned char)*src++;
        dest[i++] = (c < 32 || c == 127) ? ' ' : (char)c;
    }
    dest[i] = '\0';
}

int history_record(unsigned port, const char *short_title,
                   const char *title, const char *activity, const char *result)
{
    char path[PATH_MAX];
    if (history_path(path, sizeof path) || ensure_parent(path)) return 1;
    time_t now = time(NULL);
    struct tm tm;
    char timestamp[48] = "unknown";
    if (now != (time_t)-1 && localtime_r(&now, &tm))
        (void)strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%S%z", &tm);
    char safe_short[32], safe_title[128], safe_activity[32], safe_result[32];
    clean_field(safe_short, sizeof safe_short, short_title);
    clean_field(safe_title, sizeof safe_title, title);
    clean_field(safe_activity, sizeof safe_activity, activity);
    clean_field(safe_result, sizeof safe_result, result);
    char line[512];
    int n = snprintf(line, sizeof line, "%s\t%u\t%s\t%s\t%s\t%s\n",
                     timestamp, port, safe_short, safe_title, safe_activity, safe_result);
    if (n < 0 || (size_t)n >= sizeof line) return 1;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { perror("open history"); return 1; }
    int failure = flock(fd, LOCK_EX) != 0;
    if (!failure) {
        size_t left = (size_t)n;
        const char *at = line;
        while (left) {
            ssize_t written = write(fd, at, left);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { failure = 1; break; }
            at += written;
            left -= (size_t)written;
        }
        if (flock(fd, LOCK_UN)) failure = 1;
    }
    if (close(fd)) failure = 1;
    if (failure) fputs("Unable to record serve history\n", stderr);
    return failure;
}

int history_show(void)
{
    char path[PATH_MAX];
    if (history_path(path, sizeof path)) return 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) { puts("No history recorded."); return 0; }
        perror("open history");
        return 1;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { perror("fdopen"); close(fd); return 1; }
    printf("%-24s %-6s %-20s %-28s %-10s %s\n",
           "WHEN", "PORT", "SHORT TITLE", "TITLE", "ACTIVITY", "RESULT");
    char *line = NULL;
    size_t cap = 0;
    size_t rows = 0;
    while (getline(&line, &cap, f) != -1) {
        char *fields[6], *save = NULL;
        char *field = strtok_r(line, "\t\n", &save);
        int i = 0;
        while (field && i < 6) {
            fields[i++] = field;
            field = strtok_r(NULL, "\t\n", &save);
        }
        if (i != 6 || field) continue; /* ignore incomplete records */
        printf("%-24s %-6s %-20s %-28s %-10s %s\n",
               fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]);
        rows++;
    }
    int failure = ferror(f);
    free(line);
    if (fclose(f)) failure = 1;
    if (!rows) puts("No history recorded.");
    return failure;
}
