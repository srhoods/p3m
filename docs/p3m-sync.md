# p3m-sync — parallel one-way directory synchronisation

`p3m-sync` makes a destination directory an exact mirror of a source
directory, walking both trees in lockstep with a pool of worker threads.
It is dry-run by default: without `--apply` it only reports, as CSV, what
would change. Entries that exist only at the destination are left alone
unless `--delete` is given.

## Synopsis

```
p3m-sync [OPTIONS] SOURCE DEST
```

`DEST` is created if it does not exist. The *contents* of `SOURCE` (not
`SOURCE` itself) land directly in `DEST`, like `rsync -a SOURCE/ DEST/`.

## Options

| Option | Description |
|--------|-------------|
| `--apply` | Actually synchronise. The default is a dry run. |
| `--delete` | Remove entries that exist only at `DEST`, children before parents. Also lets an entry whose type differs between the sides (file vs directory, …) be replaced; without it such conflicts are skipped and reported. |
| `-c, --checksum` | For files of equal size, compare contents byte for byte instead of trusting the modification time. |
| `--owner` | Also synchronise owner and group, where permitted. |
| `--modify-window S` | Treat modification times within `S` seconds as equal, for file systems with coarse timestamps. Default `0` (nanosecond-exact). |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV to `FILE` and show a live progress display. |
| `-L, --lock-file FILE` | Refuse to run (exit `3`) if `FILE` already exists; otherwise create it for the duration of the run and remove it on exit. See [Lock file](#lock-file). |
| `-q, --quiet` | Suppress the console listing. Progress and the summary are still shown, and a `-o` file is still written. |
| `-h, --help` | Usage. |
| `-V, --version` | Version. |

## What counts as a change

| Situation | Action |
|-----------|--------|
| Entry missing at `DEST` | `create` |
| Regular file: size differs, or mtime differs | `update` (data copied) |
| Regular file with `-c`: same size, contents differ | `update` |
| Regular file with `-c`: same contents, mtime/mode differs | `metadata` |
| Same data but mode differs (or owner/group with `--owner`) | `metadata` (no data copied) |
| Symbolic link whose target differs | `update` |
| Device node whose `rdev` differs | `update` |
| Entry only at `DEST` (with `--delete`) | `delete` |
| Type differs (file vs directory, …) | needs `--delete` — otherwise `skipped: type conflict` |
| Socket at the source | `skipped: socket` |

Every copy preserves the source's modification time, so a synchronised tree
compares equal on the next run (the second run reports everything as
unchanged). Modes are applied exactly, regardless of the umask.

Hard links are not preserved: each name is copied as an independent file.

## Output

CSV, only for entries that change:

```csv
path,type,size,action,result
link,l,0,update,pending
top.txt,f,19,update,pending
a/big.bin,f,3000000,metadata,pending
a/fifo/inner,f,0,delete,pending
a/fifo,d,0,delete,pending
a/fifo,p,0,create,pending
```

`path` is relative to the roots (`.` is the root itself). `result` is
`pending` in a dry run and `done` once applied, or `failed: <reason>` /
`skipped: <reason>`. A deleted directory is listed after its contents.
Row order across directories is non-deterministic (parallel walk).

The summary always prints:

```
✓ p3m-sync complete — apply · 1 create · 3 update · 1 metadata · 8 delete · 5 unchanged · 0 errors
  0.1s (23,351 items/s · 32 B transferred · 40.54 KiB/s)
```

## Safety

- **Atomic updates.** Data, symlinks and special files are written to a
  temporary name (`.p3m-sync.<pid>.<n>`) in the destination directory and
  renamed into place. A destination file is never half-written, and a failed
  write leaves the previous version untouched. Leftover temporaries from a
  hard crash are ordinary destination-only entries, removed by the next
  `--delete` run.
- **Directory modes and times** are applied after the walk, deepest first, so a
  restrictive mode cannot cut off its own children.
- **Guards, checked before any work starts:** the filesystem root can never be
  the destination; source and destination may not be the same directory or
  contain one another; with `--delete`, an empty source is refused when the
  destination is not empty (a source that failed to mount cannot wipe the
  destination).
- **No deletion on a partial view.** If a source directory's listing cannot be
  read completely, nothing is deleted from the corresponding destination
  directory.
- Symbolic links are never followed; every directory is opened with
  `O_NOFOLLOW`.
- A destination directory without write permission cannot be emptied or
  populated; as with `rm -r`, the failures are reported as errors and the run
  continues.

## Interrupting (SIGINT / SIGTERM)

The first signal during `--apply` is acknowledged, no new directories are
started, and in-flight work finishes. Because updates are renamed into place
no destination file is left truncated. The summary reports `⚠ interrupted`
and the exit status is `130` (SIGINT) or `143` (SIGTERM). Re-run the same
command to complete the job. A second signal terminates immediately.

## Exit status

`0` success · `1` completed with errors · `2` usage error, invalid source or
destination, or a safety refusal · `130`/`143` interrupted.

## Lock file

`-L`/`--lock-file FILE` prevents overlapping runs. If `FILE` already exists the
tool prints the pid recorded in it (and whether that process is still running),
changes nothing, and exits `3`. Otherwise it creates `FILE` atomically
(`O_EXCL`, containing its pid), and removes it when the run ends — normally, on
error, after a graceful SIGINT/SIGTERM stop, and after a forced second signal.
Any number of sessions started with the same `FILE` are therefore mutually
exclusive; a dry run takes the lock too.

```sh
p3m-sync --apply -L /var/lock/nightly.lock SRC DEST   # a second copy exits 3
```

A lock left behind by a process that was killed outright (`SIGKILL`, power
loss) is never removed automatically — the message says when the recorded pid
is no longer running; delete the file once you are sure nothing else is active.
The lock is advisory and only as shared as the path: put it on a file system
visible to every session that should be excluded.

## Examples

```sh
# What would change?
p3m-sync /data/project /backup/project

# Mirror it, removing files that no longer exist at the source
p3m-sync --apply --delete -j 16 /data/project /mnt/nfs/project

# Distrust timestamps; keep the change list
p3m-sync -c -q -o changes.csv /data/project /backup/project

# Sync to a file system with 2-second timestamp granularity
p3m-sync --apply --modify-window 2 /data/project /mnt/fat/project
```

## Behaviour notes

- The first run against an empty destination is equivalent to `p3m-cp`; later
  runs transfer only what differs.
- `p3m-diff -c SOURCE DEST` is a good independent check after a sync.
