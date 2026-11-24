/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "dumper.h"
#include "instance.h"
#include "klog.h"
#include "log.h"
#include "notify.h"
#include "util.h"
#include "version.h"
#include "web.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int signal_number) {
  (void)signal_number;
  g_stop = 1;
}

static int acquire_instance_lock(void) {
  int fd;

  fd = open(DUMPER_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, DUMPER_MODE_FILE);
  if (fd < 0) {
    return -1;
  }

  if (flock(fd, LOCK_EX | LOCK_NB)) {
    close(fd);
    return -2;
  }

  return fd;
}

int main(void) {
  log_t log = {.fd = -1};
  dumper_t *dumper;
  klog_t *klog;
  int stopped;
  int lock_fd;
  bool web_ok = false;
  char ip[64] = "";
  char message[160];

  if (mkdir(DUMPER_OUT_ROOT, DUMPER_MODE_DIR) && errno != EEXIST) {
    fprintf(stderr, "coredump-dumper: mkdir %s: %s\n", DUMPER_OUT_ROOT, strerror(errno));
    return EXIT_FAILURE;
  }

  log_init(&log, DUMPER_LOG_PATH);
  log_printf(&log, "coredump-dumper %s started (pid %d)", DUMPER_VERSION, getpid());

  /* Show up under a recognizable name in Payload Manager and stop an older
   * instance before taking the lock. */
  instance_claim_name();
  stopped = instance_take_over();
  if (stopped > 0) {
    log_printf(&log, "replaced %d previous instance(s)", stopped);
  } else if (stopped < 0) {
    log_printf(&log, "cannot inspect the process list for previous instances");
  }

  lock_fd = acquire_instance_lock();
  if (lock_fd == -2) {
    fprintf(stderr, "coredump-dumper: already running\n");
    log_close(&log);
    return EXIT_SUCCESS;
  }
  if (lock_fd < 0) {
    fprintf(stderr, "coredump-dumper: cannot acquire lock: %s\n", strerror(errno));
    log_close(&log);
    return EXIT_FAILURE;
  }

  klog = klog_start(&log);
  if (!klog) {
    log_printf(&log, "continuing without kernel log capture");
  }

  if (!(dumper = dumper_create(&log, klog))) {
    log_printf(&log, "cannot initialize dumper");
    klog_stop(klog);
    log_close(&log);
    close(lock_fd);
    return EXIT_FAILURE;
  }

#ifndef HOST_TEST
  web_ok = web_start(DUMPER_WEB_PORT, &log) == 0;
  if (!web_ok) {
    log_printf(&log, "cannot start the web UI on port %d", DUMPER_WEB_PORT);
  }
#endif

  /* Open the watch directory once before announcing, if possible. */
  dumper_poll(dumper);
  if (web_ok && local_ipv4(ip, sizeof(ip))) {
    snprintf(message, sizeof(message), "CoreDump Dumper\nhttp://%s:%d", ip, DUMPER_WEB_PORT);
    log_printf(&log, "web: http://%s:%d", ip, DUMPER_WEB_PORT);
  } else if (web_ok) {
    snprintf(message, sizeof(message), "CoreDump Dumper\nport %d", DUMPER_WEB_PORT);
  } else {
    snprintf(message, sizeof(message), "CoreDump Dumper\nNow watching");
  }
  notify_send(message);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGHUP, on_signal);

  while (!g_stop) {
    bool active = dumper_poll(dumper);

    sleep_ms(active ? DUMPER_ACTIVE_MS : DUMPER_IDLE_MS);
  }

  log_printf(&log, "stopping");
  notify_send("CoreDump Dumper\nStopped");

  dumper_destroy(dumper);
  klog_stop(klog);
  log_close(&log);
  close(lock_fd);

  return EXIT_SUCCESS;
}
