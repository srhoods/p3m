# p3m-du — parallel disk usage

`p3m-du` estimates file space usage like `du(1)`, but walks the tree
with a pool of worker threads. It supports the common du options
(`-s -c -d -h --si -X --exclude -B -b`) with GNU-compatible semantics —
sizes, rounding and hard-link handling match `du` exactly — plus the
usual p3m output, progress and quiet options.

> **Note:** unlike the other p3m tools, `-h` means `--human-readable`,
> matching `du`. Use `--help` for usage.

## Synopsis

```
p3m-du [OPTIONS] [PATH...]
```

With no `PATH`, the current directory is measured. Multiple paths are
walked concurrently as part of the same run.

## Options

### du options

| Option | Description |
|--------|-------------|
| `-s, --summarize` | Only print each `PATH` argument (same as `-d 0`). |
| `-c, --total` | Append a grand-total row (`<size>,total`). |
| `-d, --max-depth N` | Print directories at most `N` levels below each `PATH`. The walk and the totals always cover the entire tree — this filters only what is printed. |
| `-h, --human-readable` | Sizes like `1.0K`, `23M`, `2.3G` (powers of 1024). |
| `--si` | Like `-h`, but powers of 1000 (`1.0k`, `23M`, `2.3G`). |
| `-B, --block-size SIZE` | Print sizes in `SIZE`-byte blocks, rounded up. `SIZE` takes suffixes: `K`,`M`,`G`,… (1024-based), `KB`,`MB`,… (1000-based), `KiB`,`MiB`,… (1024-based). E.g. `-BM`, `-B 4K`. |
| `-b, --bytes` | Apparent size (`st_size`) in bytes instead of disk usage. |
| `--exclude PATTERN` | Skip entries matching the glob `PATTERN`; matching directories are not entered. |
| `-X, --exclude-from FILE` | Read exclude patterns from `FILE`, one per line (blank lines ignored). May be repeated and combined with `--exclude`. |

### p3m options

| Option | Description |
|--------|-------------|
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress (on a terminal) and the end-of-run summary are still shown, and a `-o` file is still written in full — `-q` only silences stdout. |
| `--help` | Usage (long form only — `-h` is human-readable). |
| `-V, --version` | Version. |

## Output

CSV, one row per directory (and one per non-directory argument), size
first as with du:

```csv
size,path
1028,testfolder/testdir0042
10261504,testfolder
```

Each directory's size is the **recursive** total of everything beneath
it, including the directory's own blocks. Rows are printed **deepest
first**: every directory appears after all of its subdirectories, with
the `PATH` arguments last (and the `total` row after that, with `-c`) —
the same shape as du output. Beyond that guarantee the order is
non-deterministic because the tree is walked in parallel.

## Size semantics (GNU-compatible)

- **Default**: disk usage — `st_blocks × 512` summed over the tree,
  printed in 1 KiB blocks, rounded up. Sparse files count their
  allocated blocks, as with du.
- **`-b`**: apparent size — `st_size` summed, printed in bytes
  (equivalent to du's `--apparent-size --block-size=1`).
- **Rounding** happens once, on each printed value, never per file.
  Human-readable values use du's ceiling rule: one decimal below 10
  (`9.3M`), integers above (`23M`), promoting to the next unit when
  rounding would reach it (`1023.5K` → `1.0M`).
- **Hard links**: a file with multiple hard links is counted once per
  run (first encounter wins), matching du. With multiple `PATH`
  arguments the deduplication spans all of them, so `-c` totals are
  honest.
- **Symbolic links are never followed.** A symlink contributes the size
  of the link itself. This also holds for directories swapped for
  symlinks mid-run (`O_NOFOLLOW` re-verification, as in all p3m tools).

### Exclude patterns

A pattern excludes an entry if it matches (glob-style, `fnmatch(3)`)
either the entry's **name** or its **full path as printed**. Excluded
directories are not descended into, and excluded entries contribute
nothing to any total. `du --exclude='*.o'` and
`p3m-du --exclude='*.o'` behave identically for the common
basename-pattern cases.

## Progress display

With `-o` or `-q` on a terminal, the p3m live status block is shown
(8 refreshes/s):

```
⠼ p3m-du — parallel disk usage
  path      …/testfolder/testdir0042
  output    usage.csv
  threads   4              units    1K
  files     8,214          dirs     830
  rate      1,032,410 items/s      errors   0
  size      8.02 GiB       elapsed  0.9s
```

`units` shows the active size format (`1K` by default, `human`, `si`,
`bytes`, or the `-B` argument). When the run completes the block is
replaced by a summary:

```
✓ p3m-du complete — 10,000 files · 1,001 dirs · 0 errors · 9.79 GiB
  1,002 rows in 0.4s (26,383 items/s) → usage.csv
```

The summary size is the grand total across all arguments (IEC
formatted, independent of the CSV units).

## Error handling

Unreadable directories or unstattable entries do not stop the run: the
error is counted and reported on stderr after the walk, and — as with
du — the totals cover everything that *was* readable. Exit status:
`0` success · `1` completed with errors · `2` usage error.

## Examples

```sh
# What's using the space? Top-level view, human readable
p3m-du -h -d 1 /data

# Total size of several trees, one number
p3m-du -sch /srv/a /srv/b /srv/c

# Apparent bytes for a transfer estimate, excluding build artifacts
p3m-du -b --exclude='*.o' --exclude='.git' -s ~/src/project

# Full per-directory catalogue to a file, with live progress
p3m-du -B1 -o usage.csv /data

# Sizes in 4 KiB filesystem blocks
p3m-du -B 4KiB -d 2 /var
```

## Behaviour notes

- `.` and hidden entries are always included; `.` and `..` directory
  entries never are.
- Individual files are not listed (du without `-a` semantics); a `PATH`
  argument that is not a directory is printed as its own row.
- Paths longer than the system `PATH_MAX` cannot be traversed and are
  reported as errors.
- Unlike du, output is CSV with a header row and needs no locale
  guards; fields with commas or quotes are RFC 4180 quoted.

## Performance

Measured on the project's `testfolder` data set (1,001 directories,
10,000 × 1 MiB files, XFS, 4 cores, warm cache):

| Command | Time | vs p3m-du |
|---------|------|-----------|
| `p3m-du -s` (4 threads) | 0.007 s | — |
| `du -s` | 0.022 s | 3.1× slower |

Every entry needs a `stat` call (sizes live in the inode), so unlike
`p3m-ls` basic mode there is no stat-free fast path — which is exactly
why parallelism pays: on network file systems where each `stat` is a
round trip, keeping one operation in flight per thread multiplies
throughput, and thread counts above the CPU count (`-j 16`, `-j 32`)
help further.

Implementation notes: one node per directory in a chunked, pointer-stable
store; workers accumulate sizes into their directory's node during the
walk; because a child node is always created after its parent, a single
reverse sweep afterwards rolls all totals up the tree in O(n). Hard-link
deduplication uses a shared (dev, ino) hash table consulted only for
files with `st_nlink > 1`.
