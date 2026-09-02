# p3m-stats — age and hot/cold statistics from a p3m-ls catalogue

`p3m-stats` reads a CSV catalogue produced by `p3m-ls -m full` (or
`-m standard`, for mtime-only stats) and reports access and
modification age statistics: the most recently accessed and most
recently modified files, and how file count and bytes break down by
"not touched in over X" age buckets.

It is a companion to `p3m-ls`, not a directory walker itself — point it
at a CSV file (or pipe one in) and it produces a report. The CSV is
streamed once, row by row; row count does not bound memory use, so
catalogues with tens of millions of rows are fine.

## Synopsis

```
p3m-stats [OPTIONS] [FILE]
```

With no `FILE`, reads from stdin. A typical run:

```sh
p3m-ls -m full -o catalogue.csv /data
p3m-stats catalogue.csv
```

or streamed straight through without an intermediate file:

```sh
p3m-ls -m full /data | p3m-stats -n 50
```

## Options

| Option | Description |
|--------|-------------|
| `-n, --top N` | Show the N most recently accessed and N most recently modified files. Default: 20. `-n 0` disables the top lists (histograms only). |
| `-o, --output FILE` | Write the report to `FILE` instead of stdout. |
| `--csv-summary FILE` | Also write the age histogram (both atime and mtime) as CSV to `FILE`. |
| `--now TIMESTAMP` | Treat `TIMESTAMP` (ISO 8601 UTC, e.g. `2026-09-02T00:00:00Z`) as "now" instead of the wall-clock time, for reproducible reports. |
| `-q, --quiet` | Suppress the report on stdout. Only useful combined with `-o` and/or `--csv-summary`. |
| `-h, --help` | Show usage and exit. |
| `-V, --version` | Show version and exit. |

## Input requirements

`p3m-stats` reads the CSV header to locate the `path`, `size`, `atime`
and `mtime` columns by name — column order doesn't matter, and extra
columns (as in `-m full`'s `mode`, `perms`, `nlink`, …) are ignored. At
least one of `atime`/`mtime` must be present:

- **`p3m-ls -m full`** — has both `atime` and `mtime`; full report.
- **`p3m-ls -m standard`** — has `mtime` (and `size`) but no `atime`;
  modification-age report only, no access-age section.
- **`p3m-ls -m basic`** — path only; rejected (no timestamp columns).

Rows are parsed per RFC 4180 (matching `p3m-ls`'s own CSV output), so
quoted fields with embedded commas, quotes or newlines are handled.
Directory rows (`type` column, when present, `= d`) are excluded from
the report — the age of a directory's own metadata isn't a useful
"coldness" signal — but are still counted in row parsing. A row missing
a required column, or with an unparsable timestamp, is skipped and
counted; the run still completes and exits `1` (rather than aborting)
if any rows were skipped, so a mostly-good catalogue with a handful of
bad rows still produces a report.

## Output

### Top lists

The N most recently accessed and N most recently modified files, newest
first, with their timestamp and size:

```
Most recently accessed (atime)
  2026-09-02T14:03:11Z    128.4 KiB  /data/incoming/report.csv
  2026-09-02T09:41:02Z      2.1 MiB  /data/logs/app.log
  ...
```

These are computed with a bounded max-heap of size N — memory for the
top lists is `O(N)`, independent of how many rows are in the input.

### Age histograms

For each of `atime` and `mtime`, file count and total bytes are bucketed
by age relative to "now" (wall-clock time, or `--now`):

```
Access age (atime) — data not read in over X
  age               files        bytes  % bytes
  <= 1 day          1,204     512.3 MiB    0.40%  [#                       ]
  <= 1 week         8,331       3.91 GiB    3.10%  [###                     ]
  <= 1 month       22,918      10.7 GiB     8.45%  [########                ]
  <= 3 months      41,207      19.3 GiB    15.28%  [###############         ]
  <= 6 months      38,662      18.1 GiB    14.33%  [##############          ]
  <= 1 year        51,004      23.9 GiB    18.92%  [##################      ]
  <= 2 years       44,318      20.7 GiB    16.41%  [################        ]
  <= 5 years       36,552      17.1 GiB    13.55%  [#############           ]
  > 5 years        24,804      11.6 GiB     9.17%  [#########               ]
  total           269,000     126.0 GiB
```

Buckets are cumulative age brackets (`<= 1 month` means "1 week to 1
month old", not "younger than 1 month" — read the table top to bottom as
increasing age, each row disjoint from the ones above it). A file whose
timestamp is after the "now" reference (clock skew between the machine
that ran `p3m-ls` and `--now`, or an unset `--now`) is reported
separately as `future` rather than folded into a bucket.

### `--csv-summary`

Writes the same histogram data as CSV (`kind,bucket,files,bytes`, one
row per bucket per kind) for further processing or charting:

```csv
kind,bucket,files,bytes
atime,"<= 1 day",1204,537214976
atime,"<= 1 week",8331,4198761349
...
mtime,future,0,0
```

## Performance

Single-threaded (parsing is inherently sequential — CSV rows have no
useful independent unit larger than a line to parallelise across
threads for a one-shot aggregation): on a 3,000,000-row `-m full`
catalogue, `p3m-stats` completes in about 5 seconds with resident
memory in the low single-digit megabytes. Memory is dominated by two
fixed-size top-N heaps (`2 × N` entries) and per-column line buffers —
it does not grow with the number of rows in the input.

## Examples

```sh
# Full report, default top-20 lists
p3m-ls -m full -o cat.csv /data && p3m-stats cat.csv

# Streamed, no intermediate file, top 50
p3m-ls -m full /data | p3m-stats -n 50

# Just the histograms (no top lists), written to a file
p3m-stats -n 0 -o report.txt cat.csv

# Reproducible report pinned to a specific "now"
p3m-stats --now 2026-09-01T00:00:00Z cat.csv

# Histogram data only, for a spreadsheet or dashboard
p3m-stats -q --csv-summary aging.csv cat.csv
```

## Behaviour notes

- Only regular-file-shaped rows contribute to the report; directory
  rows are skipped (counted, not reported on). Other types (symlinks,
  devices, etc.) are treated like regular files if they carry a `type`
  other than `d`, or included unconditionally when the input has no
  `type` column at all.
- `p3m-ls` output row order is non-deterministic (parallel walk); this
  does not affect `p3m-stats`, which aggregates order-independently.
- Timestamps are parsed with second resolution for bucketing; the top
  lists use the full nanosecond precision `p3m-ls -m full` provides to
  break ties between files with the same second.
