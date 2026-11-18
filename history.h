#ifndef SERVE_HISTORY_H
#define SERVE_HISTORY_H

int history_record(unsigned port, const char *short_title,
                   const char *title, const char *activity, const char *result);
int history_show(void);

#endif
