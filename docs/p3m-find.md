# p3m-find — parallel find

`p3m-find` searches directory trees with a pool of worker threads,
evaluating a `find(1)`-style expression against every entry. It
supports the classic operator grammar and the common tests with
GNU-find-compatible semantics — verified byte-identical against GNU
find across the full test matrix — and calls `stat(2)` lazily, so a
name-only search issues no stat calls at all.

There is deliberately **no `-delete` and no `-exec`**: p3m-find only
reads. Pipe matches to `xargs -0` (via `-print0`) or feed them to
`p3m-rm` / `p3m-ch`, which have their own dry-run safety nets.

## Synopsis

```
p3m-find [P3M-OPTIONS] [PATH...] [EXPRESSION]
```

p3m options come first, then paths, then the expression — the same
shape as find. With no `PATH`, the current directory is searched; with
no `EXPRESSION`, everything matches.

## p3m options

| Option | Description |
|--------|-------------|
| `-j, --threads N` | Worker threads, 1–512. Default: number of online CPUs. |
| `-o, --output FILE` | Write matches to `FILE` and show a live progress display. |
| `-q, --quiet` | Suppress the listing; progress and the summary — including the **match count** — still print, making `-q` a fast "how many?" mode. |
| `-h, --help`, `-V, --version` | Usage / version. |

## Tests

| Test | Matches entries whose… |
|------|------------------------|
| `-name P`, `-iname P` | name matches the glob `P` (case-insensitive with `i`) |
| `-path P`, `-ipath P` | full path matches the glob |
| `-regex P`, `-iregex P` | full path matches the POSIX **extended** regex, anchored to the whole path (equivalent to GNU find with `-regextype posix-extended`) |
| `-type T[,T…]` | type is one of `f d l b c p s`; combinable, e.g. `-type f,l` |
| `-size [+-]N[bckMG]` | size, **rounded up** to the unit before comparing, exactly as find does (default unit `b` = 512 bytes; `c` = exact bytes) |
| `-mtime/-atime/-ctime [+-]N` | age in 24-hour units (find rounding rules: `-mtime 1` = age in [24h, 48h), `+1` = older, `-1` = younger) |
| `-mmin/-amin/-cmin [+-]N` | age in minutes |
| `-newer FILE` | modified more recently than `FILE` (nanosecond precision) |
| `-user U`, `-group G` | owner / group, by name or numeric id |
| `-perm OCTAL` / `-perm -OCTAL` / `-perm /OCTAL` | permission bits: exact / all of / any of (octal only) |
| `-empty` | empty regular file or empty directory |
| `-true`, `-false` | constants |

Positional options accepted inside the expression, as in find:
`-maxdepth N`, `-mindepth N` (a `PATH` argument itself is depth 0),
`-print`, and `-print0`.

## Operators

Highest precedence first, identical to find:

```
( EXPR )                grouping (quote the parens from your shell)
! EXPR, -not EXPR       negation
EXPR EXPR, EXPR -a EXPR and (implicit between adjacent tests)
EXPR -o EXPR            or
```

## Output

Default: CSV with a `path` header, one row per match (RFC 4180 quoted,
so paths with commas stay parseable). With `-print0` (as a leading
option or in the expression): NUL-separated raw paths, no header —
drop-in for `xargs -0`.

**Match order is non-deterministic** (parallel walk); sort if you need
stability. The summary always reports the total match count.

```
✓ p3m-find complete — 2,481 matched · 10,000 files · 1,001 dirs scanned · 0 errors
  0.01s (1,297,406 items/s) → matches.csv
```

## Semantic notes

- Symbolic links are **never followed** (find's default `-P`
  behaviour): tests apply to the link itself, link loops cannot hang
  the walk, and `-type l` is how you find them.
- `-print0` is a global output mode, not an ordered action: writing
  `-name x -print0 -o -name y` will NUL-print matches of *both*
  branches (find would only print the first). Simple usage —
  `p3m-find PATH TESTS -print0` — behaves exactly like find.
- `-prune`, `-delete`, `-exec` and symlink-following (`-L`/`-H`) are
  intentionally not implemented.
- Depth limits: `-maxdepth` stops the walk (entries beyond it are
  never visited); `-mindepth` only filters output.
- Time tests take their reference time once at startup, like find.

## Error handling

Unreadable directories and unstattable entries are counted and
reported on stderr after the walk; an entry that cannot be statted
simply fails the tests that need the data. Exit status: `0` success ·
`1` completed with errors · `2` usage/expression error.

## Examples

```sh
# Classic cleanups
p3m-find /var/log -name '*.log' -mtime +30
p3m-find /data -type f -size +1G
p3m-find /srv -type d -empty

# Security sweep: setuid or setgid, not owned by root
p3m-find / -type f -perm /6000 ! -user root

# Complex: recent large artifacts, excluding sources
p3m-find /build \( -name '*.o' -o -name '*.so' \) -size +10M -mmin -120

# Count without listing (summary shows the total)
p3m-find -q /data -type f -newer /data/.last-backup

# Feed another tool safely
p3m-find --print0 /scratch -name 'tmp-*' -mtime +7 | xargs -0 -r du -ch

# Audit to a file with live progress
p3m-find -o matches.csv /data -user olduser
```

## Performance

Measured on the project's `testfolder` data set (1,001 directories,
10,000 × 1 MiB files, XFS, 4 cores, warm cache):

| Search | `find` | `p3m-find -j4` | Speed-up |
|--------|-------:|---------------:|---------:|
| `-name 'testfile00[1-5]*'` (no stats needed) | 0.009 s | 0.003 s | 3.0× |
| `-type f -size +512k` (stat per file) | 0.012 s | 0.006 s | 2.0× |
| `( -name … -o -size +1M ) -mtime -N` | 0.015 s | 0.007 s | 2.1× |

Because stat is lazy and short-circuit evaluation is preserved, an
expression like `-name '*.o' -size +1M` only stats files whose name
already matched — the same trick find uses, multiplied across
threads. As with all p3m tools the gap widens on high-latency
storage, where `-j 16` and beyond keep many metadata round trips in
flight.
