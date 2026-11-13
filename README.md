# CoreDump Dumper

## Payload Name

`coredump-dump.elf`

## Description

CoreDump Dumper is a PS5 payload that watches the console's coredump staging
directory and preserves crash dumps before the system deletes them.

When a title crashes, the console creates
`/devlog/system/sce_coredumps.0/<TitleID>_<Timestamp>/` and removes it again as
soon as the dump has been generated or uploaded. The window can be only a
couple of seconds, which makes a manual FTP copy unreliable.

CoreDump Dumper polls for new crash directories, pins every file with an open
descriptor, and copies the contents to persistent storage. Open descriptors
keep the data reachable even when the console unlinks the source directory
while a large dump is still being copied.

## Features

- Detects new crash dumps and saves them before the console deletes them.
- Preserves all available crash files, including the dump, screenshot, video,
  and metadata.
- Includes the kernel log from 60 seconds before the crash up to 3 seconds
  after the crash folder disappears, written to `_klog.txt`.
- Writes `_manifest.txt` with the saved file list, sizes, and copy status.
- Copies complete files only and does not leave temporary duplicates behind.
- Keeps copying from already-opened files even if the console deletes the
  source folder during a large dump.
- Runs unattended in the background and requires no interaction.

## Usage

Build and deploy with a payload loader, or use the deploy target:

```sh
PS5_HOST=192.168.1.50 PS5_PORT=9021 \
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk make deploy
```

The payload runs until stopped. Its PID is written to
`/data/ps5-coredump-dumper/dumper.log`; it appears in Payload Manager as
`coredump-dump.elf`.

Only one instance runs at a time. Launching the payload again terminates the
previous instance and takes over.

No interaction is required after startup. The dumper reports readiness with a
debug toast and a log entry.

## Coredump Storage

Completed crashes are stored under:

```text
/data/ps5-coredump-dumper/<TitleID>_<Timestamp>/
```

Example of a complete dump directory:

```text
/data/ps5-coredump-dumper/PPSA00000_1794577085/
├── prosperocore-1794577085-0x00000068-eboot.bin.prosperodmp
├── prosperocore-1794577085-0x00000068-eboot.bin.prosperodmpmanifest
├── prosperocore-1794577085-0x00000068-eboot.bin-crash-screenshot.jpg
├── prosperocore-1794577085-0x00000068-eboot.bin-crash-video.webm
├── prosperocore-1794577085-0x00000068-eboot.bin-crash-video.json
├── prosperocore-report.prosperostate
├── coredump_info.json
├── _manifest.txt
└── _klog.txt
```

Each directory contains the copied crash evidence and two generated files:

- `_manifest.txt` — source path, file list, byte counts, and status for every
  tracked file.
- `_klog.txt` — kernel log covering 60 seconds before to 3 seconds after the
  crash directory disappeared.

## Build

Requirements:

- POSIX build host
- PS5 Payload SDK at `PS5_PAYLOAD_SDK`
- `llvm-config` on `PATH` for the SDK toolchain wrapper

```sh
PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk make
```

The version is read from `VERSION` and embedded in the payload. Builds are
written to `dist/coredump-dumper-v<VERSION>.elf`, for example
`dist/coredump-dumper-v1.00.elf`.

Other targets:

```sh
make version     # print the current version
make deploy      # build and upload to PS5_HOST:PS5_PORT
make host-test   # host-side smoke test with kernel stubs
make format      # apply .clang-format
make clean
```

Tagging a release as `v<VERSION>` builds and publishes the payload through
GitHub Actions.

## License

MIT. See [`LICENSE`](LICENSE).
