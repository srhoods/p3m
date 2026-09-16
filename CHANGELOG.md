# Changelog

All notable changes to the p3m tool suite, generated retrospectively from
the git history. Dates are commit dates (UTC); entries are newest first.
Each tool carries its own version number (see `--version`) rather than
the project being released as a whole — versions are noted per tool
below where a commit bumped one.

## 2026-09-16

### p3m-cp, p3m-mv — finish in-flight transfers on SIGINT/SIGTERM

`p3m-cp` → 1.1.0, `p3m-mv` → 1.1.0

Previously the only signal handling in the suite was cosmetic (restore
the terminal cursor, then re-raise with default disposition) — Ctrl-C
during `--apply` could kill a worker mid-write and leave a torn
destination file, or in `p3m-mv`'s cross-device path, delete a source
before its copy had actually finished.

Adds a shared graceful-stop mechanism to `p3mcore`: the first
SIGINT/SIGTERM sets a flag and prints one notice; `p3m_stack_pop` (the
single choke point both tools already pop work through) starts
returning `NULL` as though the walk had finished, so workers stop
picking up new directories but always finish whatever file copy,
rename, or copy+delete they are already in the middle of — never
truncating a destination or deleting a source out from under an
unfinished copy. A second signal restores default disposition and
re-raises for an immediate forced exit. `p3m_progress_start` defers its
own signal handling when a stop handler is already installed, so the
live progress display and the graceful stop use one signal path
without fighting over SIGINT/SIGTERM.

Only `p3m-cp` and `p3m-mv` opt in; other tools are unaffected. A
stopped run prints "interrupted" instead of "complete" and exits
130/143 (128+signal) instead of 0/1, so scripts can distinguish an
intentional stop from a completed run or an error.

## 2026-09-08

### p3m-stats — largest-files list and extension breakdown

`p3m-stats` → 1.1.0

Adds a "Largest files" top-N list (bounded max-heap keyed on size,
generalizing the existing recency-list heap with a comparator) and a
"Breakdown by extension" histogram (bounded open-addressing hash table,
top-N by bytes with an `(other)` overflow row) alongside the existing
recency lists and age histograms. New `-e`/`--top-ext` option controls
the extension breakdown size independently of `-n`. Both additions keep
memory bounded independent of row count.

## 2026-09-02

### p3m-stats — new tool: age and hot/cold statistics from a p3m-ls catalogue

`p3m-stats` 1.0.0 (new)

Streams a `p3m-ls -m full`/`-m standard` CSV catalogue once and reports
the most recently accessed/modified files plus atime/mtime age
histograms by file count and bytes. Pure in-memory streaming (no
sqlite3 dev headers available on the target system, and every other
p3m tool has zero external dependencies by design) — memory is bounded
by two fixed-size top-N heaps and does not grow with row count, so
catalogues with tens of millions of rows are fine.

## 2026-07-13

### Rename p3m-comp to p3m-diff

Renamed the directory-comparison tool from `p3m-comp` to `p3m-diff`
across the source, docs, Makefile and tool indexes. Internal version
macro and helper names updated for consistency; behaviour unchanged.

## 2026-07-12

### p3m-diff — new tool: parallel directory comparison with content verification

`p3m-diff` 1.0.0 (new, as `p3m-comp`)

Compares two trees in lockstep with worker threads: entry names, types,
sizes and metadata (mode, owner, group, mtime at nanosecond precision)
by default; `-c` additionally verifies file contents. The content
check is byte-exact rather than checksum-based — both files are read
in 1 MiB chunks and compared directly, stopping at (and reporting) the
first differing byte — and is gated on size, since files whose sizes
differ are never opened.

Symlinks are compared by target/owner/group and never followed.
Differences stream as CSV (`path,difference,left,right`); the summary
gives a per-category breakdown and a plain-language verdict: "very
likely identical" (default), "identical, verified byte-for-byte"
(`-c`), "trees differ", or withheld when errors made coverage
incomplete. Exit codes are diff-like: 0 same, 1 differ, 2 trouble.

## 2026-07-10

### Show finalize-phase progress in p3m-cp and p3m-mv

After the parallel walk, `--apply` runs a serial post-pass: directory
modes/ownership/timestamps deepest-first (and, for `mv`, removal of
emptied source directories). On trees with many directories this took
real time with no console feedback, so the progress display appeared
to hang on its last frame. The display now reports the phase: the
action field switches to "finalizing dir metadata", the path line
tracks the directory being finalized, and the size line becomes a
"finalize N / M dirs" counter, which also feeds the items/s rate.

### Add MIT LICENSE.md

The README already declared the project MIT-licensed; added the full
licence text and linked it from the Licence section.

### p3m-find — new tool: parallel find with the classic expression grammar

`p3m-find` 1.0.0 (new)

Recursive-descent parser over the `find(1)` grammar (`! ( ) -a -o`,
implicit and) with the common tests: `-name`/`-iname`, `-path`/`-ipath`,
`-regex`/`-iregex` (posix-extended, whole-path anchored), `-type`,
`-size` (find's round-up rule), `-mtime`/`-atime`/`-ctime` and minute
variants (find's interval semantics), `-newer`, `-user`/`-group`,
`-perm` (`-`/`/` prefix forms), `-empty`, `-mindepth`/`-maxdepth`,
`-print0`. `stat(2)` is lazy: tests that need it trigger it once per
entry, so name-only searches issue no stat calls; short-circuit
evaluation is preserved.

Read-only by design — no `-delete` or `-exec`; matches print as CSV or
NUL-separated for `xargs -0` / `p3m-rm` / `p3m-ch`. Symlinks never
followed (link loops cannot hang the walk).

### p3m-mv — new tool: parallel move with rename fast path and tree merging

`p3m-mv` 1.0.0 (new)

Same-filesystem subtrees move with a single `rename(2)` (NOREPLACE
unless `--overwrite`); cross-device entries are copied in parallel with
full metadata preservation and unlinked only after the copy succeeds;
existing destination directories are merged (`mv` refuses this), with
each conflict-free subtree still moving by rename. Emptied source
directories are removed deepest-first; a directory still holding
skipped entries is kept, so a skipped conflict never loses data.

Suite safety: dry run by default with rename/copy prediction,
`--overwrite` to replace, hard non-overridable guard against moving
`/`, `.`/`..` refused, into-itself moves refused, symlinks never
followed.

Extracted the fd-to-fd copy loop (`copy_file_range` + read/write
fallback) from `p3m-cp` into `p3mcore` as `p3m_copy_fd`, now shared
between the two tools.

### p3m-cp — new tool: parallel copy, dry-run by default

`p3m-cp` 1.0.0 (new)

Recursive copy like `cp -R` with the suite's safety design: dry run
unless `--apply`, existing destinations skipped unless `--overwrite`,
into-itself copies refused up front, symlinks recreated (never
followed), `O_EXCL` destination creation so data is never written
through a planted symlink. `-p` preserves mode/ownership/timestamps;
directory metadata is fixed deepest-first after the walk so read-only
trees copy correctly. Data moves via `copy_file_range` (reflink/server-
side copy where available) with a read/write fallback.

### p3m-du — new tool: parallel disk usage with GNU du-compatible options

`p3m-du` 1.0.0 (new)

Supports `-s -c -d -h --si -X --exclude -B -b` with output matching GNU
`du` exactly (block rounding, human-readable ceiling rules, hard-link
dedup via a shared dev/ino table, symlinks never followed), plus the
standard p3m `-j`/`-o`/`-q` options and live progress. Per `du`
convention `-h` is human-readable here; help is `--help` only.

Sizes aggregate bottom-up through a chunked directory-node store: a
child node's index always exceeds its parent's, so one reverse sweep
after the parallel walk completes all totals in O(n).

## 2026-07-09

### Add -q/--quiet to all tools: suppress the console listing

`p3m-ls` → 1.2.0, `p3m-ch` → 1.2.0, `p3m-rm` → 1.1.0

`-q` silences the CSV listing on stdout while keeping everything else:
the live progress display now activates for `-q` as well as `-o` (the
output field shows "none (-q)"), the end-of-run summary always prints,
and a `-o` file is still written in full — `-q` only affects the
console. Useful for count-only scans and scripted `--apply` runs.

### p3m-rm — new tool: parallel recursive remove with hard root guard

`p3m-rm` 1.0.0 (new)

Parallel `rm -rf` with a safety-first design. Dry run by default lists
everything that would be removed as CSV; `--apply` deletes. Arguments
are literal paths or glob masks (expanded with `glob(3)`).

Safety: a hard, non-overridable guard resolves every target with
`realpath` before any work starts, and anything resolving to `/` aborts
the whole run (`.` and `..` are refused too); symlinks are never
followed — links are unlinked, targets untouched, and each directory is
opened with `O_NOFOLLOW` so a directory swapped for a symlink mid-run
cannot redirect deletion (TOCTOU race); the guard pass completes before
the first unlink (fail-fast).

Apply runs in two phases: the parallel walk unlinks files as it scans
and collects directories; directories are then `rmdir`'d deepest level
first, level-parallel with a barrier, so parents never race children.
`ENOENT` is treated as already-removed (`rm -f` semantics).

### p3m-ch — drop --dry-run/-n flag; capture errno before reporting

`p3m-ch` → 1.1.0

Suite convention: tools that default to dry-run take only `--apply` —
a dry-run flag is redundant with the default state. Also saved errno
before calling reporting helpers in `do_change` so the failure message
can no longer be clobbered.

### p3m-ch — new tool: parallel chmod + chown + chgrp in a single scan

`p3m-ch` 1.0.0 (new)

One multi-threaded traversal changes mode, owner and group together.
Dry run is the default; `--apply` makes the changes. Only entries whose
metadata actually differs generate syscalls, so re-runs on compliant
trees are nearly free and the dry run is an exact preview.

Features: `-m`/`--mode` for both, `-f`/`--file-mode` and
`-d`/`--dir-mode` for separate file/directory permissions in the same
pass; modes as exact octal (`0775`) or `chmod(1)` symbolic grammar
(`u+rwx,g-w o=rx a+X g=u +t`); `-u`/`--owner` and `-g`/`--group` (names
or numeric ids, `USER:GROUP` form); chown before chmod so set-id bits
land as specified; `lchown` on symlinks, never follows links, never
chmods them.

### Extract shared engine into p3mcore

Moved the parallel walk stack, buffered thread-safe output, uid/gid
name caches (plus name→id resolvers), error accounting, current-path
tracker, progress-display scaffolding and formatting helpers out of
`p3m-ls` into `src/p3mcore.{c,h}` so upcoming tools reuse them. `p3m-ls`
now supplies only its scan logic, CSV schema and progress layout.

### p3m-ls — new tool: parallel recursive directory lister

`p3m-ls` 1.0.0 (new)

Multi-threaded tree walk (shared LIFO work queue, idle-detection
termination) emitting CSV at three detail levels:

- `basic` — path only; classifies via `d_type` so no `stat` calls are
  needed
- `standard` — path, type, size, mode, owner/group, mtime
- `full` — every `stat(2)` field, symbolic perms, nanosecond-precision
  times

`-o FILE` writes output to a file and shows a live progress display
(current path, threads, mode, output, file/dir counts, scan rate,
errors, aggregate size, elapsed) refreshed 8×/s on a TTY. `-t`/`--type`
filter (`f d l b c p s`, combinable) and `--no-dirs`; `-j`/`--threads`
worker count, default the number of online CPUs. RFC 4180 CSV quoting,
per-thread 1 MiB output buffers, cached uid/gid name resolution,
`fstatat` relative to open directory fds.

### Initial project skeleton for p3m

Top-level Makefile (tools register themselves as they are added);
README with project overview, build and test instructions; `docs/`
directory for per-tool user documentation; `.gitignore` excluding
generated test data and build artifacts.
