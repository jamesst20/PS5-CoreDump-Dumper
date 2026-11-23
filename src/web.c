/* SPDX-License-Identifier: MIT */
/*
 * Minimal HTTP server for the web UI: list, download and delete coredumps and
 * read the dumper and kernel logs. Every connection runs in its own detached
 * thread so a slow download never stalls the dumper.
 */
#include "web.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"
#include "fs.h"
#include "log.h"
#include "util.h"
#include "web_assets.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define REQUEST_MAX 8192
#define LOG_READ_MAX (4u * 1024u * 1024u)
#define COPY_MAX (64u * 1024u)
#define WEB_THREAD_STACK (512u * 1024u)

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} buf_t;

typedef struct {
  char name[NAME_MAX + 1];
  off_t bytes;
  time_t mtime;
  int files;
  bool has_log;
  bool has_video;
} dump_entry_t;

typedef struct {
  char name[NAME_MAX + 1];
  off_t bytes;
  time_t mtime;
} tar_entry_t;

static void buf_need(buf_t *buf, size_t extra) {
  if (buf->failed || buf->len + extra + 1 <= buf->cap) {
    return;
  }
  size_t cap = buf->cap ? buf->cap : 512;
  while (cap < buf->len + extra + 1) {
    cap *= 2;
  }
  char *grown = realloc(buf->data, cap);
  if (!grown) {
    buf->failed = true;
    return;
  }
  buf->data = grown;
  buf->cap = cap;
}

static void buf_add(buf_t *buf, const void *data, size_t length) {
  buf_need(buf, length);
  if (buf->failed) {
    return;
  }
  memcpy(buf->data + buf->len, data, length);
  buf->len += length;
  buf->data[buf->len] = '\0';
}

static void buf_puts(buf_t *buf, const char *text) { buf_add(buf, text, strlen(text)); }

static void buf_printf(buf_t *buf, const char *format, ...) {
  char line[512];
  va_list ap;

  if (buf->failed) {
    return;
  }
  va_start(ap, format);
  int length = vsnprintf(line, sizeof(line), format, ap);
  va_end(ap);
  if (length > 0) {
    buf_add(buf, line, (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1);
  }
}

static void buf_json(buf_t *buf, const char *text) {
  buf_puts(buf, "\"");
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    if (*p == '"' || *p == '\\') {
      buf_printf(buf, "\\%c", *p);
    } else if (*p < 0x20) {
      buf_printf(buf, "\\u%04x", *p);
    } else {
      buf_add(buf, p, 1);
    }
  }
  buf_puts(buf, "\"");
}

static bool send_all(int fd, const void *data, size_t length) {
  const char *cursor = data;

  while (length) {
    ssize_t written = send(fd, cursor, length, 0);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    cursor += written;
    length -= (size_t)written;
  }
  return true;
}

static void send_reply(int fd, int status, const char *reason, const char *type, const char *extra, const char *body,
                       size_t length) {
  buf_t head = {0};

  buf_printf(&head, "HTTP/1.1 %d %s\r\n", status, reason);
  buf_printf(&head, "Content-Type: %s\r\n", type ? type : "text/plain; charset=utf-8");
  buf_printf(&head, "Content-Length: %zu\r\n", length);
  buf_puts(&head, "Access-Control-Allow-Origin: *\r\n");
  buf_puts(&head, "Connection: close\r\n");
  buf_puts(&head, "Cache-Control: no-store\r\n");
  if (extra) {
    buf_puts(&head, extra);
  }
  buf_puts(&head, "\r\n");
  if (!head.failed) {
    send_all(fd, head.data, head.len);
  }
  if (body && length) {
    send_all(fd, body, length);
  }
  free(head.data);
}

static void send_text(int fd, int status, const char *reason, const char *text) {
  send_reply(fd, status, reason, "text/plain; charset=utf-8", NULL, text, strlen(text));
}

static void send_buffer(int fd, int status, const char *reason, const char *type, buf_t *body) {
  if (body->failed) {
    send_text(fd, 500, "Internal Server Error", "out of memory\n");
  } else {
    send_reply(fd, status, reason, type, NULL, body->data ? body->data : "", body->len);
  }
  free(body->data);
  body->data = NULL;
}

/* Only direct children of the dump root with harmless names are reachable. */
static bool valid_name(const char *name) {
  size_t length = strlen(name);

  if (length == 0 || length > NAME_MAX) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)name[i];
    if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

static int decode_segment(const char *source, size_t length, char *dest, size_t cap) {
  size_t out = 0;

  for (size_t i = 0; i < length; i++) {
    char c = source[i];
    if (c == '%' && i + 2 < length && isxdigit((unsigned char)source[i + 1]) && isxdigit((unsigned char)source[i + 2])) {
      char hex[3] = {source[i + 1], source[i + 2], 0};
      c = (char)strtol(hex, NULL, 16);
      i += 2;
    }
    if (out + 1 >= cap) {
      return -1;
    }
    dest[out++] = c;
  }
  dest[out] = '\0';
  return 0;
}

static int split_path(const char *path, char segments[][NAME_MAX + 1], int max) {
  const char *cursor = path;
  int count = 0;

  while (*cursor == '/') {
    cursor++;
  }
  while (*cursor) {
    const char *slash = strchr(cursor, '/');
    size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);

    if (length) {
      if (count == max || length > NAME_MAX || decode_segment(cursor, length, segments[count], NAME_MAX + 1)) {
        return -1;
      }
      count++;
    }
    if (!slash) {
      break;
    }
    cursor = slash + 1;
  }
  return count;
}

static bool query_value(const char *query, const char *key, char *value, size_t cap) {
  size_t key_length = strlen(key);
  const char *cursor = query;

  while (cursor && *cursor) {
    if (!strncmp(cursor, key, key_length) && cursor[key_length] == '=') {
      const char *start = cursor + key_length + 1;
      const char *end = strchr(start, '&');
      size_t length = end ? (size_t)(end - start) : strlen(start);
      size_t out = 0;

      for (size_t i = 0; i < length; i++) {
        char c = start[i];
        if (c == '+') {
          c = ' ';
        } else if (c == '%' && i + 2 < length && isxdigit((unsigned char)start[i + 1]) && isxdigit((unsigned char)start[i + 2])) {
          char hex[3] = {start[i + 1], start[i + 2], 0};
          c = (char)strtol(hex, NULL, 16);
          i += 2;
        }
        if (out + 1 >= cap) {
          return false;
        }
        value[out++] = c;
      }
      value[out] = '\0';
      return true;
    }
    cursor = strchr(cursor, '&');
    if (cursor) {
      cursor++;
    }
  }
  return false;
}

static int compare_dumps(const void *left, const void *right) {
  const dump_entry_t *a = left;
  const dump_entry_t *b = right;

  if (a->mtime != b->mtime) {
    return a->mtime < b->mtime ? 1 : -1;
  }
  return strcmp(a->name, b->name);
}

static void emit_items(buf_t *json, const char *name) {
  char path[PATH_MAX];
  DIR *dir;
  struct dirent *entry;
  bool first = true;

  snprintf(path, sizeof(path), "%s/%s", DUMPER_OUT_ROOT, name);
  if (!(dir = opendir(path))) {
    return;
  }
  while ((entry = readdir(dir))) {
    char full[PATH_MAX];
    struct stat statbuf;

    if (entry->d_name[0] == '.') {
      continue;
    }
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    if (stat(full, &statbuf) || !S_ISREG(statbuf.st_mode)) {
      continue;
    }
    buf_puts(json, first ? "" : ",");
    first = false;
    buf_puts(json, "{\"name\":");
    buf_json(json, entry->d_name);
    buf_printf(json, ",\"bytes\":%lld}", (long long)statbuf.st_size);
  }
  closedir(dir);
}

static dump_entry_t *collect_dumps(size_t *count_out) {
  dump_entry_t *entries = NULL;
  size_t count = 0;
  size_t cap = 0;
  DIR *root = opendir(DUMPER_OUT_ROOT);
  struct dirent *entry;

  *count_out = 0;
  if (!root) {
    return NULL;
  }
  while ((entry = readdir(root))) {
    char path[PATH_MAX];
    struct stat statbuf;
    DIR *dir;
    struct dirent *file;

    if (entry->d_name[0] == '.' || !valid_name(entry->d_name)) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", DUMPER_OUT_ROOT, entry->d_name);
    if (stat(path, &statbuf) || !S_ISDIR(statbuf.st_mode)) {
      continue;
    }
    if (count == cap) {
      cap = cap ? cap * 2 : 16;
      dump_entry_t *grown = realloc(entries, cap * sizeof(*entries));
      if (!grown) {
        free(entries);
        closedir(root);
        return NULL;
      }
      entries = grown;
    }
    dump_entry_t *item = &entries[count];
    memset(item, 0, sizeof(*item));
    strlcpy(item->name, entry->d_name, sizeof(item->name));
    item->mtime = statbuf.st_mtime;
    if ((dir = opendir(path))) {
      while ((file = readdir(dir))) {
        char full[PATH_MAX];
        struct stat file_stat;

        if (file->d_name[0] == '.') {
          continue;
        }
        snprintf(full, sizeof(full), "%s/%s", path, file->d_name);
        if (stat(full, &file_stat) || !S_ISREG(file_stat.st_mode)) {
          continue;
        }
        item->bytes += file_stat.st_size;
        item->files++;
        if (!strcmp(file->d_name, "_klog.txt")) {
          item->has_log = true;
        }
        if (fs_looks_like_video(file->d_name)) {
          item->has_video = true;
        }
      }
      closedir(dir);
    }
    count++;
  }
  closedir(root);
  if (count > 1) {
    qsort(entries, count, sizeof(*entries), compare_dumps);
  }
  *count_out = count;
  return entries;
}

static void api_dumps(int fd) {
  size_t count;
  dump_entry_t *entries = collect_dumps(&count);
  buf_t json = {0};

  buf_puts(&json, "{\"dumps\":[");
  for (size_t i = 0; i < count; i++) {
    buf_puts(&json, i ? ",{\"name\":" : "{\"name\":");
    buf_json(&json, entries[i].name);
    buf_printf(&json, ",\"bytes\":%lld,\"files\":%d,\"mtime\":%lld,\"log\":%s,\"video\":%s,\"items\":[",
               (long long)entries[i].bytes, entries[i].files, (long long)entries[i].mtime, entries[i].has_log ? "true" : "false",
               entries[i].has_video ? "true" : "false");
    emit_items(&json, entries[i].name);
    buf_puts(&json, "]}");
  }
  buf_puts(&json, "]}");
  free(entries);
  send_buffer(fd, 200, "OK", "application/json", &json);
}

static void api_logs(int fd) {
  size_t count;
  dump_entry_t *entries = collect_dumps(&count);
  struct stat statbuf;
  buf_t json = {0};
  bool first = true;

  buf_puts(&json, "{\"logs\":[");
  if (!stat(DUMPER_LOG_PATH, &statbuf) && S_ISREG(statbuf.st_mode)) {
    buf_printf(&json, "{\"name\":\"dumper.log\",\"bytes\":%lld}", (long long)statbuf.st_size);
    first = false;
  }
  for (size_t i = 0; i < count; i++) {
    char full[PATH_MAX];

    snprintf(full, sizeof(full), "%s/%s/_klog.txt", DUMPER_OUT_ROOT, entries[i].name);
    if (!stat(full, &statbuf) && S_ISREG(statbuf.st_mode)) {
      buf_puts(&json, first ? "" : ",");
      first = false;
      buf_printf(&json, "{\"name\":\"%s/_klog.txt\",\"bytes\":%lld}", entries[i].name, (long long)statbuf.st_size);
    }
    snprintf(full, sizeof(full), "%s/%s/_manifest.txt", DUMPER_OUT_ROOT, entries[i].name);
    if (!stat(full, &statbuf) && S_ISREG(statbuf.st_mode)) {
      buf_puts(&json, first ? "" : ",");
      first = false;
      buf_printf(&json, "{\"name\":\"%s/_manifest.txt\",\"bytes\":%lld}", entries[i].name, (long long)statbuf.st_size);
    }
  }
  buf_puts(&json, "]}");
  free(entries);
  send_buffer(fd, 200, "OK", "application/json", &json);
}

static void api_log(int fd, const char *query) {
  char name[NAME_MAX * 2 + 2];
  char path[PATH_MAX];
  char *file;
  struct stat statbuf;
  buf_t body = {0};

  if (!query_value(query, "name", name, sizeof(name))) {
    send_text(fd, 400, "Bad Request", "missing name\n");
    return;
  }
  if (!strcmp(name, "dumper.log")) {
    snprintf(path, sizeof(path), "%s", DUMPER_LOG_PATH);
  } else {
    char *slash = strchr(name, '/');
    if (!slash) {
      send_text(fd, 400, "Bad Request", "invalid log name\n");
      return;
    }
    *slash = '\0';
    file = slash + 1;
    if (!valid_name(name) || (strcmp(file, "_klog.txt") && strcmp(file, "_manifest.txt"))) {
      send_text(fd, 400, "Bad Request", "invalid log name\n");
      return;
    }
    snprintf(path, sizeof(path), "%s/%s/%s", DUMPER_OUT_ROOT, name, file);
  }

  int log_fd = open(path, O_RDONLY | O_CLOEXEC);
  if (log_fd < 0 || fstat(log_fd, &statbuf) || !S_ISREG(statbuf.st_mode)) {
    if (log_fd >= 0) {
      close(log_fd);
    }
    send_text(fd, 404, "Not Found", "log not found\n");
    return;
  }

  off_t start = statbuf.st_size > (off_t)LOG_READ_MAX ? statbuf.st_size - (off_t)LOG_READ_MAX : 0;
  if (start > 0) {
    buf_puts(&body, "[... earlier output truncated ...]\n\n");
  }
  if (lseek(log_fd, start, SEEK_SET) >= 0) {
    char chunk[COPY_MAX];
    ssize_t got;
    while ((got = read(log_fd, chunk, sizeof(chunk))) > 0) {
      buf_add(&body, chunk, (size_t)got);
      if (body.failed) {
        break;
      }
    }
  }
  close(log_fd);
  send_buffer(fd, 200, "OK", "text/plain; charset=utf-8", &body);
}

static int remove_tree(const char *path) {
  struct stat statbuf;

  if (lstat(path, &statbuf)) {
    return errno == ENOENT ? 0 : -1;
  }
  if (!S_ISDIR(statbuf.st_mode)) {
    return unlink(path);
  }

  DIR *dir = opendir(path);
  struct dirent *entry;
  int result = 0;

  if (!dir) {
    return -1;
  }
  while ((entry = readdir(dir))) {
    char child[PATH_MAX];

    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (remove_tree(child)) {
      result = -1;
    }
  }
  closedir(dir);
  if (rmdir(path)) {
    result = -1;
  }
  return result;
}

static void api_delete(int fd, const char *name) {
  char path[PATH_MAX];
  buf_t json = {0};

  if (!valid_name(name)) {
    send_text(fd, 400, "Bad Request", "invalid name\n");
    return;
  }
  snprintf(path, sizeof(path), "%s/%s", DUMPER_OUT_ROOT, name);
  if (remove_tree(path)) {
    send_text(fd, 500, "Internal Server Error", "cannot delete coredump\n");
    return;
  }
  buf_puts(&json, "{\"ok\":true}");
  send_buffer(fd, 200, "OK", "application/json", &json);
}

static void tar_octal(char *field, size_t width, unsigned long long value) {
  char text[32];

  snprintf(text, sizeof(text), "%0*llo", (int)(width - 1), value);
  memcpy(field, text, width - 1);
  field[width - 1] = '\0';
}

static void tar_header(unsigned char *header, const char *directory, const tar_entry_t *entry) {
  memset(header, 0, 512);
  strlcpy((char *)header, entry->name, 100);
  tar_octal((char *)header + 100, 8, 0644);
  tar_octal((char *)header + 108, 8, 0);
  tar_octal((char *)header + 116, 8, 0);
  tar_octal((char *)header + 124, 12, (unsigned long long)entry->bytes);
  tar_octal((char *)header + 136, 12, (unsigned long long)entry->mtime);
  memset(header + 148, ' ', 8);
  header[156] = '0';
  memcpy(header + 257, "ustar", 6);
  memcpy(header + 263, "00", 2);
  strlcpy((char *)header + 345, directory, 155);

  unsigned int checksum = 0;
  for (size_t i = 0; i < 512; i++) {
    checksum += header[i];
  }
  char text[8];
  snprintf(text, sizeof(text), "%06o", checksum);
  memcpy(header + 148, text, 7);
}

static tar_entry_t *collect_tar(const char *directory, size_t *count_out, bool skip_video) {
  char path[PATH_MAX];
  DIR *dir;
  struct dirent *entry;
  tar_entry_t *files = NULL;
  size_t count = 0;
  size_t cap = 0;

  *count_out = 0;
  snprintf(path, sizeof(path), "%s/%s", DUMPER_OUT_ROOT, directory);
  if (!(dir = opendir(path))) {
    return NULL;
  }
  while ((entry = readdir(dir))) {
    char full[PATH_MAX];
    struct stat statbuf;

    if (entry->d_name[0] == '.') {
      continue;
    }
    snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
    if (stat(full, &statbuf) || !S_ISREG(statbuf.st_mode)) {
      continue;
    }
    if (skip_video && fs_looks_like_video(entry->d_name)) {
      continue;
    }
    if (count == cap) {
      cap = cap ? cap * 2 : 16;
      tar_entry_t *grown = realloc(files, cap * sizeof(*files));
      if (!grown) {
        free(files);
        closedir(dir);
        return NULL;
      }
      files = grown;
    }
    memset(&files[count], 0, sizeof(files[count]));
    strlcpy(files[count].name, entry->d_name, sizeof(files[count].name));
    files[count].bytes = statbuf.st_size;
    files[count].mtime = statbuf.st_mtime;
    count++;
  }
  closedir(dir);
  *count_out = count;
  return files;
}

static bool stream_file(int fd, const char *path, off_t expected) {
  int file = open(path, O_RDONLY | O_CLOEXEC);
  off_t left = expected;
  bool ok = true;

  if (file < 0) {
    return false;
  }
  while (left > 0 && ok) {
    char chunk[COPY_MAX];
    size_t want = left < (off_t)sizeof(chunk) ? (size_t)left : sizeof(chunk);
    ssize_t got = read(file, chunk, want);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      ok = false;
      break;
    }
    ok = send_all(fd, chunk, (size_t)got);
    left -= got;
  }
  close(file);
  return ok;
}

static void download_dump(int fd, const char *name, const char *query) {
  size_t count;
  tar_entry_t *files;
  unsigned long long total = 1024; /* two zero blocks terminate the archive */
  char extra[NAME_MAX + 64];
  char path[PATH_MAX];
  char flag[8];
  bool skip_video;
  struct stat statbuf;

  skip_video = query_value(query, "novideo", flag, sizeof(flag)) && strcmp(flag, "0");
  snprintf(path, sizeof(path), "%s/%s", DUMPER_OUT_ROOT, name);
  if (!valid_name(name) || stat(path, &statbuf) || !S_ISDIR(statbuf.st_mode)) {
    send_text(fd, 404, "Not Found", "coredump not found\n");
    return;
  }
  files = collect_tar(name, &count, skip_video);
  for (size_t i = 0; i < count; i++) {
    total += 512 + ((unsigned long long)files[i].bytes + 511) / 512 * 512;
  }
  snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s%s.tar\"\r\n", name,
           skip_video ? "-no-video" : "");
  send_reply(fd, 200, "OK", "application/x-tar", extra, NULL, (size_t)total);

  bool ok = true;
  for (size_t i = 0; i < count && ok; i++) {
    unsigned char header[512];
    tar_header(header, name, &files[i]);
    snprintf(path, sizeof(path), "%s/%s/%s", DUMPER_OUT_ROOT, name, files[i].name);
    ok = send_all(fd, header, sizeof(header)) && stream_file(fd, path, files[i].bytes);
    if (ok && files[i].bytes % 512) {
      unsigned char padding[512] = {0};
      ok = send_all(fd, padding, 512 - (size_t)(files[i].bytes % 512));
    }
  }
  if (ok) {
    unsigned char end[1024] = {0};
    send_all(fd, end, sizeof(end));
  }
  free(files);
}

static void download_file(int fd, const char *name, const char *file) {
  char path[PATH_MAX];
  char extra[NAME_MAX + 64];
  struct stat statbuf;

  if (!valid_name(name) || !valid_name(file)) {
    send_text(fd, 400, "Bad Request", "invalid name\n");
    return;
  }
  snprintf(path, sizeof(path), "%s/%s/%s", DUMPER_OUT_ROOT, name, file);
  int handle = open(path, O_RDONLY | O_CLOEXEC);
  if (handle < 0 || fstat(handle, &statbuf) || !S_ISREG(statbuf.st_mode)) {
    if (handle >= 0) {
      close(handle);
    }
    send_text(fd, 404, "Not Found", "file not found\n");
    return;
  }
  close(handle);
  snprintf(extra, sizeof(extra), "Content-Disposition: attachment; filename=\"%s\"\r\n", file);
  send_reply(fd, 200, "OK", "application/octet-stream", extra, NULL, (size_t)statbuf.st_size);
  stream_file(fd, path, statbuf.st_size);
}

static void serve_static(int fd, const char *path) {
  const web_asset_t *asset = NULL;

  for (int i = 0; i < WEB_ASSET_COUNT; i++) {
    if (!strcmp(web_assets[i].path, path)) {
      asset = &web_assets[i];
      break;
    }
  }
  if (!asset && (!strcmp(path, "/") || !strrchr(path, '.'))) {
    for (int i = 0; i < WEB_ASSET_COUNT; i++) {
      if (!strcmp(web_assets[i].path, "/index.html")) {
        asset = &web_assets[i];
        break;
      }
    }
  }
  if (!asset) {
    send_text(fd, 404, "Not Found", "not found\n");
    return;
  }
  send_reply(fd, 200, "OK", asset->type, NULL, asset->data, asset->length);
}

static void handle_api(int fd, const char *method, const char *path, const char *query) {
  char segments[4][NAME_MAX + 1];
  bool get = !strcmp(method, "GET");
  bool remove = !strcmp(method, "DELETE");
  int count = split_path(path, segments, 4);

  if (count < 2) {
    send_text(fd, 404, "Not Found", "not found\n");
    return;
  }

  if (!strcmp(segments[1], "log") && get && count == 2) {
    api_log(fd, query);
    return;
  }
  if (!strcmp(segments[1], "logs") && get && count == 2) {
    api_logs(fd);
    return;
  }
  if (!strcmp(segments[1], "dumps")) {
    if (get && count == 2) {
      api_dumps(fd);
      return;
    }
    if (count >= 3) {
      const char *name = segments[2];
      if (remove && count == 3) {
        api_delete(fd, name);
        return;
      }
      if (get && count == 3) {
        download_dump(fd, name, query);
        return;
      }
      if (get && count == 4) {
        download_file(fd, name, segments[3]);
        return;
      }
    }
  }
  send_text(fd, 404, "Not Found", "not found\n");
}

static void handle_request(int fd, char *request) {
  char method[8];
  char target[2048];

  if (sscanf(request, "%7s %2047s", method, target) != 2) {
    send_text(fd, 400, "Bad Request", "bad request\n");
    return;
  }
  if (!strcmp(method, "OPTIONS")) {
    send_reply(fd, 204, "No Content", NULL,
               "Access-Control-Allow-Methods: GET, DELETE, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n", NULL, 0);
    return;
  }
  if (strcmp(method, "GET") && strcmp(method, "DELETE")) {
    send_text(fd, 405, "Method Not Allowed", "method not allowed\n");
    return;
  }

  char *query = strchr(target, '?');
  if (query) {
    *query++ = '\0';
  } else {
    query = target + strlen(target);
  }
  if (!strncmp(target, "/api/", 5)) {
    handle_api(fd, method, target, query);
  } else if (strcmp(method, "GET")) {
    send_text(fd, 405, "Method Not Allowed", "method not allowed\n");
  } else {
    serve_static(fd, target);
  }
}

static void *connection_thread(void *argument) {
  int fd = (int)(intptr_t)argument;
  char request[REQUEST_MAX];
  size_t length = 0;

  while (length + 1 < sizeof(request)) {
    ssize_t got = recv(fd, request + length, sizeof(request) - 1 - length, 0);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      break;
    }
    length += (size_t)got;
    request[length] = '\0';
    if (strstr(request, "\r\n\r\n") || strstr(request, "\n\n")) {
      break;
    }
  }
  request[length] = '\0';

  if (length) {
    handle_request(fd, request);
  }
  close(fd);
  return NULL;
}

static bool thread_attributes(pthread_attr_t *attributes) {
  if (pthread_attr_init(attributes)) {
    return false;
  }
  if (pthread_attr_setstacksize(attributes, WEB_THREAD_STACK)) {
    pthread_attr_destroy(attributes);
    return false;
  }
  return true;
}

static void *accept_thread(void *argument) {
  int server = (int)(intptr_t)argument;
  pthread_attr_t attributes;
  bool sized = thread_attributes(&attributes);

  for (;;) {
    int fd = accept(server, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      sleep_ms(100);
      continue;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    pthread_t thread;
    if (pthread_create(&thread, sized ? &attributes : NULL, connection_thread, (void *)(intptr_t)fd)) {
      close(fd);
    } else {
      pthread_detach(thread);
    }
  }
  return NULL;
}

int web_start(unsigned short port, log_t *log) {
  int server = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address;
  pthread_t thread;
  pthread_attr_t attributes;
  bool sized;
  int one = 1;

  if (server < 0) {
    return -1;
  }
  /* The console's socket layer does not always honour SO_NOSIGPIPE. A write
   * to a dropped connection would otherwise terminate the whole payload. */
  signal(SIGPIPE, SIG_IGN);
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);

  if (bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 16)) {
    if (log) {
      log_printf(log, "web: cannot bind port %u: %s", port, strerror(errno));
    }
    close(server);
    return -1;
  }
#ifdef SO_NOSIGPIPE
  setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

  sized = thread_attributes(&attributes);
  if (pthread_create(&thread, sized ? &attributes : NULL, accept_thread, (void *)(intptr_t)server)) {
    if (log) {
      log_printf(log, "web: cannot create the server thread");
    }
    close(server);
    return -1;
  }
  pthread_detach(thread);
  if (log) {
    log_printf(log, "web: listening on port %u", port);
  }
  return 0;
}
