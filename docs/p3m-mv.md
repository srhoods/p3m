# p3m-mv — parallel move

`p3m-mv` moves files and directory trees. On the same filesystem a
subtree moves with a single `rename(2)` — instant regardless of size,
exactly like `mv`. Across filesystems, where `mv` degrades to a
single-threaded copy-and-delete, `p3m-mv` copies with a pool of worker
threads and removes the source as it goes. And unlike `mv`, moving onto
an **existing** destination directory merges the trees instead of
failing with "Directory not empty".

**By default nothing is moved**: the tool performs a dry run showing
exactly what would happen — including whether each subtree would be
renamed or copied — and flagging destination conflicts; add `--apply`
to move.

## Synopsis

```
p3m-mv [OPTIONS] SOURCE... DEST
```

Standard mv target rules: an existing directory `DEST` receives each
`SOURCE` as `DEST/basename(SOURCE)`; otherwise a single `SOURCE` moves
to the path `DEST`.

## Options

| Option | Description |
|--------|-------------|
| `--apply` | Actually move. Without it the run is a dry run — per p3m convention there is no `--dry-run` flag, because that is the default state. |
| `--overwrite` | Replace existing destination entries. Without it they are **skipped** — and a skipped entry always **stays in the source**, so nothing is ever lost. |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV listing to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress (on a terminal) and the end-of-run summary are still shown, and a `-o` file is still written in full — `-q` only silences stdout. |
| `-h, --help`, `-V, --version` | Usage / version. |

## How it moves

For every entry, cheapest mechanism first:

1. **rename(2)** — attempted whenever the entry might land on the same
   filesystem. Succeeds → the entire subtree has moved in one atomic
   syscall (`renameat2(RENAME_NOREPLACE)` without `--overwrite`, so an
   existing destination can never be clobbered by the fast path).
2. **merge** — the destination directory already exists: the walk
   descends and moves each child individually (each child again tries
   rename first), then removes the emptied source directories, deepest
   first. A source directory still holding skipped or failed entries
   is deliberately kept.
3. **copy + delete** — rename returned `EXDEV` (different filesystem):
   the entry is copied with full metadata preservation (mode including
   set-id bits, ownership when permitted, timestamps) and the source
   is unlinked only after the copy has fully succeeded. Directories
   copy in parallel exactly like `p3m-cp`; mount points *inside* the
   tree are handled per-entry, so mixed-device trees work.

A destination directory that already existed keeps its own metadata
(only directories p3m-mv creates inherit the source's mode, owner and
times).

## Safety

- **Dry run by default**, with per-entry rename/copy prediction and
  conflict detection before anything moves.
- **Hard root guard — cannot be overridden.** A source resolving to
  `/` is refused with exit 2 before any work starts. `.` and `..` are
  refused as sources, as with mv.
- **Moving a directory into itself is refused** up front
  (`p3m-mv a a/b` → exit 2).
- **Nothing is lost on conflict.** Without `--overwrite` an existing
  destination is skipped and the source entry is untouched; the
  summary says how many and reminds you of `--overwrite`.
- **Symbolic links are never followed** — a link moves (or is
  recreated) as a link, dangling links included. Cross-device file
  creation uses `O_EXCL`, so data is never written through a symlink
  planted at the destination.
- A file that fails mid-copy is removed from the destination and kept
  at the source.

## Output

CSV, one row per *operation* (a renamed subtree is one row — its
contents are not touched individually):

```csv
source,type,size,destination,result
/data/proj,d,0,/archive/proj,merge
/data/proj/v2,d,0,/archive/proj/v2,renamed
/data/proj/README,f,4096,/archive/proj/README,skipped: exists
```

| `result` value | Meaning |
|----------------|---------|
| `pending: rename` | Dry run: would move with a single rename |
| `pending: copy` | Dry run: would be copied to another filesystem and the source removed |
| `merge` | Destination directory exists; contents are moved into it |
| `exists` | Dry run: destination exists — would be skipped (source kept) |
| `overwrite` | Dry run with `--overwrite`: destination would be replaced |
| `renamed` / `copied` | Applied, via rename / via cross-device copy |
| `skipped: exists` | Applied without `--overwrite`: source kept in place |
| `failed: <step>` | Also counted and summarised on stderr |

Because renames move whole subtrees in one operation, the files/dirs
counts reflect **operations performed**, not the number of entries in
the tree.

## Progress display

With `-o` or `-q` on a terminal, the p3m live status block is shown,
including a running rename count and (during cross-device copies) live
throughput:

```
⠼ p3m-mv — parallel move
  path      …/data/proj/renders
  output    moved.csv
  threads   4              action   apply
  files     8,214          dirs     830
  renames   712            errors   0
  size      1.9 GiB · 320 MiB/s    elapsed  6.1s
```

After the walk, `--apply` runs a finalize pass: created destination
directories get their permanent mode, ownership and timestamps
(deepest first), then the emptied source directories are removed. On
trees with many directories this takes real time, so instead of
appearing to hang the display switches to report it — `action` reads
`finalizing dir metadata`, the path line tracks the directory being
worked on, and the bottom line counts the pass down (the total covers
both the metadata fix-ups and the source-directory removals):

```
⠧ p3m-mv — parallel move
  path      …/src/deep/subdir
  output    moved.csv
  threads   4              action   finalizing dir metadata
  files     10,000         dirs     40,201
  renames   0              errors   0
  finalize  48,211 / 80,402 dirs   elapsed  9.8s
```

The summary always prints; a dry run adds the `--apply` reminder,
skipped conflicts add the `--overwrite` hint, and any source
directories kept because they still hold skipped entries are counted.

## Error handling

Per-entry: recorded in the CSV `result` column, counted, and
summarised on stderr; the run continues. Skips are not errors. Exit
status: `0` success · `1` completed with errors · `2` usage error,
root-guard or into-itself refusal.

## Examples

```sh
# Preview: what happens, and is any of it a slow cross-device copy?
p3m-mv /data/proj /archive/

# Same-filesystem move: one rename, instant at any size
p3m-mv --apply /data/proj /data/proj-2026

# Merge new results into an existing tree (mv would refuse this)
p3m-mv --apply /scratch/results /srv/results

# Evacuate to another filesystem, 8 threads, with an audit trail
p3m-mv --apply -j 8 -o moved.csv /scratch/results /mnt/big/results

# Replace stale duplicates while merging
p3m-mv --apply --overwrite /staging/site /var/www/site
```

## Performance

Measured on the project's test data (XFS, 4 cores, warm cache):

| Workload | `mv` | `p3m-mv --apply -j4` | Speed-up |
|----------|-----:|---------------------:|---------:|
| 9.77 GiB tree, same filesystem | ~0.001 s | 0.001 s | parity (both a single rename) |
| 10,000 × 4 KiB files, cross-filesystem | 0.567 s | 0.141 s | 4.0× |
| Merge into an existing tree | refuses ("Directory not empty") | works | — |

Same-filesystem moves are already optimal in `mv` — one rename — and
p3m-mv takes exactly the same path. The parallel win is the
cross-device case (every file is a copy + metadata + unlink round
trip, and threads keep many in flight — the gap grows further on
network storage) plus the merge capability, where each conflict-free
subtree still moves with a single rename rather than a copy.
