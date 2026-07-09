# p3m-ls — parallel recursive directory lister

`p3m-ls` walks one or more directory trees with a pool of worker threads
and emits every entry found as CSV. It is a drop-in high-performance
alternative to `ls -R` / `find` for cataloguing large trees, and is
fastest exactly where single-threaded tools are slowest: directories with
very many entries, and storage where each metadata operation has latency
(NFS, CIFS, cluster and object-backed file systems).

## Synopsis

```
p3m-ls [OPTIONS] [PATH...]
```

With no `PATH`, the current directory is scanned. Multiple paths may be
given; they are scanned concurrently as part of the same run.

## Options

| Option | Description |
|--------|-------------|
| `-m, --mode LEVEL` | Detail level: `basic` (default), `standard` or `full`. Single-letter abbreviations (`b`, `s`, `f`) are accepted. |
| `-j, --threads N` | Number of worker threads, 1–512. Default: the number of online CPUs. |
| `-t, --type TYPES` | Only output entries of the given types. Types combine freely, e.g. `-t fl` for files and symlinks. See [Type characters](#type-characters). |
| `--no-dirs` | Omit directories from the output. The walk still descends into them. |
| `-o, --output FILE` | Write the CSV to `FILE` instead of stdout, and show a live progress display on the terminal. |
| `-h, --help` | Show usage and exit. |
| `-V, --version` | Show version and exit. |

## Detail levels

### `basic` (default)

One column: the path of every entry. In this mode `p3m-ls` classifies
entries from the directory stream itself (`d_type`) and normally issues
**no `stat` calls at all**, making it the fastest way to enumerate a tree.

```csv
path
testfolder/testdir0001
testfolder/testdir0001/testfile001
```

### `standard`

```csv
path,type,size,mode,owner,group,mtime
testfolder/testdir0001/testfile001,f,1048576,0644,alice,staff,2026-07-09T19:32:35Z
```

| Column | Meaning |
|--------|---------|
| `path` | Entry path |
| `type` | One-character type (see below) |
| `size` | Size in bytes (`st_size`) |
| `mode` | Permission bits in octal, e.g. `0644` |
| `owner`, `group` | Resolved user and group names (numeric if unresolvable) |
| `mtime` | Last modification time, ISO 8601 UTC |

### `full`

Every field `stat(2)` provides:

```csv
path,type,mode,perms,nlink,owner,uid,group,gid,size,blksize,blocks,dev,ino,rdev,atime,mtime,ctime
```

`perms` is the symbolic form (`-rw-r--r--`); `atime`/`mtime`/`ctime` are
ISO 8601 UTC with nanosecond precision; `dev`, `ino`, `rdev`, `blksize`
and `blocks` are the raw numeric stat fields.

## Type characters

Used both in the `type` output column and as arguments to `--type`:

| Char | Type |
|------|------|
| `f` | regular file |
| `d` | directory |
| `l` | symbolic link |
| `b` | block device |
| `c` | character device |
| `p` | FIFO (named pipe) |
| `s` | socket |

`--type` values may be concatenated (`-t fdl`) and are case-insensitive;
an optional comma separator is allowed (`-t f,l`). Filtering out `d` (or
using `--no-dirs`) affects only the output — the walk always descends
into every directory.

## Progress display

When `-o` is used and stderr is a terminal, a live status block is shown
and refreshed 8 times per second:

```
⠼ p3m-ls — parallel scan
  path      …/testfolder/testdir0042
  output    results.csv
  threads   4              mode     standard
  files     8,214          dirs     830
  rate      1,032,410 items/s      errors   0
  size      8.02 GiB       elapsed  0.9s
```

The `size` line (aggregate size of all regular files discovered) appears
in `standard` and `full` modes only, since `basic` mode does not stat
files. When the run completes the block is replaced by a one-line
summary:

```
✓ p3m-ls complete — 10,000 files · 1,001 dirs · 0 errors · 9.77 GiB
  standard in 0.4s (26,383 items/s) → results.csv
```

If stderr is not a terminal (e.g. inside a cron job) the live display is
suppressed and only the final summary is printed.

## CSV format

- The first line is always a header row.
- Fields containing commas, double quotes or newlines are quoted per
  RFC 4180, with embedded quotes doubled — safe to load into any
  spreadsheet, `sqlite3 .import`, pandas, etc.
- **Row order is non-deterministic** because directories are processed in
  parallel. Pipe through `sort` (or sort after import) if you need a
  stable order.

## Error handling

Unreadable directories or unstattable entries do not stop the run: the
error is counted, the first 24 messages are collected, and everything is
reported on stderr after the scan finishes. The exit status is:

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Completed, but errors were encountered (or the output could not be fully written) |
| 2 | Usage or startup error (bad option, cannot open output file) |

## Examples

```sh
# Fast inventory of a tree, one path per line
p3m-ls /data

# Full metadata catalogue written to a file, with live progress
p3m-ls -m full -o catalogue.csv /data

# Only symlinks and regular files, 16 threads (high-latency NFS mount)
p3m-ls -j 16 -t fl --no-dirs -m standard /mnt/nfs/share

# Total size of all regular files under several trees
p3m-ls -m standard -t f /srv/a /srv/b | awk -F, 'NR>1 {s+=$3} END {print s}'

# Stable sorted listing
p3m-ls /data | tail -n +2 | sort
```

## Behaviour notes

- Symbolic links are reported but **never followed** — neither for
  metadata (`lstat` semantics) nor for recursion, so link cycles cannot
  cause infinite loops.
- Hidden (dot) files are always included; `.` and `..` never are.
- The root paths themselves are not emitted, only their contents —
  matching `ls` semantics. A `PATH` argument that is not a directory is
  emitted as a single entry.
- The aggregate size counter sums `st_size` of regular files only.

## Performance

Measured on the project's `testfolder` data set (1,001 directories,
10,000 × 1 MiB files, XFS, 4 cores, warm cache):

| Command | Time | vs p3m-ls |
|---------|------|-----------|
| `p3m-ls` (basic, 4 threads) | 0.003 s | — |
| `find -mindepth 1` | 0.011 s | 3.7× slower |
| `p3m-ls -m standard` (4 threads) | 0.010 s | — |
| `find -printf '%p,%y,%s,…'` | 0.020 s | 2.0× slower |
| `ls -lR` | 0.064 s | 6.4× slower |

The gap widens substantially on cold caches and on network file systems,
where each `stat`/`readdir` round trip carries real latency and p3m-ls
keeps one operation in flight per thread. On high-latency storage, thread
counts well above the CPU count (`-j 16`, `-j 32`) are usually
beneficial because threads spend most of their time blocked in the
kernel.

Implementation notes: work is distributed via a shared LIFO stack of
directories (LIFO keeps the frontier — and therefore memory — small);
`basic` mode avoids `stat` entirely via `d_type`; `standard`/`full` use
`fstatat` relative to the open directory fd to avoid re-walking paths in
the kernel; output is staged in 1 MiB per-thread buffers so rows never
interleave and lock contention is negligible; uid/gid name lookups are
cached.
