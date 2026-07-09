# p3m — Parallel POSIX Permission Manager

A set of high-performance command line tools for Linux that parallelise
traditionally single-threaded file system operations (permission changes,
ownership changes, and related metadata work) across large directory trees.

## Why

Standard coreutils such as `chmod -R` and `chown -R` walk the tree and issue
system calls from a single thread. On directory trees with tens of thousands
of files — especially on network or high-latency storage — the wall-clock
time is dominated by serialised metadata operations. The p3m tools use
multiple worker threads to walk and modify the tree concurrently, keeping
many operations in flight at once.

## Tools

| Tool | Description | Docs |
|------|-------------|------|
| *(coming soon)* | | |

## Building

```sh
make            # builds all tools into ./bin
make clean      # removes build artifacts
```

Requirements: GCC (or Clang), GNU Make, glibc with POSIX threads. No
external library dependencies.

## Documentation

Per-tool user documentation lives in [`docs/`](docs/).

## Testing

The project expects a `testfolder/` directory (git-ignored) containing
generated test data: directories of files of known size used to benchmark
and validate the tools against their single-threaded coreutils equivalents.

## Licence

MIT
