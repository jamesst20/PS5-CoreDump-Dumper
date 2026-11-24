/* SPDX-License-Identifier: MIT */
/*
 * ampr-emu-ng debug transport: a tiny localhost HTTP client for the debug
 * build's port 9090, the black-box trace, and a ZIP with all of them.
 */
#include "web_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "web_util.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define AMPR_PORT 9090
#define MAX_HTTP_BODY (8u * 1024u * 1024u)
#define LOG_READ_MAX (4u * 1024u * 1024u)
#define ZIP_MAX_ENTRIES 16

typedef struct {
  int status;
  char type[96];
  char *body;
  size_t length;
} http_result_t;

typedef struct {
  char name[64];
  uint32_t crc;
  uint32_t size;
  uint32_t offset;
} zip_entry_t;

static size_t header_length_value(const char *headers, const char *name) {
  size_t name_length = strlen(name);
  const char *line = headers;

  while (*line) {
    const char *next = strstr(line, "\r\n");
    size_t length = next ? (size_t)(next - line) : strlen(line);

    if (length > name_length && !strncasecmp(line, name, name_length) && line[name_length] == ':') {
      return (size_t)strtoll(line + name_length + 1, NULL, 10);
    }
    if (!next) {
      break;
    }
    line = next + 2;
  }
  return 0;
}

static void header_text_value(const char *headers, const char *name, char *out, size_t cap) {
  size_t name_length = strlen(name);
  const char *line = headers;

  out[0] = '\0';
  while (*line) {
    const char *next = strstr(line, "\r\n");
    size_t length = next ? (size_t)(next - line) : strlen(line);

    if (length > name_length && !strncasecmp(line, name, name_length) && line[name_length] == ':') {
      const char *value = line + name_length + 1;
      while (value < line + length && (*value == ' ' || *value == '\t')) {
        value++;
      }
      size_t value_length = (size_t)(line + length - value);
      if (value_length >= cap) {
        value_length = cap - 1;
      }
      memcpy(out, value, value_length);
      out[value_length] = '\0';
      return;
    }
    if (!next) {
      break;
    }
    line = next + 2;
  }
}

static bool http_get(unsigned short port, const char *path, http_result_t *result, int timeout_ms) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address;
  struct timeval timeout;
  web_buf_t request = {0};
  web_buf_t raw = {0};
  char chunk[16384];
  bool ok = false;

  memset(result, 0, sizeof(*result));
  if (fd < 0) {
    return false;
  }
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
    close(fd);
    return false;
  }

  web_buf_printf(&request, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
  if (!request.failed) {
    size_t sent = 0;
    while (sent < request.len) {
      ssize_t written = send(fd, request.data + sent, request.len - sent, 0);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        break;
      }
      sent += (size_t)written;
    }
    if (sent == request.len) {
      for (;;) {
        ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0 && errno == EINTR) {
          continue;
        }
        if (got <= 0) {
          break;
        }
        web_buf_add(&raw, chunk, (size_t)got);
        if (raw.failed || raw.len >= MAX_HTTP_BODY) {
          break;
        }
      }
    }
  }
  free(request.data);
  close(fd);

  if (raw.len) {
    char *end = strstr(raw.data, "\r\n\r\n");
    if (end) {
      size_t header_len = (size_t)(end - raw.data) + 4;
      size_t available = raw.len - header_len;
      size_t declared = header_length_value(raw.data, "Content-Length");
      size_t body_length = declared && declared < available ? declared : available;

      result->status = atoi(raw.data + 9);
      header_text_value(raw.data, "Content-Type", result->type, sizeof(result->type));
      if (!result->type[0]) {
        snprintf(result->type, sizeof(result->type), "text/plain; charset=utf-8");
      }
      result->body = malloc(body_length + 1);
      if (result->body) {
        memcpy(result->body, raw.data + header_len, body_length);
        result->body[body_length] = '\0';
        result->length = body_length;
        ok = true;
      }
    }
  }
  free(raw.data);
  return ok;
}

/* Fire and forget: /crash kills the game before it answers. */
static void http_fire(unsigned short port, const char *path) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address;
  char request[256];
  int length;

  if (fd < 0) {
    return;
  }
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
    length = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
    if (length > 0) {
      send(fd, request, (size_t)length, 0);
    }
  }
  close(fd);
}

static bool read_file(const char *path, size_t cap, web_buf_t *out) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  struct stat statbuf;
  char chunk[65536];

  if (fd < 0) {
    return false;
  }
  if (fstat(fd, &statbuf) || !S_ISREG(statbuf.st_mode)) {
    close(fd);
    return false;
  }
  if ((size_t)statbuf.st_size > cap) {
    web_buf_puts(out, "[... earlier content truncated ...]\n\n");
    lseek(fd, statbuf.st_size - (off_t)cap, SEEK_SET);
  }
  for (;;) {
    ssize_t got = read(fd, chunk, sizeof(chunk));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      break;
    }
    web_buf_add(out, chunk, (size_t)got);
    if (out->failed) {
      break;
    }
  }
  close(fd);
  return true;
}

static void append_file_json(web_buf_t *json, const char *path) {
  struct stat statbuf;

  web_buf_puts(json, "{\"path\":");
  web_buf_json(json, path);
  if (!stat(path, &statbuf) && S_ISREG(statbuf.st_mode)) {
    web_buf_printf(json, ",\"exists\":true,\"bytes\":%lld,\"mtime\":%lld}", (long long)statbuf.st_size,
                   (long long)statbuf.st_mtime);
  } else {
    web_buf_puts(json, ",\"exists\":false,\"bytes\":0,\"mtime\":0}");
  }
}

static void first_line(const char *text, char *out, size_t cap) {
  size_t i = 0;

  while (text[i] && text[i] != '\n' && text[i] != '\r' && i + 1 < cap) {
    out[i] = text[i];
    i++;
  }
  out[i] = '\0';
}

static bool require_csrf(int fd, bool csrf) {
  if (csrf) {
    return true;
  }
  web_send_text(fd, 403, "Forbidden", "missing X-Dumper-Debug: 1 header\n");
  return false;
}

static void debug_status(int fd) {
  web_buf_t json = {0};
  char build[256] = "";
  http_result_t probe = {0};
  bool ampr = http_get(AMPR_PORT, "/metrics", &probe, 1500);

  if (ampr) {
    first_line(probe.body, build, sizeof(build));
  }
  free(probe.body);

  web_buf_puts(&json, "{\"ampr\":{\"reachable\":");
  web_buf_puts(&json, ampr ? "true" : "false");
  web_buf_puts(&json, ",\"build\":");
  web_buf_json(&json, build);
  web_buf_puts(&json, "},\"trace\":");
  append_file_json(&json, "/data/ampr_emu_trace.txt");
  web_buf_puts(&json, ",\"tune\":");
  append_file_json(&json, "/data/ampr_emu_tune.txt");
  web_buf_puts(&json, "}");
  web_send_buffer(fd, 200, "OK", "application/json", &json);
}

static void debug_ampr(int fd, const char *query) {
  static const char *const pages[] = {"metrics", "fsfail", "modules", "threads", "queues", "set"};
  char page[32];
  const char *path = NULL;
  http_result_t result = {0};

  if (!web_query_value(query, "page", page, sizeof(page))) {
    web_send_text(fd, 400, "Bad Request", "missing page\n");
    return;
  }
  for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
    if (!strcmp(page, pages[i])) {
      path = pages[i];
      break;
    }
  }
  if (!path) {
    web_send_text(fd, 400, "Bad Request", "unknown page\n");
    return;
  }
  char target[64];
  snprintf(target, sizeof(target), "/%s", path);
  if (!http_get(AMPR_PORT, target, &result, 2500)) {
    web_send_text(fd, 502, "Bad Gateway", "ampr debug API not reachable (game not running or release build)\n");
    return;
  }
  web_send_reply(fd, 200, "OK", "text/plain; charset=utf-8", NULL, result.body, result.length);
  free(result.body);
}

static void debug_local_file(int fd, const char *path, const char *missing) {
  web_buf_t text = {0};

  if (!read_file(path, LOG_READ_MAX, &text) || (!text.len && !text.failed)) {
    web_buf_puts(&text, missing);
  }
  web_send_buffer(fd, 200, "OK", "text/plain; charset=utf-8", &text);
}

static void debug_crash(int fd) {
  http_result_t probe = {0};

  if (!http_get(AMPR_PORT, "/metrics", &probe, 1200)) {
    web_send_text(fd, 502, "Bad Gateway", "ampr debug API not reachable\n");
    return;
  }
  free(probe.body);
  http_fire(AMPR_PORT, "/crash");
  web_buf_t json = {0};
  web_buf_puts(&json, "{\"ok\":true}");
  web_send_buffer(fd, 202, "Accepted", "application/json", &json);
}

static uint32_t crc_bytes(const char *data, size_t length) {
  static const uint32_t table[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                     0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                     0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  uint32_t crc = 0xffffffffu;

  for (size_t i = 0; i < length; i++) {
    crc ^= (unsigned char)data[i];
    crc = (crc >> 4) ^ table[crc & 0xf];
    crc = (crc >> 4) ^ table[crc & 0xf];
  }
  return crc ^ 0xffffffffu;
}

static void put16(web_buf_t *buf, uint16_t value) {
  unsigned char bytes[2] = {(unsigned char)value, (unsigned char)(value >> 8)};
  web_buf_add(buf, bytes, sizeof(bytes));
}

static void put32(web_buf_t *buf, uint32_t value) {
  unsigned char bytes[4] = {(unsigned char)value, (unsigned char)(value >> 8), (unsigned char)(value >> 16),
                            (unsigned char)(value >> 24)};
  web_buf_add(buf, bytes, sizeof(bytes));
}

static bool zip_add(web_buf_t *zip, zip_entry_t *entries, int *count, const char *name, const char *data, size_t length) {
  struct tm local;
  time_t now = time(NULL);
  uint32_t crc;
  uint32_t offset;
  uint16_t name_length;
  uint16_t dos_time;
  uint16_t dos_date;

  if (*count >= ZIP_MAX_ENTRIES || length > 0xffffffffu || strlen(name) >= sizeof(entries[0].name)) {
    return false;
  }
  localtime_r(&now, &local);
  dos_date = (uint16_t)(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
  dos_time = (uint16_t)((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
  crc = crc_bytes(data ? data : "", length);
  offset = (uint32_t)zip->len;
  name_length = (uint16_t)strlen(name);

  put32(zip, 0x04034b50u);
  put16(zip, 20);
  put16(zip, 0);
  put16(zip, 0);
  put16(zip, dos_time);
  put16(zip, dos_date);
  put32(zip, crc);
  put32(zip, (uint32_t)length);
  put32(zip, (uint32_t)length);
  put16(zip, name_length);
  put16(zip, 0);
  web_buf_add(zip, name, name_length);
  if (length) {
    web_buf_add(zip, data, length);
  }

  strlcpy(entries[*count].name, name, sizeof(entries[*count].name));
  entries[*count].crc = crc;
  entries[*count].size = (uint32_t)length;
  entries[*count].offset = offset;
  (*count)++;
  return !zip->failed;
}

static void zip_finish(web_buf_t *zip, const zip_entry_t *entries, int count) {
  uint32_t start = (uint32_t)zip->len;

  for (int i = 0; i < count; i++) {
    put32(zip, 0x02014b50u);
    put16(zip, 20);
    put16(zip, 20);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, 0);
    put32(zip, entries[i].crc);
    put32(zip, entries[i].size);
    put32(zip, entries[i].size);
    put16(zip, (uint16_t)strlen(entries[i].name));
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, 0);
    put32(zip, 0);
    put32(zip, entries[i].offset);
    web_buf_add(zip, entries[i].name, strlen(entries[i].name));
  }
  uint32_t cd_size = (uint32_t)zip->len - start;
  put32(zip, 0x06054b50u);
  put16(zip, 0);
  put16(zip, 0);
  put16(zip, (uint16_t)count);
  put16(zip, (uint16_t)count);
  put32(zip, cd_size);
  put32(zip, start);
  put16(zip, 0);
}

static void zip_add_file(web_buf_t *zip, zip_entry_t *entries, int *count, const char *name, const char *path) {
  web_buf_t file = {0};

  if (read_file(path, LOG_READ_MAX, &file)) {
    zip_add(zip, entries, count, name, file.data ? file.data : "", file.len);
  } else {
    static const char note[] = "(not available)\n";
    zip_add(zip, entries, count, name, note, sizeof(note) - 1);
  }
  free(file.data);
}

static void debug_logs(int fd) {
  static const char *const pages[][2] = {{"ampr_metrics.txt", "/metrics"}, {"ampr_fsfail.txt", "/fsfail"},
                                         {"ampr_modules.txt", "/modules"}, {"ampr_threads.txt", "/threads"},
                                         {"ampr_queues.txt", "/queues"},   {"ampr_set.txt", "/set"}};
  web_buf_t zip = {0};
  zip_entry_t entries[ZIP_MAX_ENTRIES];
  int count = 0;
  char extra[128];

  for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
    http_result_t result = {0};

    if (http_get(AMPR_PORT, pages[i][1], &result, 2500)) {
      zip_add(&zip, entries, &count, pages[i][0], result.body, result.length);
      free(result.body);
    } else {
      static const char note[] = "(ampr debug API not reachable: is the debug build loaded and the game running?)\n";
      zip_add(&zip, entries, &count, pages[i][0], note, sizeof(note) - 1);
    }
  }
  zip_add_file(&zip, entries, &count, "ampr_emu_trace.txt", "/data/ampr_emu_trace.txt");
  zip_add_file(&zip, entries, &count, "ampr_emu_tune.txt", "/data/ampr_emu_tune.txt");
  zip_finish(&zip, entries, count);

  if (zip.failed) {
    web_send_text(fd, 500, "Internal Server Error", "cannot build the log archive\n");
    free(zip.data);
    return;
  }
  snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"ampr-emu-ng-logs.zip\"\r\n");
  web_send_reply(fd, 200, "OK", "application/zip", extra, zip.data, zip.len);
  free(zip.data);
}

void web_debug_route(int fd, const char *method, const char *path, const char *query, bool csrf) {
  bool get = !strcmp(method, "GET");
  bool post = !strcmp(method, "POST");

  if (get && !strcmp(path, "/api/debug/status")) {
    debug_status(fd);
  } else if (get && !strcmp(path, "/api/debug/ampr")) {
    debug_ampr(fd, query);
  } else if (get && !strcmp(path, "/api/debug/trace")) {
    debug_local_file(fd, "/data/ampr_emu_trace.txt", "No ampr_emu_trace.txt captured yet.\n");
  } else if (get && !strcmp(path, "/api/debug/tune")) {
    debug_local_file(fd, "/data/ampr_emu_tune.txt", "No custom tune set.\n");
  } else if (get && !strcmp(path, "/api/debug/logs.zip")) {
    debug_logs(fd);
  } else if (post && !strcmp(path, "/api/debug/crash")) {
    if (require_csrf(fd, csrf)) {
      debug_crash(fd);
    }
  } else {
    web_send_text(fd, 404, "Not Found", "not found\n");
  }
}
