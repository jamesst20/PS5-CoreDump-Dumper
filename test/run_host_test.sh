#!/bin/bash
# SPDX-License-Identifier: MIT
# Host-side smoke test for the dumper logic.  Not used on the PS5.
set -eu

here="$(cd "$(dirname "$0")/.." && pwd)"
tmp="$(mktemp -d)"
watch="$tmp/watch"
out="$tmp/out"

mkdir -p "$watch"
klog="$tmp/klog"
: >"$klog"

clang -std=gnu11 -O1 -g -Wall -Wextra -DHOST_TEST \
  "-DDUMPER_WATCH_DIR=\"$watch\"" "-DDUMPER_OUT_ROOT=\"$out\"" \
  "-DDUMPER_KLOG_PATH=\"$tmp/klog\"" \
  -I"$here/src" -o "$tmp/dumper" \
  "$here"/src/main.c "$here"/src/dump.c "$here"/src/dumper.c "$here"/src/fs.c \
  "$here"/src/instance.c "$here"/src/klog.c "$here"/src/log.c \
  "$here"/src/notify.c "$here"/src/util.c "$here/test/host_stubs.c"

"$tmp/dumper" >"$tmp/run.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; [ -n "${KEEP:-}" ] || rm -rf "$tmp"' EXIT
if [ -n "${KEEP:-}" ]; then echo "tmp: $tmp"; fi

sleep 0.3
printf 'line before crash\n' >>"$klog"

# Crash folder appears.  The dump is written as .tmp and renamed when ready.
crash="$watch/TEST00001_1234567890"
mkdir -p "$crash"
head -c 20000000 /dev/urandom >"$crash/dump.prosperodmp.tmp.part"
mv "$crash/dump.prosperodmp.tmp.part" "$crash/dump.prosperodmp.tmp"
mv "$crash/dump.prosperodmp.tmp" "$crash/dump.prosperodmp"
printf 'notes\n' >"$crash/notes.txt"
head -c 300000 /dev/urandom >"$crash/video.webm"
printf 'line during crash\n' >>"$klog"

sleep 0.5
rm -rf "$crash"
printf 'line after disappearance\n' >>"$klog"
sleep 4

dest="$out/TEST00001_1234567890"
[ -f "$dest/dump.prosperodmp" ] || { echo "FAIL: dump missing"; exit 1; }
[ -f "$dest/notes.txt" ] || { echo "FAIL: notes missing"; exit 1; }
[ -f "$dest/video.webm" ] || { echo "FAIL: video missing"; exit 1; }
[ "$(stat -c %s "$dest/dump.prosperodmp" 2>/dev/null || stat -f %z "$dest/dump.prosperodmp")" = "20000000" ] \
  || { echo "FAIL: dump truncated"; exit 1; }
[ "$(stat -c %s "$dest/video.webm" 2>/dev/null || stat -f %z "$dest/video.webm")" = "300000" ] \
  || { echo "FAIL: video truncated"; exit 1; }
grep -q '^dump.prosperodmp' "$dest/_manifest.txt" || { echo "FAIL: manifest"; exit 1; }
[ -f "$dest/_klog.txt" ] || { echo "FAIL: kernel log missing"; exit 1; }
[ ! -e "$dest/dump.prosperodmp.tmp" ] || { echo "FAIL: duplicate tmp copy"; exit 1; }
grep -q 'line before crash' "$dest/_klog.txt" || { echo "FAIL: pre-crash log missing"; exit 1; }
grep -q 'line during crash' "$dest/_klog.txt" || { echo "FAIL: during-crash log missing"; exit 1; }
grep -q 'line after disappearance' "$dest/_klog.txt" || { echo "FAIL: post-crash log missing"; exit 1; }

# Second crash: .tmp never renamed, folder vanishes.  Data must still survive.
crash2="$watch/TEST00002_2234567890"
mkdir -p "$crash2"
head -c 5000000 /dev/urandom >"$crash2/never.prosperodmp.tmp"
sleep 0.3
rm -rf "$crash2"
sleep 2

dest2="$out/TEST00002_2234567890"
[ -f "$dest2/never.prosperodmp.tmp" ] || { echo "FAIL: unrenamed tmp missing"; exit 1; }
[ "$(stat -c %s "$dest2/never.prosperodmp.tmp" 2>/dev/null || stat -f %z "$dest2/never.prosperodmp.tmp")" = "5000000" ] \
  || { echo "FAIL: unrenamed tmp truncated"; exit 1; }

# Third crash: late file added while the folder exists must be picked up.
crash3="$watch/TEST00003_3234567890"
mkdir -p "$crash3"
printf 'first\n' >"$crash3/first.txt"
sleep 0.4
printf 'late\n' >"$crash3/late.txt"
sleep 0.4
rm -rf "$crash3"
sleep 2

dest3="$out/TEST00003_3234567890"
[ -f "$dest3/late.txt" ] || { echo "FAIL: late file missing"; exit 1; }

# Fourth crash: still growing while the folder is deleted.  An open fd keeps
# the inode alive, so the complete data must be drained.
crash4="$watch/TEST00004_4234567890"
mkdir -p "$crash4"
(
  exec 3>"$crash4/growing.prosperodmp.tmp"
  for i in $(seq 1 20); do
    head -c 500000 /dev/urandom >&3
    sleep 0.05
  done
) &
writer=$!
sleep 0.3
rm -rf "$crash4"
wait $writer
sleep 2

dest4="$out/TEST00004_4234567890"
[ -f "$dest4/growing.prosperodmp.tmp" ] || { echo "FAIL: growing file missing"; exit 1; }
[ "$(stat -c %s "$dest4/growing.prosperodmp.tmp" 2>/dev/null || stat -f %z "$dest4/growing.prosperodmp.tmp")" = "10000000" ] \
  || { echo "FAIL: growing file truncated"; exit 1; }

echo "PASS"
