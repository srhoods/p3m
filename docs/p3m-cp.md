# p3m-cp — parallel copy

`p3m-cp` copies files and directory trees with a pool of worker
threads — a parallel `cp -R` with the p3m safety-first design. **By
default nothing is copied**: the tool performs a dry run listing
everything that would be copied and flagging destination conflicts;
add `--apply` to copy.

## Synopsis

```
p3m-cp [OPTIONS] SOURCE... DEST
```

Standard cp target rules apply:

- `DEST` is an existing directory → each `SOURCE` is copied *into* it
  as `DEST/basename(SOURCE)`.
- `DEST` does not exist → a single `SOURCE` is copied *to* the path
  `DEST` (rename-style copy).
- Multiple sources require `DEST` to be an existing directory.

## Options

| Option | Description |
|--------|-------------|
| `--apply` | Actually copy. Without it the run is a dry run — per p3m convention there is no `--dry-run` flag, because that is the default state. |
| `--overwrite` | Replace existing destination entries. Without it they are **skipped** and reported (cp `-n` semantics), never clobbered. |
| `-p, --preserve` | Preserve mode (including set-id bits), ownership (when permitted) and timestamps — like `cp -p`. Default: permission bits masked by the umask, fresh timestamps, as with plain `cp`. |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV listing to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress (on a terminal) and the end-of-run summary are still shown, and a `-o` file is still written in full — `-q` only silences stdout. |
| `-h, --help`, `-V, --version` | Usage / version. |

## Safety

- **Dry run by default.** The dry run walks the source, totals the
  bytes to copy, and checks every destination path, so conflicts are
  known *before* anything is written.
- **Existing destinations are never overwritten silently.** Without
  `--overwrite` they are skipped, counted, and called out in the
  summary. Existing destination *directories* are merged (contents
  copied into them), as with cp.
- **Copying a directory into itself is refused** before any work
  starts (`p3m-cp a a/b`, `p3m-cp a a` → exit 2), as is copying a file
  onto itself.
- **Symbolic links are never followed.** A link is recreated as a link
  with the same target (like `cp -P`), whether given as an argument or
  found in the tree — dangling links included. Destination files are
  created with `O_EXCL` (or unlink-then-create under `--overwrite`),
  so data is never written *through* a symlink planted at the
  destination.
- A file that fails mid-copy is unlinked rather than left truncated.

## What is copied

Regular files, directories, symbolic links, FIFOs, and device nodes
(devices need the required privilege). Sockets cannot be copied and are
reported as errors, matching cp. Hard-linked source files are copied as
independent files (as `cp -R` without `--preserve=links`).

Destination directories are created owner-writable while being
populated; final modes — and with `-p`, ownership and timestamps, which
writes inside would otherwise disturb — are applied in a deepest-first
pass after the copy, so read-only directory trees copy correctly.

## Output

CSV, one row per entry:

```csv
source,type,size,destination,result
data/run.log,f,10485760,/backup/data/run.log,pending
data/cfg,d,0,/backup/data/cfg,merge
data/cfg/old.ini,f,912,/backup/data/cfg/old.ini,exists
```

| `result` value | Meaning |
|----------------|---------|
| `pending` | Dry run: would be copied |
| `exists` | Dry run: destination exists — would be skipped (use `--overwrite`) |
| `overwrite` | Dry run with `--overwrite`: destination exists and would be replaced |
| `merge` / `merged` | Directory already exists at the destination; contents are merged |
| `copied` / `created` | Applied: file copied / directory created |
| `skipped: exists` | Applied without `--overwrite`: destination left untouched |
| `failed: <step>` | The error is also counted and summarised on stderr |

Row order is non-deterministic (parallel walk); fields are RFC 4180
quoted.

## Progress display

With `-o` or `-q` on a terminal, the p3m live status block is shown.
During `--apply` the size line shows live throughput:

```
⠼ p3m-cp — parallel copy
  path      …/testfolder/testdir0042
  output    copied.csv
  threads   4              action   apply
  files     8,214          dirs     830
  rate      96,000 items/s errors   0
  size      8.02 GiB · 412 MiB/s    elapsed  19.4s
```

Once all data is copied, `--apply` runs a final pass that applies each
created directory's permanent mode (and, with `-p`, ownership and
timestamps) deepest-first. On trees with many directories this phase
takes real time — `strace` would show a stream of `chmod`/`chown`
calls — so the display switches to report it rather than appearing to
hang: the `action` field reads `finalizing dir metadata`, the path
line tracks the directory being finalized, and the bottom line counts
it down:

```
⠧ p3m-cp — parallel copy
  path      …/dst/deep/subdir
  output    copied.csv
  threads   4              action   finalizing dir metadata
  files     10,000         dirs     40,201
  rate      100,355 items/s errors   0
  finalize  17,411 / 40,201 dirs    elapsed  21.3s
```

The summary always prints; a dry run adds a reminder that `--apply` is
required, and skipped conflicts add a `--overwrite` hint:

```
✓ p3m-cp complete — apply · 10,000 files · 1,002 dirs · 9.77 GiB copied · 0 skipped · 0 errors
  0.1s (110,205 items/s · 97.7 GiB/s) → copied.csv
```

## Error handling

Per-entry: an unreadable file or failed create is recorded in the CSV
`result`, counted, and summarised on stderr; the run continues. Skipped
existing destinations are **not** errors (exit 0). Exit status: `0`
success · `1` completed with errors · `2` usage error, invalid target,
or into-itself refusal.

## Examples

```sh
# What would this copy, and how big is it? (dry run)
p3m-cp /data/projects /backup/

# Do it, preserving metadata, with an audit CSV and live progress
p3m-cp --apply -p -o copied.csv /data/projects /backup/

# Refresh a mirror: copy over an existing tree, replacing changed files
p3m-cp --apply --overwrite /data/projects /backup/

# Rename-style copy of one tree
p3m-cp --apply /data/projects /data/projects.snapshot

# High-latency NFS target: more threads keep more copies in flight
p3m-cp --apply -j 16 /data/projects /mnt/nfs/backup/
```

## Performance

Measured on the project's `testfolder` data set (1,002 directories,
10,000 × 1 MiB files, 9.77 GiB) and a small-file tree (20,000 × 4 KiB
files in 200 directories); XFS, 4 cores, warm cache:

| Workload | `cp -r` | `p3m-cp --apply -j4` | Speed-up |
|----------|--------:|---------------------:|---------:|
| testfolder, same XFS (reflink clone) | 0.293 s | 0.100 s | 2.9× |
| 20,000 small files, cross-filesystem | 0.544 s | 0.174 s | 3.1× |
| testfolder, cross-filesystem (9.77 GiB real data) | 45.5 s | 57.3 s | **0.8× — slower** |

The pattern: **parallelism wins when the work is per-entry overhead**
— metadata operations, small files, reflink clones (`copy_file_range`
clones extents on reflink-capable XFS/Btrfs without moving data), and
high-latency network storage where every operation is a round trip. It
**loses when a single local device is the bottleneck**: bulk data for
the 9.77 GiB cross-filesystem copy moves at the disk's sequential write
speed regardless, and concurrent write streams add allocation and seek
contention (single-threaded `p3m-cp -j1` measured 54.4 s — the gap is
device scheduling, not the tool). For a few huge files on local disks,
plain `cp` or `-j1`/`-j2` is the right call; for trees of many files or
network targets, parallel wins by ~3×.

Implementation notes: file data is moved with `copy_file_range` (4 MiB
chunks — server-side copy on NFS, extent cloning on reflink
filesystems) falling back automatically to 1 MiB read/write loops where
unsupported (e.g. cross-filesystem on this kernel); destination files
are created `O_EXCL` with `fchown` before `fchmod` so set-id bits
survive; directory metadata is fixed up deepest-first after the walk.
