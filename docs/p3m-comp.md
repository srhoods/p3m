# p3m-comp — parallel directory comparison

`p3m-comp` compares two directory trees with a pool of worker threads
and reports the likelihood that their contents are the same. By
default it compares entry **names**, **types**, **sizes** (regular
files) and **metadata** — mode, owner, group and mtime — which is fast
enough to run constantly; with `-c` it additionally verifies file
**contents**. Every difference is listed as CSV and the run ends with
a summary and a plain-language verdict.

The tool is read-only: nothing on either side is ever modified.

## Synopsis

```
p3m-comp [OPTIONS] LEFT RIGHT
```

Both arguments must be directories. Exit status is diff-like:
`0` no differences · `1` differences found · `2` usage error, or the
comparison hit errors (the verdict is withheld — coverage was
incomplete, so "no differences found" would be misleading).

## Options

| Option | Description |
|--------|-------------|
| `-c, --checksum` | Also compare file contents. A file's **size is checked first**: when the sizes already differ the content read is skipped, since the files are already proven different. |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV listing to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress and the summary — including the verdict — still print, making `-q` a fast "are these the same?" check. |
| `-h, --help`, `-V, --version` | Usage / version. |

## What is compared

| Entry kind | Checked |
|------------|---------|
| all common entries | type (`f d l b c p s`) — a type mismatch stops further comparison of that entry |
| regular files | size; mode, owner, group, mtime; contents with `-c` |
| directories | mode, owner, group, mtime; then compared recursively |
| symbolic links | target, owner, group (mode is meaningless on Linux and link mtimes are rarely preserved — both skipped). **Links are never followed** and never descended into. |
| block/char devices | device numbers; mode, owner, group, mtime |
| fifos, sockets | mode, owner, group, mtime |

A directory present on only one side is reported once and not
descended into. The two roots' own metadata is compared as path `.`.
Timestamps are compared at full nanosecond precision — a copy made
with `p3m-cp --apply -p` or `cp -a` compares clean.

## Content verification (`-c`)

Rather than computing checksums of each side, `-c` reads both files
in 1 MiB chunks and compares the bytes directly. This is strictly
stronger than a checksum — there is no hash-collision risk, a
mismatch is never missed — and cheaper: the comparison stops at the
first differing byte instead of reading both files to the end, and
the listing reports that offset (`content,differ at byte 12345`).

The size gate always applies: files whose sizes differ are reported
as `size` differences and their contents are never read. This was
verified with strace — a size-mismatched file is never opened.

## Output

CSV columns: `path,difference,left,right`, one row per differing
attribute (a file can produce several rows, e.g. `mode` + `mtime`).
`path` is relative to the roots. `difference` is one of:

| Value | Meaning | left / right columns |
|-------|---------|----------------------|
| `only-left`, `only-right` | entry exists on one side only | type char / empty |
| `type` | different entry types | the two type chars |
| `size` | regular file sizes differ | the two sizes in bytes |
| `content` | contents differ (`-c`) | first differing byte offset / empty |
| `mode`, `owner`, `group`, `mtime` | metadata differs | the two values |
| `target` | symlink targets differ | the two targets |
| `device` | device numbers differ | the two `major,minor` pairs |

## Summary and verdict

The summary always prints, with a per-category breakdown when
differences were found, and ends with the verdict:

```
✓ p3m-comp complete — 10,000 files · 1,002 dirs · 0 differences · 0 errors
  0.01s (493,732 items/s)
  trees are very likely identical — names, types, sizes and metadata all match (add -c to verify file contents)
```

- **no differences, default mode** — *"very likely identical"*: every
  name, type, size and metadatum matches; only content substitution at
  identical size and mtime could hide (run `-c` to eliminate it).
- **no differences with `-c`** — *"identical"*: contents were verified
  byte-for-byte.
- **differences** — *"trees differ"*, with the breakdown:

```
✗ p3m-comp complete — 12 files · 5 dirs · 8 differences · 0 errors
  2 only in left · 1 only in right · 1 type · 1 size · 0 content · 3 metadata
  0.0s (54,795 items/s)
  trees differ — 8 differences are listed above
```

- **errors occurred** — the verdict is withheld and the exit status
  is 2: an unreadable directory means the trees were not fully
  compared, so no equality claim is made.

## Progress display

With `-o` or `-q` on a terminal, the p3m live status block is shown.
The `diffs` counter is live, and with `-c` the bottom line tracks
content bytes compared and throughput:

```
⠼ p3m-comp — parallel compare
  path      …/testfolder/testdir0042
  output    none (-q)
  threads   4              check    names+meta+size+content
  files     8,214          dirs     830
  diffs     0              errors   0
  checked   3.91 GiB · 412 MiB/s   elapsed  11.2s
```

## Examples

```sh
# Quick verdict: did the copy get everything? (metadata only, fast)
p3m-comp -q /data/projects /backup/projects

# Verify a migration byte-for-byte before deleting the source
p3m-comp -cq /old/array/home /new/array/home

# Full difference report to CSV with live progress
p3m-comp -c -o drift.csv /etc /srv/config-snapshot/etc

# Audit a p3m-cp -p copy (compares clean, including dir mtimes)
p3m-cp --apply -p /src/tree /dst && p3m-comp -cq /src/tree /dst/tree
```

## Performance

Measured on the project's `testfolder` data set (1,001 directories,
10,000 × 1 MiB files, 9.77 GiB) against a `cp -a` copy, XFS, 4 cores,
warm cache:

| Comparison | single-threaded tool | `p3m-comp -j4` | Speed-up |
|------------|---------------------:|---------------:|---------:|
| names + metadata + size (default) | `rsync -an` 0.089 s | 0.014 s | 6.4× |
| contents (`-c`, 2 × 9.77 GiB read) | `diff -qr` 34.2 s | 26.6 s | 1.3× |

Metadata mode is syscall-bound and parallelises well (`-j1` → `-j4`
is 2.8×). Content mode is storage-bound: the 1.3× above is what four
concurrent read streams buy on this array, and `-j1` (57.6 s) is
slower than `diff -qr` — on high-latency or many-spindle storage the
gap widens, on a single busy disk more threads can seek-thrash. As
always the size gate means trees that differ in size compare almost
instantly even with `-c`.
