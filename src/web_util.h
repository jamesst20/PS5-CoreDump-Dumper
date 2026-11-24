/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Small growable buffer shared by the API routes. */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  bool failed;
} web_buf_t;

void web_buf_add(web_buf_t *buf, const void *data, size_t length);
void web_buf_puts(web_buf_t *buf, const char *text);
void web_buf_printf(web_buf_t *buf, const char *format, ...);
void web_buf_json(web_buf_t *buf, const char *text);

void web_send_reply(int fd, int status, const char *reason, const char *type, const char *extra, const char *body, size_t length);
void web_send_text(int fd, int status, const char *reason, const char *text);
void web_send_buffer(int fd, int status, const char *reason, const char *type, web_buf_t *body);

/* Only direct children with harmless names are accepted. */
bool web_valid_name(const char *name);
bool web_query_value(const char *query, const char *key, char *value, size_t cap);
