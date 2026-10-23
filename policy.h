#ifndef POLICY_H
#define POLICY_H
#include "serve.h"
typedef struct {
    int persist_boot;
    int persist_port;
    int auto_start_on_boot;
} ServePolicy;
int policy_bool(const char *text, int *value);
int policy_global(ServePolicy *policy);
int policy_effective(const ServeConfig *cfg, ServePolicy *policy);
int policy_config(int argc, const char *const *argv);
#endif
