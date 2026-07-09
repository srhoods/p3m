# p3m-rm — parallel recursive remove

`p3m-rm` removes files and directory trees using a pool of worker
threads — a parallel `rm -rf` with a safety-first design. Arguments may
be literal paths or glob masks. **By default nothing is removed**: the
tool performs a dry run listing everything that would be deleted; add
`--apply` to delete.

## Synopsis

```
p3m-rm [OPTIONS] PATH|MASK...
```

A `MASK` is a glob pattern (`*`, `?`, `[…]`) expanded by the tool
itself — quote it so your shell doesn't expand it first:

```sh
p3m-rm '/data/scratch/tmp-*'
```

The named paths themselves are removed along with their contents.

## Options

| Option | Description |
|--------|-------------|
| `--apply` | Actually remove. Without it the run is a dry run — per p3m convention there is no `--dry-run` flag, because that is the default state. |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV listing to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress (on a terminal) and the end-of-run summary are still shown, and a `-o` file is still written in full — `-q` only silences stdout. |
| `-h, --help`, `-V, --version` | Usage / version. |

## Safety

`p3m-rm` is a destructive tool, so it carries guarantees the
coreutils equivalents do not:

- **Dry run by default.** `--apply` is the only way to delete anything.
- **Hard root guard — cannot be overridden.** Before any work starts,
  every target (after mask expansion) is fully resolved; if any one of
  them resolves to `/`, the entire run is refused with exit status 2.
  There is deliberately no flag, environment variable or build option
  that bypasses this. `/`, `//`, `/tmp/..`, a chain of `..` that lands
  on the root, or running from `/` against `.` are all caught.
- **`.` and `..` are refused** as targets, as with `rm`.
- **Symbolic links are never followed, under any circumstances.**
  A symlink is unlinked; its target is untouched. This holds for
  links given as arguments, links found during the walk, and — via
  `O_NOFOLLOW` re-verification when opening each directory — even for
  a directory swapped for a symlink *mid-run* (the classic
  TOCTOU deletion race). `unlinkat` and `rmdir` never dereference.
- **Fail-fast validation.** The guard pass completes before the first
  unlink; a refused target means nothing at all has been removed.

## How apply works

1. **Phase 1 — parallel walk**: worker threads scan the tree, unlink
   files (and symlinks, sockets, FIFOs, devices) as they are
   discovered, and collect every directory.
2. **Phase 2 — deepest-first rmdir**: the collected directories are
   sorted by depth and removed level by level (deepest first), each
   level processed in parallel. A parent is never attempted before all
   of its children's levels are complete, so no retry logic is needed.

An entry that has already vanished when its turn comes (`ENOENT`) is
treated as removed, not as an error — as with `rm -f`.

## Output

CSV, one row per entry:

```csv
path,type,result
/data/scratch/tmp-1,d,pending
/data/scratch/tmp-1/run.log,f,pending
```

| Column | Meaning |
|--------|---------|
| `path` | Entry path |
| `type` | `f` file · `d` dir · `l` symlink · `b`/`c` device · `p` fifo · `s` socket |
| `result` | `pending` (dry run) · `removed` · `failed: <op>: <reason>` |

No `stat` calls are made for the listing (types come from the directory
stream), which keeps the scan as fast as `p3m-ls` basic mode. Row order
is non-deterministic; fields with commas/quotes are RFC 4180 quoted.

## Progress display

With `-o` or `-q` on a terminal, the p3m live status block is shown
(8 refreshes/s) and replaced by a summary when done:

```
⠼ p3m-rm — parallel remove (apply)
  path      …/scratch/tmp-7/renders
  output    removed.csv
  threads   4              action   apply
  files     8,214          dirs     830
  removed   8,102          errors   0
  rate      96,000 items/s elapsed  0.8s
```

```
✓ p3m-rm complete — apply · 10,251 scanned · 10,251 removed · 0 errors
  0.1s (94,915 items/s) → removed.csv
```

The summary always prints (with or without `-o`); a dry run also prints
a reminder that `--apply` is required to delete.

## Error handling

Errors (unreadable directories, permission failures) are per-entry:
recorded in the CSV `result` column, counted, and summarised on stderr.
An undeletable child naturally causes its ancestors' `rmdir` to fail
with "Directory not empty", which is reported the same way — matching
`rm -rf` behaviour. The run never aborts part-way (except for the root
guard, which aborts *before* it starts).

Exit status: `0` success · `1` completed with errors · `2` usage error
or root-guard refusal.

## Examples

```sh
# See what would go (dry run, CSV to stdout)
p3m-rm /data/scratch/old-builds

# Remove it, 8 threads, with an audit trail and live progress
p3m-rm --apply -j 8 -o removed.csv /data/scratch/old-builds

# Masks: clean up matching trees only (quote the mask!)
p3m-rm --apply '/data/scratch/tmp-*' '/data/cache/sess_??'

# Count what a cleanup would reclaim before doing it
p3m-rm '/var/tmp/build-*' | tail -n +2 | wc -l
```

## Behaviour notes

- Overlapping targets (e.g. `a` and `a/b`) are safe: whichever thread
  gets there first wins and the loser's `ENOENT` is ignored.
- Paths longer than the system `PATH_MAX` (~4 KiB) cannot be traversed;
  such entries are reported as errors.
- The walk uses `d_type` from the directory stream and only falls back
  to `lstat` on filesystems that don't provide it.

## Performance

Measured on the project's XFS test filesystem (4 cores, warm cache):

| Workload | `rm -rf` | `p3m-rm --apply -j4` | Speed-up |
|----------|---------:|---------------------:|---------:|
| 10,252 entries, 64 KiB files (629 MB) | 0.310 s | 0.108 s | 2.9× |
| 11,002 entries, empty files | 0.104 s | 0.089 s | 1.2× |

Deleting *empty* files on local XFS is bound by the filesystem journal,
so parallelism buys little; with real file content (extents to free)
the win is ~3×, and on high-latency network storage — where every
unlink is a round trip — it grows with `-j 16` and beyond, as with all
p3m tools.
