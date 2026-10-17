#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "manager.h"
#include "serve.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int port_number(const char *text, unsigned *port)
{
    if (!text || !*text) return 1;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return 1;
    errno = 0;
    char *end;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n == 0 || n > 65535) return 1;
    char canonical[6];
    snprintf(canonical, sizeof canonical, "%lu", n);
    if (strcmp(text, canonical) != 0) return 1;
    *port = (unsigned)n;
    return 0;
}

static int state_dir(char *dest, size_t size)
{
    const char *base = getenv("XDG_STATE_HOME");
    char fallback[PATH_MAX];
    if (!base || base[0] != '/') {
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            fprintf(stderr, "HOME is not an absolute path\n");
            return 1;
        }
        int n = snprintf(fallback, sizeof fallback, "%s/.local/state", home);
        if (n < 0 || (size_t)n >= sizeof fallback) return 1;
        base = fallback;
    }
    int n = snprintf(dest, size, "%s/serve", base);
    return n < 0 || (size_t)n >= size;
}

static int make_dirs(const char *path)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof copy) return 1;
    strcpy(copy, path);
    for (char *p = copy + 1; ; ++p) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
                perror(copy);
                return 1;
            }
            struct stat st;
            if (stat(copy, &st) != 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "%s is not a directory\n", copy);
                return 1;
            }
            *p = saved;
            if (!saved) break;
        }
    }
    return 0;
}

static int state_path(char *dest, size_t size, unsigned port, const char *suffix)
{
    char dir[PATH_MAX];
    if (state_dir(dir, sizeof dir)) return 1;
    int n = snprintf(dest, size, "%s/%u.%s", dir, port, suffix);
    return n < 0 || (size_t)n >= size;
}

static int registered_path(unsigned port, char *dest, size_t size)
{
    char path[PATH_MAX];
    if (state_path(path, sizeof path, port, "path")) return 1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 1;
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return 1; }
    int okay = fgets(dest, (int)size, file) != NULL;
    if (okay) {
        size_t n = strlen(dest);
        if (!n || dest[n - 1] != '\n') okay = 0;
        else dest[n - 1] = '\0';
    }
    if (fclose(file) != 0) okay = 0;
    return okay ? 0 : 1;
}

static int write_registration(unsigned port, const char *file)
{
    char dir[PATH_MAX], dest[PATH_MAX];
    if (state_dir(dir, sizeof dir) || make_dirs(dir) ||
        state_path(dest, sizeof dest, port, "path")) return 1;
    int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("Register service (port already registered?)");
        return 1;
    }
    size_t len = strlen(file);
    int okay = write(fd, file, len) == (ssize_t)len && write(fd, "\n", 1) == 1;
    if (close(fd) != 0) okay = 0;
    if (!okay) {
        perror("Write registration");
        unlink(dest);
        return 1;
    }
    return 0;
}

static int paused(unsigned port)
{
    char path[PATH_MAX];
    if (state_path(path, sizeof path, port, "paused")) return 0;
    return access(path, F_OK) == 0;
}

static int pause_marker(unsigned port)
{
    char path[PATH_MAX];
    if (state_path(path, sizeof path, port, "paused")) return 1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { perror("Pause marker"); return 1; }
    return close(fd) != 0;
}

static void remove_marker(unsigned port)
{
    char path[PATH_MAX];
    if (!state_path(path, sizeof path, port, "paused")) unlink(path);
}

static int unit_command(unsigned port, const char *action, int quiet, char *out, size_t out_size)
{
    char unit[32];
    snprintf(unit, sizeof unit, "serve@%u.service", port);
    const char *signal_arg = !strcmp(action, "pause") ? "--signal=SIGSTOP" :
                             !strcmp(action, "resume") ? "--signal=SIGCONT" : NULL;
    char *args[10];
    int i = 0;
    args[i++] = "systemctl";
    args[i++] = "--user";
    if (signal_arg) {
        args[i++] = "kill";
        args[i++] = "--kill-whom=all";
        args[i++] = (char *)signal_arg;
    } else if (!strcmp(action, "pid")) {
        args[i++] = "show";
        args[i++] = "--property=MainPID";
        args[i++] = "--value";
    } else {
        args[i++] = (char *)action;
        if (quiet) args[i++] = "--quiet";
    }
    args[i++] = unit;
    args[i] = NULL;

    int pipefd[2] = {-1, -1};
    if (out && pipe(pipefd) < 0) { perror("pipe"); return 1; }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        if (out) { close(pipefd[0]); close(pipefd[1]); }
        return 1;
    }
    if (child == 0) {
        if (out) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
            close(pipefd[1]);
        } else if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)dup2(devnull, STDOUT_FILENO);
                (void)dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(args[0], args);
        perror("systemctl");
        _exit(127);
    }
    if (out) {
        close(pipefd[1]);
        size_t used = 0;
        while (used + 1 < out_size) {
            ssize_t n = read(pipefd[0], out + used, out_size - used - 1);
            if (n == 0) break;
            if (n < 0) { if (errno == EINTR) continue; break; }
            used += (size_t)n;
        }
        if (out_size) out[used] = '\0';
        close(pipefd[0]);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return 1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

static const char *service_state(unsigned port)
{
    if (!unit_command(port, "is-active", 1, NULL, 0))
        return paused(port) ? "paused" : "running";
    if (!unit_command(port, "is-failed", 1, NULL, 0)) return "failed";
    return "inactive";
}

int manager_start(const char *filename)
{
    ServeConfig cfg;
    if (parse_serve(filename, &cfg)) return 1;
    char *absolute = realpath(filename, NULL);
    if (!absolute) { perror(filename); return 1; }
    if (strchr(absolute, '\n') || strchr(absolute, '\r')) {
        fprintf(stderr, "Configuration path cannot contain a newline\n");
        free(absolute);
        return 1;
    }
    int result = write_registration(cfg.port, absolute);
    free(absolute);
    if (result) return 1;
    printf("Starting %s on port %u ...\n", cfg.title, cfg.port);
    fflush(stdout);
    if (unit_command(cfg.port, "start", 0, NULL, 0)) {
        fprintf(stderr, "Could not start serve@%u.service. Try --status %u or journalctl --user -u serve@%u.service\n",
                cfg.port, cfg.port, cfg.port);
        return 1;
    }
    printf("Registered serve@%u.service\n", cfg.port);
    return 0;
}

int manager_run(const char *port_text)
{
    unsigned port;
    if (port_number(port_text, &port)) return 1;
    char file[PATH_MAX];
    if (registered_path(port, file, sizeof file)) {
        fprintf(stderr, "No registered .serve file for port %u\n", port);
        return 1;
    }
    ServeConfig cfg;
    if (parse_serve(file, &cfg) || cfg.port != port) {
        fprintf(stderr, "Registered configuration does not match port %u\n", port);
        return 1;
    }
    return run_serve(file);
}

static int show_status(unsigned port)
{
    char filename[PATH_MAX];
    if (registered_path(port, filename, sizeof filename)) {
        fprintf(stderr, "No registered service on port %u\n", port);
        return 1;
    }
    ServeConfig cfg;
    int valid = parse_serve(filename, &cfg) == 0;
    char pid[32] = "0";
    (void)unit_command(port, "pid", 0, pid, sizeof pid);
    pid[strcspn(pid, "\n")] = '\0';
    printf("Unit:         serve@%u.service\n", port);
    printf("State:        %s\n", service_state(port));
    printf("Main PID:     %s\n", pid);
    printf("Configuration:%s\n", filename);
    if (valid) {
        printf("Title:        %s\n", cfg.title);
        printf("Description:  %s\n", cfg.description);
        printf("Root:         %s (relative to config)\n", cfg.root);
        printf("Server:       %s\n", cfg.server);
        printf("Exposure:     %s\n", cfg.exposure);
        printf("Port:         %u\n", cfg.port);
        if (cfg.port != port)
            fprintf(stderr, "Warning: configuration port differs from registered port\n");
    } else {
        fprintf(stderr, "Warning: configuration missing or invalid\n");
    }
    return 0;
}

int manager_status(const char *port_text)
{
    unsigned port;
    if (port_number(port_text, &port)) {
        fprintf(stderr, "Invalid port (expected 1-65535)\n");
        return 1;
    }
    return show_status(port);
}

int manager_list(void)
{
    char dirpath[PATH_MAX];
    if (state_dir(dirpath, sizeof dirpath)) return 1;
    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (errno == ENOENT) { puts("No registered services."); return 0; }
        perror(dirpath);
        return 1;
    }
    printf("%-7s %-10s %s\n", "PORT", "STATE", "TITLE");
    struct dirent *entry;
    int found = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".path")) continue;
        char number[16];
        size_t length = (size_t)(dot - entry->d_name);
        if (!length || length >= sizeof number) continue;
        memcpy(number, entry->d_name, length);
        number[length] = '\0';
        unsigned port;
        if (port_number(number, &port)) continue;
        char file[PATH_MAX];
        if (registered_path(port, file, sizeof file)) continue;
        ServeConfig cfg;
        const char *title = parse_serve(file, &cfg) ? "(config missing/invalid)" : cfg.title;
        printf("%-7u %-10s %s\n", port, service_state(port), title);
        found++;
    }
    if (closedir(dir) != 0) return 1;
    if (!found) puts("No registered services.");
    return 0;
}

static int require_registered(unsigned port)
{
    char file[PATH_MAX];
    if (!registered_path(port, file, sizeof file)) return 0;
    fprintf(stderr, "Port %u is not registered with serve\n", port);
    return 1;
}

int manager_pause(const char *port_text)
{
    unsigned port;
    if (port_number(port_text, &port) || require_registered(port)) return 1;
    if (paused(port)) { fprintf(stderr, "Port %u is already paused\n", port); return 1; }
    if (unit_command(port, "is-active", 1, NULL, 0)) {
        fprintf(stderr, "Port %u is not running\n", port);
        return 1;
    }
    if (unit_command(port, "pause", 0, NULL, 0)) return 1;
    if (pause_marker(port)) {
        (void)unit_command(port, "resume", 0, NULL, 0);
        return 1;
    }
    printf("Paused port %u (existing listening sockets remain bound)\n", port);
    return 0;
}

int manager_restart(const char *port_text)
{
    unsigned port;
    if (port_number(port_text, &port) || require_registered(port)) return 1;
    if (!paused(port) || unit_command(port, "is-active", 1, NULL, 0)) {
        fprintf(stderr, "Port %u is not a running, paused service\n", port);
        return 1;
    }
    if (unit_command(port, "resume", 0, NULL, 0)) return 1;
    remove_marker(port);
    printf("Resumed port %u\n", port);
    return 0;
}

int manager_stop(const char *port_text)
{
    unsigned port;
    if (port_number(port_text, &port) || require_registered(port)) return 1;
    if (paused(port)) (void)unit_command(port, "resume", 0, NULL, 0);
    if (unit_command(port, "stop", 0, NULL, 0)) return 1;
    (void)unit_command(port, "reset-failed", 1, NULL, 0);
    remove_marker(port);
    char path[PATH_MAX];
    if (state_path(path, sizeof path, port, "path") || unlink(path)) {
        perror("Remove registration");
        return 1;
    }
    printf("Stopped port %u and removed its registration\n", port);
    return 0;
}
