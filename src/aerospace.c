#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "aerospace.h"
#include "cJSON.h"

#define DEFAULT_MAX_BUFFER_SIZE 2048
#define DEFAULT_EXTENDED_BUFFER_SIZE 4096

static const char *ERROR_SOCKET_CREATE = "Failed to create Unix domain socket";
static const char *ERROR_SOCKET_CONNECT_FMT =
    "Failed to connect to socket at %s";
static const char *ERROR_SOCKET_SEND = "Failed to send data through socket";
static const char *ERROR_SOCKET_RECEIVE = "Failed to receive data from socket";
static const char *ERROR_SOCKET_CLOSE = "Failed to close socket connection";
static const char *ERROR_SOCKET_NOT_CONN = "Socket is not connected";
static const char *ERROR_JSON_DECODE = "Failed to decode JSON response";

struct Aerospace {
  int fd;
  char *socket_path;
};

static void fatal_error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  va_end(args);
  exit(EXIT_FAILURE);
}

static ssize_t write_all(int fd, const char *buf, size_t count) {
  size_t total_written = 0;
  while (total_written < count) {
    ssize_t written = write(fd, buf + total_written, count - total_written);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    total_written += written;
  }
  return total_written;
}

static cJSON *decode_response(const char *response) {
  cJSON *json = cJSON_Parse(response);
  if (!json)
    fprintf(stderr, "%s: %s\n", ERROR_JSON_DECODE, cJSON_GetErrorPtr());
  return json;
}

static cJSON *perform_query(Aerospace *client, cJSON *query) {
  aerospace_send(client, query);
  /* The query object is deleted inside perform_query (via aerospace_send call)
   */
  char *response_str = aerospace_receive(client, DEFAULT_MAX_BUFFER_SIZE);
  cJSON *response_json = decode_response(response_str);
  free(response_str);
  return response_json;
}

static char *execute_workspace_command(Aerospace *client, const char *cmd,
                                       int wrap, const char *stdin_value) {
  cJSON *query = cJSON_CreateObject();
  if (!query)
    fatal_error("Memory allocation error");

  cJSON_AddStringToObject(query, "command", "");
  cJSON *args = cJSON_CreateArray();
  if (!args) {
    cJSON_Delete(query);
    fatal_error("Memory allocation error");
  }
  cJSON_AddItemToArray(args, cJSON_CreateString("workspace"));
  cJSON_AddItemToArray(args, cJSON_CreateString(cmd));
  if (wrap)
    cJSON_AddItemToArray(args, cJSON_CreateString("--wrap-around"));
  cJSON_AddItemToObject(query, "args", args);
  cJSON_AddStringToObject(query, "stdin", stdin_value);

  cJSON *response_json = perform_query(client, query);
  if (!response_json)
    return NULL;

  cJSON *exitCodeItem = cJSON_GetObjectItem(response_json, "exitCode");
  int exitCode = cJSON_IsNumber(exitCodeItem) ? exitCodeItem->valueint : -1;
  if (exitCode == 0) {
    cJSON_Delete(response_json);
    return NULL;
  } else {
    cJSON *stderr_item = cJSON_GetObjectItem(response_json, "stderr");
    if (!stderr_item || !cJSON_IsString(stderr_item)) {
      fprintf(stderr, "Response does not contain valid stderr\n");
      cJSON_Delete(response_json);
      return NULL;
    }
    char *stderr_str = strdup(stderr_item->valuestring);
    cJSON_Delete(response_json);
    return stderr_str;
  }
}

static char *get_default_socket_path(void) {
  uid_t uid = getuid();
  struct passwd *pw = getpwuid(uid);

  if (uid == 0) {
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user) {
      struct passwd *pw_temp = getpwnam(sudo_user);
      if (pw_temp)
        pw = pw_temp;
    } else {
      const char *user_env = getenv("USER");
      if (user_env && strcmp(user_env, "root") != 0) {
        struct passwd *pw_temp = getpwnam(user_env);
        if (pw_temp)
          pw = pw_temp;
      }
    }
  }

  if (!pw)
    fatal_error("Unable to determine user information for default socket path");

  const char *username = pw->pw_name;
  size_t len = snprintf(NULL, 0, "/tmp/bobko.aerospace-%s.sock", username);
  char *path = malloc(len + 1);
  if (!path)
    fatal_error("Memory allocation error");
  snprintf(path, len + 1, "/tmp/bobko.aerospace-%s.sock", username);
  return path;
}

Aerospace *aerospace_new(const char *socketPath) {
  Aerospace *client = malloc(sizeof(Aerospace));
  if (!client)
    fatal_error("Memory allocation error");

  if (socketPath) {
    client->socket_path = strdup(socketPath);
    if (!client->socket_path)
      fatal_error("Memory allocation error");
  } else {
    client->socket_path = get_default_socket_path();
  }

  client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (client->fd < 0) {
    free(client->socket_path);
    free(client);
    fatal_error("%s: %s", ERROR_SOCKET_CREATE, strerror(errno));
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, client->socket_path, sizeof(addr.sun_path) - 1);

  if (connect(client->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    char *used_path = strdup(client->socket_path);
    close(client->fd);
    free(client->socket_path);
    free(client);
    fatal_error(ERROR_SOCKET_CONNECT_FMT, used_path);
  }

  return client;
}

int aerospace_is_initialized(Aerospace *client) {
  return (client && client->fd >= 0);
}

ssize_t aerospace_send(Aerospace *client, cJSON *query) {
  if (!aerospace_is_initialized(client))
    fatal_error("%s", ERROR_SOCKET_NOT_CONN);

  char *json_str = cJSON_PrintUnformatted(query);
  if (!json_str)
    fatal_error("%s", ERROR_JSON_DECODE);

  size_t len = strlen(json_str);
  size_t total_len = len + 1;
  char *send_buf = malloc(total_len + 1);
  if (!send_buf) {
    free(json_str);
    fatal_error("Memory allocation error");
  }
  snprintf(send_buf, total_len + 1, "%s\n", json_str);
  free(json_str);

  ssize_t bytes_sent = write_all(client->fd, send_buf, total_len);
  if (bytes_sent < 0) {
    free(send_buf);
    fatal_error("%s: %s", ERROR_SOCKET_SEND, strerror(errno));
  }
  free(send_buf);
  return bytes_sent;
}

char *aerospace_receive(Aerospace *client, size_t maxBytes) {
  if (!aerospace_is_initialized(client))
    fatal_error("%s", ERROR_SOCKET_NOT_CONN);

  char *buffer = malloc(maxBytes + 1);
  if (!buffer)
    fatal_error("Memory allocation error");

  ssize_t bytes_read = read(client->fd, buffer, maxBytes);
  if (bytes_read < 0) {
    free(buffer);
    fatal_error("%s: %s", ERROR_SOCKET_RECEIVE, strerror(errno));
  }
  buffer[bytes_read] = '\0';
  return buffer;
}

void aerospace_close(Aerospace *client) {
  if (client) {
    if (client->fd >= 0) {
      if (close(client->fd) < 0)
        fprintf(stderr, "%s: %s\n", ERROR_SOCKET_CLOSE, strerror(errno));
      client->fd = -1;
    }
    free(client->socket_path);
    free(client);
  }
}

char *aerospace_switch(Aerospace *client, const char *direction) {
  return execute_workspace_command(client, direction, 0, "");
}

char *aerospace_workspace(Aerospace *client, int wrap, const char *ws,
                          const char *in) {
  return execute_workspace_command(client, ws, wrap, in);
}

char *aerospace_list_workspaces(Aerospace *client, bool empty) {
  cJSON *query = cJSON_CreateObject();
  if (!query)
    fatal_error("Memory allocation error");

  cJSON_AddStringToObject(query, "command", "");
  cJSON *args = cJSON_CreateArray();
  if (!args) {
    cJSON_Delete(query);
    fatal_error("Memory allocation error");
  }
  cJSON_AddItemToArray(args, cJSON_CreateString("list-workspaces"));
  cJSON_AddItemToArray(args, cJSON_CreateString("--monitor"));
  cJSON_AddItemToArray(args, cJSON_CreateString("focused"));
  if (empty) {
    cJSON_AddItemToArray(args, cJSON_CreateString("--empty"));
    cJSON_AddItemToArray(args, cJSON_CreateString("no"));
  }
  cJSON_AddItemToObject(query, "args", args);
  cJSON_AddStringToObject(query, "stdin", "");

  cJSON *response_json = perform_query(client, query);
  if (!response_json)
    return NULL;

  cJSON *stdout_item = cJSON_GetObjectItem(response_json, "stdout");
  if (!stdout_item || !cJSON_IsString(stdout_item)) {
    fprintf(stderr, "Response does not contain valid stdout\n");
    cJSON_Delete(response_json);
    return NULL;
  }
  char *result = strdup(stdout_item->valuestring);
  cJSON_Delete(response_json);
  return result;
}
