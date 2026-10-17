# p3m-churn — estimate the change rate of a path

`p3m-churn` answers the question "how much does this path change?" before you
add it to a replication job. It scans the path repeatedly at a fixed interval,
compares each scan with the previous one, and when the run ends reports the
change rate, the data a replication would have to move, and the *hot* files and
directories that change most often.

## Synopsis

```
p3m-churn [OPTIONS] PATH...
```

Run it for as long as is representative — a day, ideally a week — then press
Ctrl-C (or use `-t`/`-c` to bound the run). The report is printed at the end.

```sh
# Sample every 5 minutes for 24 hours, saving the raw data
p3m-churn -i 300 -t 86400 -o scans.csv --hot-csv hot.csv /srv/share

# A quick look: 6 scans a minute apart, ignoring scratch files
p3m-churn -i 60 -c 6 --exclude '*.tmp' --exclude '.cache' /data
```

## How changes are detected

Every scan records, for each non-directory entry, its size, modification time
and change time. Against the previous scan an entry is:

| Class | Condition |
|-------|-----------|
| **created** | not present in the previous scan (a file recreated under an old path counts) |
| **modified** | size or mtime differs |
| **metadata only** | ctime differs but size and mtime do not (`chmod`, `chown`, rename…) |
| **deleted** | present before, absent now |

The first scan is the baseline and reports no changes. Directories are walked
but not themselves classified (their mtimes change whenever a child does).
Symbolic links are never followed; a link is tracked by its own times and
target length.

A rename shows up as one delete and one create. Changes between two scans are
collapsed — a file rewritten ten times within one interval counts once — so the
rates are **lower bounds**; shorten `--interval` to resolve bursts.

A directory that cannot be read during a scan does not make its files look
deleted: deletions are not counted for the directory group that failed, and the
errors are shown in the report.

## Options

| Option | Description |
|--------|-------------|
| `-i, --interval SECS` | Seconds between scan starts. Default `300`, minimum `0.1`. A scan that overruns the interval is followed immediately by the next. |
| `-c, --count N` | Stop after `N` scans in total, including the baseline (≥ 2). |
| `-t, --duration SECS` | Start no new scan after `SECS` seconds. |
| `-n, --top N` | Length of the hot-file and hot-directory lists. Default `20`. |
| `-d, --group-depth N` | Roll directory changes up to `N` levels below each `PATH`. Default `2`. |
| `--exclude PATTERN` | Skip entries whose name or full path matches the glob. Repeatable; excluded directories are not entered. |
| `--max-tracked N` | Remember at most `N` distinct changed file paths. Default `1000000`. Further changes are still counted in totals and directory roll-ups. |
| `-j, --threads N` | Scan threads. Default: number of online CPUs. |
| `-o, --output FILE` | One CSV row per scan (see below). |
| `--hot-csv FILE` | Every tracked changed file, most active first. |
| `-q, --quiet` | No per-scan line on stderr. |
| `-h, --help` / `-V, --version` | Usage / version. |

With no `--count` or `--duration` it runs until stopped.

## The report

```
Change rate (6 intervals observed)
                           avg/interval  peak/interval extrapolated/day
  files changed                     3.5              4          301,265
    created                         1.0              1           86,075
    modified                        1.5              2          129,113
    deleted                         1.0              1           86,075
    metadata only                   0.0              0                0
  data to re-replicate        50.81 KiB     103.56 KiB         4.17 GiB
  data deleted                      3 B            3 B       252.17 KiB

Hot files (changed in the most scans; paths relative to the scanned path)
    scans %scans   new   mod    bytes/chg    last size  path
        6   100%     0     6         29 B         47 B  logs/hot.log
        3    50%     0     3   101.56 KiB   103.52 KiB  db/warm.db

Hot directories (changes rolled up to depth 2)
     events   created  modified   deleted     data chg  path
         12         6         0         6         18 B  tmp
```

- **data to re-replicate** is the size of the created and modified files — what
  a whole-file replication tool would re-send. Block- or delta-based tools move
  less, so treat it as an upper bound for them.
- **extrapolated/day** scales the observed totals to 24 hours from the length of
  the window. The report warns when the window is under an hour: daily and
  weekly cycles will not be visible in a short sample.
- **Hot files** are ranked by the number of scans in which the file changed
  (`%scans` is that share of all intervals), then by data moved. A file at or
  near 100 % is written continuously.
- **Hot directories** roll every change — including deletions — up to
  `--group-depth` levels below the path. This is what exposes churn from
  temporary files whose names never repeat, which cannot appear as a hot *file*.
- **Replication notes** summarise what the numbers imply: how many files cause
  80 % of the change events, how many files change in at least 90 % of the
  intervals (candidates to exclude or to replicate from a snapshot), whether
  creates and deletes are of similar volume (temporary or rotated files), and
  whether the window is long enough to trust.

### Per-scan CSV (`-o`)

```csv
scan,start,duration_s,files,bytes,dirs,created,modified,metadata,deleted,bytes_changed,bytes_deleted,errors
1,2026-10-17T18:47:48Z,0.002,303,101100,7,0,0,0,0,0,0,0
2,2026-10-17T18:47:49Z,0.001,303,101107,7,1,1,0,1,15,3,0
```

Plot `bytes_changed` against `start` to see daily cycles.

### Hot-file CSV (`--hot-csv`)

`path,events,pct_scans,created,modified,bytes_changed,last_size,last_scan`

## Stopping

The first SIGINT/SIGTERM lets the scan in progress finish and then prints the
report — this is the normal way to end an open-ended run. A second signal
aborts the scan in progress (its totals are discarded; per-file and
per-directory changes it had already recorded remain in the hot lists). A third
quits at once with no report.

## Exit status

`0` a report was produced and no scan errors occurred · `1` fewer than two scans
completed, or scan errors occurred (the report is still printed) · `2` usage
error.

## Resource use

Per file, the state carried between scans is 40 bytes (a 64-bit path hash plus
size, mtime and ctime) in hash tables kept 35–70 % full — roughly 60–115 bytes
per file — plus the path of every file that has changed (bounded by
`--max-tracked`). A 254,000-file tree used about 33 MB; the baseline scan took
6.7 s cold and each later scan 0.2 s. Because paths are stored as 64-bit hashes
a collision between two different paths is theoretically possible but
vanishingly unlikely at any realistic file count.

## Behaviour notes

- Only changes made while the tool is running are seen; it cannot say what a
  path did before the first scan.
- mtime-based detection misses a rewrite that restores both the size and the
  modification time.
- Choose the interval for the question: a few minutes shows hot working sets; an
  hour or more is enough for the daily volume of a slowly-changing share.
