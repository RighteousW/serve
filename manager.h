#ifndef MANAGER_H
#define MANAGER_H

int manager_sync(void);
int manager_all(const char *action);
int manager_start(const char *filename);
int manager_run(const char *port);
int manager_list(void);
int manager_status(const char *port);
int manager_stop(const char *port);
int manager_pause(const char *port);
int manager_restart(const char *port);

#endif
