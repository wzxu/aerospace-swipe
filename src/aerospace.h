#ifndef AEROSPACE_H
#define AEROSPACE_H

#include <stddef.h>
#include <sys/types.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Aerospace Aerospace;

Aerospace *aerospace_new(const char *socketPath);

int aerospace_is_initialized(Aerospace *client);

ssize_t aerospace_send(Aerospace *client, cJSON *query);

char *aerospace_receive(Aerospace *client, size_t maxBytes);

void aerospace_close(Aerospace *client);

char *aerospace_switch(Aerospace *client, const char *direction);

char *aerospace_workspace(Aerospace *client, int wrap, const char *ws, const char *in);

char *aerospace_list_workspaces(Aerospace *client);

#ifdef __cplusplus
}
#endif

#endif
