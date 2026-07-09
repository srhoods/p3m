# p3m-ch — parallel chmod + chown + chgrp

`p3m-ch` changes permissions, owner and group across directory trees in
a **single recursive, multi-threaded scan** — one traversal does the
work of `chmod -R`, `chown -R` and `chgrp -R` combined. Directories and
files can be given different modes in the same run (something plain
`chmod -R` cannot do at all), and only entries whose metadata actually
differs generate system calls, so re-running against an already
compliant tree is nearly free.

**By default nothing is changed**: `p3m-ch` performs a dry run and lists
the changes it *would* make. Add `--apply` to make them.

## Synopsis

```
p3m-ch [OPTIONS] PATH...
```

Unlike `p3m-ls`, the named `PATH`s themselves are processed as well as
their contents (matching `chmod -R` semantics). At least one of
`-m`, `-f`, `-d`, `-u`, `-g` is required.

## Options

### Mode and ownership

| Option | Description |
|--------|-------------|
| `-m, --mode MODE` | Mode to apply to both files and directories. |
| `-f, --file-mode MODE` | Mode for files only (overrides `-m` for files). |
| `-d, --dir-mode MODE` | Mode for directories only (overrides `-m` for directories). |
| `-u, --owner USER` | New owner — a name or numeric uid. `USER:GROUP` is also accepted as a shorthand for `-u USER -g GROUP`. |
| `-g, --group GROUP` | New group — a name or numeric gid. |

### Execution

| Option | Description |
|--------|-------------|
| `--apply` | Actually make the changes. Without it the run is a dry run — there is deliberately no `--dry-run` flag, since that is the default state. |
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write the CSV change list to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the console listing. Progress (on a terminal) and the end-of-run summary are still shown, and a `-o` file is still written in full — `-q` only silences stdout. |
| `-h, --help`, `-V, --version` | Usage / version. |

## MODE syntax

**Octal** — 1 to 4 octal digits: `775`, `0644`, `2755`, `4750`. An octal
mode is an exact replacement: all twelve permission bits (including
setuid, setgid and sticky) are set to precisely the given value.
*Note: this is more predictable than GNU `chmod`, which quietly
preserves a directory's set-id bits when given a numeric mode.*

**Symbolic** — the same grammar as `chmod(1)`:
`[ugoa]…[+-=][rwxXst…|u|g|o]`, with multiple clauses separated by
commas.

| Element | Meaning |
|---------|---------|
| `u` `g` `o` `a` | who: user, group, other, all (omitted = all, limited by the umask) |
| `+` `-` `=` | add, remove, set exactly |
| `r` `w` `x` | read, write, execute |
| `X` | execute only for directories, or files that already have some execute bit |
| `s` | setuid (with `u`), setgid (with `g`) |
| `t` | sticky bit |
| `u` `g` `o` after the operator | copy that class's current bits, e.g. `g=u` |

Examples: `u+rwx,g-w` · `o=rx` · `a+X` · `g=u` · `+t` · `ug=rw,o=r`

The symbolic engine is validated against GNU `chmod` (304-case test
matrix covering set-id bits, sticky, `X`, class copies and umask
handling), including chmod's rule that `=` preserves a directory's
set-id bits unless the clause mentions `s` explicitly.

## Dry run and output

Every entry that would change (and only those — compliant entries are
counted but not listed) produces one CSV row:

```csv
path,type,old_mode,new_mode,old_owner,new_owner,old_group,new_group,result
data/reports,d,0755,0775,root,alice,root,staff,pending
data/reports/q1.csv,f,0600,0664,root,alice,root,staff,pending
```

`new_mode`, `new_owner` and `new_group` are filled in only for the
properties that are actually changing. The `result` column is:

| Value | Meaning |
|-------|---------|
| `pending` | Dry run — this change would be made by `--apply` |
| `applied` | The change was made |
| `failed: <op>: <reason>` | The change could not be made (also counted as an error) |

Rows appear in non-deterministic order (parallel walk); sort after
import if needed. Fields containing commas or quotes are RFC 4180
quoted.

## Progress display

With `-o` or `-q` on a terminal, a live status block in the p3m house style is
shown and refreshed 8 times per second:

```
⠼ p3m-ch — parallel mode/ownership (apply)
  path      …/data/projects/renders
  output    changes.csv
  threads   4              action     apply
  files     8,214          dirs       830
  changes   6,120          unchanged  2,924
  rate      412,000 items/s errors    0
  elapsed   1.2s
```

A one-line summary is always printed on completion (with or without
`-o`, terminal or not):

```
✓ p3m-ch complete — dry-run · 11,002 scanned · 11,002 changes pending · 0 unchanged · 0 errors
  0.3s (36,700 items/s) → changes.csv
  re-run with --apply to make these changes
```

## Semantics and safety

- **Dry run by default.** Nothing is modified until `--apply` is given.
- **chown before chmod**: ownership is changed first because `chown`
  clears setuid/setgid; a following mode change then reinstates exactly
  what the mode spec requests.
- **Symbolic links** are never followed, so the walk cannot escape the
  tree or loop. Ownership *is* changed on links themselves
  (`lchown` semantics); modes never are (Linux does not support
  changing a symlink's mode) — a link therefore only appears in the
  output for ownership changes.
- **Only real differences cause syscalls.** An entry already matching
  the requested mode/owner/group is counted as `unchanged` and skipped.
  This makes `p3m-ch --apply` safe and cheap to re-run, and makes the
  dry run an exact preview of what `--apply` will do.
- Failures (e.g. `EPERM` chowning to another user without privilege)
  are per-entry: they are recorded in the CSV `result`, counted,
  summarised on stderr, and never abort the run.

Exit status: `0` success · `1` completed with errors · `2` usage error.

## Examples

```sh
# Preview normalising a web tree: dirs 0755, files 0644
p3m-ch -d 0755 -f 0644 /srv/www

# ...then actually do it
p3m-ch -d 0755 -f 0644 --apply /srv/www

# Full handover in ONE scan: new owner, new group, group-writable,
# dirs traversable — with an audit trail and live progress
p3m-ch -u alice -g staff -m g+rwX -o handover.csv --apply /data/project

# Remove world access everywhere, 16 threads (high-latency NFS)
p3m-ch -m o-rwx -j 16 --apply /mnt/nfs/secure

# Make every directory setgid so new files inherit the group
p3m-ch -d g+s --apply /shared

# Numeric ids and the USER:GROUP shorthand work too
p3m-ch -u 1000:988 --apply /opt/app
```

## Performance

Measured on the project's `testfolder` data set (11,002 entries, XFS,
4 cores, warm cache), each command performing an identical 11,002 real
metadata changes:

| Command | Time | vs p3m-ch |
|---------|------|-----------|
| `p3m-ch --apply -j 8` (split dir/file modes) | 0.016 s | — |
| `p3m-ch --apply -j 4` (split dir/file modes) | 0.033 s | — |
| `chmod -R` (single mode only) | 0.045 s | 2.8× slower |
| `find -type d/-type f -exec chmod {} +` (split modes) | 0.056 s | 3.5× slower |
| `chgrp -R` | 0.031 s | 1.8× slower (vs `-g` apply, 0.017 s) |

And because mode, owner and group are changed in the same pass, a
combined `chmod -R && chown -R && chgrp -R` job collapses from three
full traversals into one. As with all p3m tools, the advantage grows on
cold caches and network file systems; use `-j 16` or higher there.
