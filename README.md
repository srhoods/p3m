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
| `p3m-ls` | Parallel recursive directory lister with CSV output, three detail levels, type filtering and live progress | [docs/p3m-ls.md](docs/p3m-ls.md) |
| `p3m-ch` | Parallel chmod + chown + chgrp in a single scan; separate dir/file modes, octal or symbolic, dry-run by default | [docs/p3m-ch.md](docs/p3m-ch.md) |
| `p3m-rm` | Parallel recursive remove with glob masks, dry-run by default, non-overridable root guard, never follows symlinks | [docs/p3m-rm.md](docs/p3m-rm.md) |
| `p3m-du` | Parallel disk usage with GNU du-compatible options (`-s -c -d -h --si -X -B -b`), hard-link dedup, never follows symlinks | [docs/p3m-du.md](docs/p3m-du.md) |
| `p3m-cp` | Parallel copy (recursive like `cp -R`), dry-run by default, skips existing destinations unless `--overwrite`, `-p` metadata preservation, never follows symlinks | [docs/p3m-cp.md](docs/p3m-cp.md) |
| `p3m-mv` | Parallel move: single-rename fast path, parallel cross-device copy+delete, merges into existing trees (mv refuses), dry-run by default, hard root guard | [docs/p3m-mv.md](docs/p3m-mv.md) |
| `p3m-find` | Parallel find: classic expression grammar (`! ( ) -a -o`), common tests (`-name -type -size -mtime -perm -user -empty -regex …`), lazy stat, `-print0`; deliberately no `-delete`/`-exec` | [docs/p3m-find.md](docs/p3m-find.md) |
| `p3m-diff` | Parallel directory comparison: names, types, sizes and metadata by default, byte-exact content verification with `-c` (size checked first — content read skipped when sizes differ), diff-like exit codes and a plain-language verdict | [docs/p3m-diff.md](docs/p3m-diff.md) |
| `p3m-stats` | Age and hot/cold statistics from a `p3m-ls -m full` catalogue: most recently accessed/modified and largest files, atime/mtime age histograms and an extension breakdown by file count and bytes, streamed so row count doesn't bound memory | [docs/p3m-stats.md](docs/p3m-stats.md) |

## Building and installing

```sh
make                  # builds all tools into ./bin and man pages into ./obj/man
make clean            # removes build artifacts
sudo make install     # installs to /usr (PREFIX=, DESTDIR= supported)
```

Requirements: GCC (or Clang), GNU Make, glibc with POSIX threads. No
external library dependencies. Packaging needs `rpmbuild`.

### RPM

```sh
make rpm              # binary RPM  -> rpmbuild/RPMS/<arch>/p3m-<version>-1.<dist>.<arch>.rpm
make srpm             # source RPM  -> rpmbuild/SRPMS/
```

The package installs the nine tools to `/usr/bin`, their man pages
(`man p3m`, `man p3m-ls`, ...) and the documentation under
`/usr/share/doc/p3m`.

## Versioning

The suite is versioned as a whole ([Semantic Versioning](https://semver.org/)).
The top-level [`VERSION`](VERSION) file is the single source of truth: it is
compiled into every tool, so any of them reports the release number:

```
$ p3m-ls --version
p3m-ls 1.0.0 (p3m: Parallel POSIX Permission Manager)
```

[CHANGELOG.md](CHANGELOG.md) has one `## [x.y.z] - date` heading per release,
and the RPM version, its `%changelog` and the man-page footers are generated
from those two files. `make check-version` fails if they disagree.

To cut a release: move the `[Unreleased]` entries under a new
`## [x.y.z] - YYYY-MM-DD` heading, put `x.y.z` in `VERSION`, run
`make check-version rpm`, then commit and tag `vx.y.z`.

## Documentation

Per-tool user documentation lives in [`docs/`](docs/), and every tool has a
man page (`man p3m` gives an overview). See [CHANGELOG.md](CHANGELOG.md) for
a history of what changed and when.

## Testing

The project expects a `testfolder/` directory (git-ignored) containing
generated test data: directories of files of known size used to benchmark
and validate the tools against their single-threaded coreutils equivalents.

## Licence

MIT — see [LICENSE.md](LICENSE.md).
