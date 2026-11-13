/* SPDX-License-Identifier: MIT */
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HOST_TEST
intptr_t kernel_get_root_vnode(void);
intptr_t kernel_get_proc_rootdir(pid_t pid);
int32_t kernel_set_proc_rootdir(pid_t pid, intptr_t vnode);
#else
#include <ps5/kernel.h>
#endif

static const char *const temp_suffixes[] = {".tmp", ".part"};

int fs_open_root(const char *path, int flags) {
  int saved_errno;
  intptr_t rootdir;
  intptr_t rootvnode;
  pid_t pid;
  int fd;

  if ((fd = open(path, flags)) >= 0) {
    return fd;
  }

  pid = getpid();
  if (!(rootvnode = kernel_get_root_vnode())) {
    return -1;
  }
  if (!(rootdir = kernel_get_proc_rootdir(pid))) {
    return -1;
  }
  if (kernel_set_proc_rootdir(pid, rootvnode)) {
    return -1;
  }

  fd = open(path, flags);
  saved_errno = errno;

  kernel_set_proc_rootdir(pid, rootdir);
  errno = saved_errno;

  return fd;
}

bool fs_safe_name(const char *name) {
  size_t len = strlen(name);

  if (len == 0 || len > NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) {
    return false;
  }

  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)name[i];

    if (c <= 0x20 || c == 0x7f || c == '/') {
      return false;
    }
  }

  return true;
}

bool fs_has_suffix(const char *string, const char *suffix) {
  size_t string_len = strlen(string);
  size_t suffix_len = strlen(suffix);

  return string_len >= suffix_len && !strcasecmp(string + string_len - suffix_len, suffix);
}

bool fs_is_temp_name(const char *name) {
  for (size_t i = 0; i < sizeof(temp_suffixes) / sizeof(temp_suffixes[0]); i++) {
    if (fs_has_suffix(name, temp_suffixes[i])) {
      return true;
    }
  }

  return false;
}

void fs_strip_temp_suffixes(char *name) {
  bool stripped;

  do {
    size_t length = strlen(name);

    stripped = false;
    for (size_t i = 0; i < sizeof(temp_suffixes) / sizeof(temp_suffixes[0]); i++) {
      size_t suffix_length = strlen(temp_suffixes[i]);

      if (length > suffix_length && fs_has_suffix(name, temp_suffixes[i])) {
        name[length - suffix_length] = '\0';
        stripped = true;
        break;
      }
    }
  } while (stripped);
}

bool fs_looks_like_video(const char *name) {
  static const char *const extensions[] = {".webm", ".mp4", ".mkv", ".avi", ".mov", ".m4v"};

  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    if (fs_has_suffix(name, extensions[i])) {
      return true;
    }
  }

  return false;
}

bool fs_looks_like_dump(const char *name) {
  return strcasestr(name, "prosperodmp") || strcasestr(name, "coredump") || fs_has_suffix(name, ".dmp") ||
         !strcasecmp(name, "core") || !strcasecmp(name, "core.dump");
}
