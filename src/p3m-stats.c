/*
 * p3m-stats — age / hot-cold statistics from a p3m-ls full-mode catalogue
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Streams a CSV file produced by `p3m-ls -m full` (or `-m standard` for
 * mtime-only stats) and reports, without materialising the input:
 *
 *   - the N most recently accessed files (atime) and N most recently
 *     modified files (mtime), with their paths and timestamps
 *   - an age histogram for both atime and mtime: how many files/bytes
 *     fall in each "not touched in over X" bucket
 *
 * Designed for catalogues with tens of millions of rows: the CSV is
 * read once, line by line, and only fixed-size aggregate state plus two
 * bounded top-N heaps are kept in memory — the row count does not bound
 * memory use.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define P3M_STATS_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* age buckets                                                          */
/* ------------------------------------------------------------------ */

/* Each bucket i holds items with age in (bound[i-1], bound[i]], where
 * bound[-1] is 0. The last bucket is open-ended. Ages are in seconds. */
static const int64_t bucket_bound[] = {
    1LL * 86400,            /* 1 day    */
    7LL * 86400,            /* 1 week   */
    30LL * 86400,           /* 1 month  */
    90LL * 86400,           /* 3 months */
    182LL * 86400,          /* 6 months */
    365LL * 86400,          /* 1 year   */
    2LL * 365 * 86400,      /* 2 years  */
    5LL * 365 * 86400,      /* 5 years  */
};
static const char *bucket_label[] = {
    "<= 1 day",
    "<= 1 week",
    "<= 1 month",
    "<= 3 months",
    "<= 6 months",
    "<= 1 year",
    "<= 2 years",
    "<= 5 years",
    "> 5 years",
};
#define N_BOUNDS ((int)(sizeof bucket_bound / sizeof bucket_bound[0]))
#define N_BUCKETS (N_BOUNDS + 1)

typedef struct {
    uint64_t count[N_BUCKETS];
    uint64_t bytes[N_BUCKETS];
    uint64_t future;        /* timestamp is after "now" (clock skew)   */
    uint64_t future_bytes;
    int64_t  oldest_age;    /* seconds; -1 if no data                  */
    int64_t  newest_age;    /* seconds; -1 if no data                  */
} age_hist;

static void hist_add(age_hist *h, int64_t age, uint64_t size)
{
    if (age < 0) {
        h->future++;
        h->future_bytes += size;
        return;
    }
    int b = N_BOUNDS;
    for (int i = 0; i < N_BOUNDS; i++) {
        if (age <= bucket_bound[i]) { b = i; break; }
    }
    h->count[b]++;
    h->bytes[b] += size;
    if (h->oldest_age < 0 || age > h->oldest_age) h->oldest_age = age;
    if (h->newest_age < 0 || age < h->newest_age) h->newest_age = age;
}

/* ------------------------------------------------------------------ */
/* bounded top-N min-heap, keyed on timestamp (keep the N largest)      */
/* ------------------------------------------------------------------ */

typedef struct {
    time_t   ts;
    long     ns;
    uint64_t size;
    char    *path;   /* malloc'd */
} top_item;

typedef struct {
    top_item *items;
    int       n, cap;
} top_heap;

static void heap_init(top_heap *h, int cap)
{
    h->items = cap ? calloc((size_t)cap, sizeof *h->items) : NULL;
    h->n = 0;
    h->cap = cap;
}

static int item_before(const top_item *a, const top_item *b)
{
    /* true if a is older (smaller) than b — min-heap ordering */
    if (a->ts != b->ts) return a->ts < b->ts;
    return a->ns < b->ns;
}

static void heap_sift_down(top_heap *h, int i)
{
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, small = i;
        if (l < h->n && item_before(&h->items[l], &h->items[small])) small = l;
        if (r < h->n && item_before(&h->items[r], &h->items[small])) small = r;
        if (small == i) return;
        top_item tmp = h->items[i];
        h->items[i] = h->items[small];
        h->items[small] = tmp;
        i = small;
    }
}

static void heap_sift_up(top_heap *h, int i)
{
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!item_before(&h->items[i], &h->items[p])) return;
        top_item tmp = h->items[i];
        h->items[i] = h->items[p];
        h->items[p] = tmp;
        i = p;
    }
}

/* offer path/ts; heap takes ownership of path only if it keeps it */
static void heap_offer(top_heap *h, const char *path, time_t ts, long ns,
                        uint64_t size)
{
    if (h->cap == 0) return;
    if (h->n < h->cap) {
        top_item *it = &h->items[h->n];
        it->ts = ts; it->ns = ns; it->size = size;
        it->path = strdup(path);
        h->n++;
        heap_sift_up(h, h->n - 1);
        return;
    }
    top_item cand = { .ts = ts, .ns = ns, .size = size };
    if (!item_before(&h->items[0], &cand)) return; /* not newer than min */
    free(h->items[0].path);
    h->items[0] = cand;
    h->items[0].path = strdup(path);
    heap_sift_down(h, 0);
}

/* sorted newest-first; caller must free() the returned array (not the
 * strings inside — those are still owned by the heap) */
static top_item *heap_sorted(top_heap *h)
{
    top_item *out = malloc((size_t)h->n * sizeof *out);
    memcpy(out, h->items, (size_t)h->n * sizeof *out);
    /* simple insertion sort by descending ts,ns — n is small (top-N) */
    for (int i = 1; i < h->n; i++) {
        top_item key = out[i];
        int j = i - 1;
        while (j >= 0 && item_before(&out[j], &key)) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* CSV parsing (RFC 4180 subset matching p3m-ls output)                 */
/* ------------------------------------------------------------------ */

#define LINEBUF_INIT (64 * 1024)

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} strbuf;

static void sb_init(strbuf *s, size_t cap)
{
    s->buf = malloc(cap);
    s->cap = cap;
    s->len = 0;
}

static void sb_reset(strbuf *s) { s->len = 0; }

static void sb_putc(strbuf *s, char c)
{
    if (s->len + 1 >= s->cap) {
        s->cap *= 2;
        s->buf = realloc(s->buf, s->cap);
    }
    s->buf[s->len++] = c;
}

/* Reads one CSV record from f into fields[0..nfields-1], each a pointer
 * into per-column strbufs (caller-owned, reused across calls). Handles
 * quoted fields with embedded commas/quotes/newlines per RFC 4180.
 * Returns the number of fields found, 0 at EOF, -1 on read error. */
static int csv_read_record(FILE *f, strbuf *fields, int maxfields)
{
    int nf = 0;
    int c = fgetc(f);
    if (c == EOF) return 0;

    sb_reset(&fields[0]);
    bool in_quotes = false;
    bool any_char = false;

    for (;;) {
        if (c == EOF) {
            if (!any_char && nf == 0) return 0;
            nf++;
            break;
        }
        any_char = true;
        if (in_quotes) {
            if (c == '"') {
                int c2 = fgetc(f);
                if (c2 == '"') {
                    sb_putc(&fields[nf], '"');
                } else {
                    in_quotes = false;
                    ungetc(c2, f);
                }
            } else {
                sb_putc(&fields[nf], (char)c);
            }
        } else {
            if (c == '"' && fields[nf].len == 0) {
                in_quotes = true;
            } else if (c == ',') {
                sb_putc(&fields[nf], '\0'); fields[nf].len--;
                nf++;
                if (nf >= maxfields) {
                    /* discard remainder of the line, keep parsing sane */
                    int cc;
                    while ((cc = fgetc(f)) != EOF && cc != '\n') {}
                    return nf;
                }
                sb_reset(&fields[nf]);
            } else if (c == '\n') {
                sb_putc(&fields[nf], '\0'); fields[nf].len--;
                nf++;
                break;
            } else if (c == '\r') {
                /* swallow bare CR (CRLF line endings) */
            } else {
                sb_putc(&fields[nf], (char)c);
            }
        }
        c = fgetc(f);
    }
    return nf;
}

/* Parse "YYYY-MM-DDTHH:MM:SS[.fraction]Z" (as emitted by p3m_fmt_ts) into
 * epoch seconds (UTC) + nanoseconds. Returns 0 on success, -1 on a
 * malformed string. */
static int parse_iso8601(const char *s, time_t *sec, long *nsec)
{
    struct tm tm = {0};
    int frac = 0;
    char fracbuf[16] = {0};

    int n = sscanf(s, "%d-%d-%dT%d:%d:%d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n != 6) return -1;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;

    const char *dot = strchr(s, '.');
    if (dot) {
        dot++;
        int fl = 0;
        while (isdigit((unsigned char)dot[fl]) && fl < 9) {
            fracbuf[fl] = dot[fl];
            fl++;
        }
        for (int i = fl; i < 9; i++) fracbuf[i] = '0';
        fracbuf[9] = '\0';
        frac = atoi(fracbuf);
    }

    *sec = timegm(&tm);
    if (*sec == (time_t)-1 && errno == EOVERFLOW) return -1;
    *nsec = frac;
    return 0;
}

/* ------------------------------------------------------------------ */
/* configuration                                                        */
/* ------------------------------------------------------------------ */

static struct {
    int          topn;
    const char  *inpath;   /* NULL => stdin */
    const char  *outpath;  /* NULL => stdout */
    const char  *csv_out;  /* histogram CSV dump, or NULL */
    time_t       now;
    bool         now_set;
    bool         quiet;
} g = { .topn = 20 };

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-stats [OPTIONS] [FILE]\n"
"\n"
"Reads a CSV catalogue produced by `p3m-ls -m full` (atime/mtime require\n"
"full mode; standard mode works for mtime-only stats) and reports access\n"
"and modification age statistics: the most recently accessed and most\n"
"recently modified files, and how much data falls into each\n"
"'not touched in over X' age bucket, by file count and by bytes.\n"
"\n"
"With no FILE, reads from stdin. The CSV header is used to locate the\n"
"path/size/atime/mtime columns, so column order does not matter and\n"
"extra columns are ignored.\n"
"\n"
"Options:\n"
"  -n, --top N            show the N most recent files per list\n"
"                         (default: 20; 0 disables the top lists)\n"
"  -o, --output FILE      write the report to FILE instead of stdout\n"
"      --csv-summary FILE also write the age histogram as CSV to FILE\n"
"      --now TIMESTAMP    treat TIMESTAMP (ISO 8601 UTC, e.g.\n"
"                         2026-09-02T00:00:00Z) as \"now\" instead of the\n"
"                         current time — for reproducible reports\n"
"  -q, --quiet            suppress the report on stdout (only useful\n"
"                         with -o and/or --csv-summary)\n"
"  -h, --help              show this help and exit\n"
"  -V, --version           show version and exit\n"
"\n"
"Examples:\n"
"  p3m-ls -m full -o cat.csv /data && p3m-stats cat.csv\n"
"  p3m-ls -m full /data | p3m-stats -n 50\n",
    to);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */

enum { OPT_CSV_SUMMARY = 1000, OPT_NOW };

static const struct option lopts[] = {
    { "top",         required_argument, NULL, 'n' },
    { "output",      required_argument, NULL, 'o' },
    { "csv-summary", required_argument, NULL, OPT_CSV_SUMMARY },
    { "now",         required_argument, NULL, OPT_NOW },
    { "quiet",       no_argument,       NULL, 'q' },
    { "help",        no_argument,       NULL, 'h' },
    { "version",     no_argument,       NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

/* human-readable age: "3.2 days", "5.1 months", "2.3 years" */
static char *fmt_age(int64_t secs, char out[32])
{
    double d = (double)secs / 86400.0;
    if (d < 1.0)
        snprintf(out, 32, "%.1f hours", d * 24.0);
    else if (d < 60.0)
        snprintf(out, 32, "%.1f days", d);
    else if (d < 730.0)
        snprintf(out, 32, "%.1f months", d / 30.0);
    else
        snprintf(out, 32, "%.1f years", d / 365.0);
    return out;
}

static void print_bar(FILE *out, uint64_t bytes, uint64_t max_bytes, int width)
{
    int filled = max_bytes ? (int)((double)bytes / (double)max_bytes * width) : 0;
    if (filled > width) filled = width;
    fputc('[', out);
    for (int i = 0; i < width; i++)
        fputc(i < filled ? '#' : ' ', out);
    fputc(']', out);
}

static void print_hist(FILE *out, const char *title, const age_hist *h,
                        uint64_t total_files, uint64_t total_bytes)
{
    char nb[32], sb[32];
    fprintf(out, "\n%s%s%s\n", C_BOLD, title, C_RESET);
    fprintf(out, "  %-12s %10s %12s %8s  %s\n",
            "age", "files", "bytes", "% bytes", "");

    uint64_t max_bytes = h->bytes[0];
    for (int i = 1; i < N_BUCKETS; i++)
        if (h->bytes[i] > max_bytes) max_bytes = h->bytes[i];

    for (int i = 0; i < N_BUCKETS; i++) {
        double pct = total_bytes ? 100.0 * (double)h->bytes[i] / (double)total_bytes : 0.0;
        fprintf(out, "  %-12s %10s %12s %7.2f%%  ",
                bucket_label[i],
                p3m_fmt_u64(h->count[i], nb),
                p3m_fmt_size(h->bytes[i], sb),
                pct);
        print_bar(out, h->bytes[i], max_bytes, 24);
        fputc('\n', out);
    }
    if (h->future) {
        fprintf(out, "  %-12s %10s %12s  %s(timestamp after \"now\" — clock skew?)%s\n",
                "future", p3m_fmt_u64(h->future, nb),
                p3m_fmt_size(h->future_bytes, sb), C_DIM, C_RESET);
    }
    fprintf(out, "  %-12s %10s %12s\n", "total",
            p3m_fmt_u64(total_files, nb), p3m_fmt_size(total_bytes, sb));

    if (h->newest_age >= 0) {
        char eb[32];
        fprintf(out, "  %sspan: %s (newest) .. %s (oldest)%s\n", C_DIM,
                fmt_age(h->newest_age, nb),
                fmt_age(h->oldest_age, eb), C_RESET);
    }
}

static void print_top(FILE *out, const char *title, top_heap *h)
{
    if (h->cap == 0) return;
    fprintf(out, "\n%s%s%s\n", C_BOLD, title, C_RESET);
    if (h->n == 0) {
        fprintf(out, "  (no data)\n");
        return;
    }
    top_item *sorted = heap_sorted(h);
    for (int i = 0; i < h->n; i++) {
        struct timespec ts = { .tv_sec = sorted[i].ts, .tv_nsec = sorted[i].ns };
        char tsbuf[64], szbuf[32];
        p3m_fmt_ts(&ts, tsbuf, false);
        fprintf(out, "  %s  %10s  %s\n",
                tsbuf, p3m_fmt_size(sorted[i].size, szbuf), sorted[i].path);
    }
    free(sorted);
}

static void dump_csv_summary(const char *path, const age_hist *ah,
                              const age_hist *mh)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "p3m-stats: %s: %s\n", path, strerror(errno));
        return;
    }
    fputs("kind,bucket,files,bytes\n", f);
    for (int i = 0; i < N_BUCKETS; i++)
        fprintf(f, "atime,\"%s\",%" PRIu64 ",%" PRIu64 "\n",
                bucket_label[i], ah->count[i], ah->bytes[i]);
    fprintf(f, "atime,future,%" PRIu64 ",%" PRIu64 "\n", ah->future, ah->future_bytes);
    for (int i = 0; i < N_BUCKETS; i++)
        fprintf(f, "mtime,\"%s\",%" PRIu64 ",%" PRIu64 "\n",
                bucket_label[i], mh->count[i], mh->bytes[i]);
    fprintf(f, "mtime,future,%" PRIu64 ",%" PRIu64 "\n", mh->future, mh->future_bytes);
    fclose(f);
}

int main(int argc, char **argv)
{
    p3m_color = isatty(STDOUT_FILENO);

    int c;
    while ((c = getopt_long(argc, argv, "n:o:qhV", lopts, NULL)) != -1) {
        switch (c) {
        case 'n': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 100000) {
                fprintf(stderr, "p3m-stats: --top: invalid value '%s'\n", optarg);
                return 2;
            }
            g.topn = (int)v;
            break;
        }
        case 'o': g.outpath = optarg; break;
        case OPT_CSV_SUMMARY: g.csv_out = optarg; break;
        case OPT_NOW: {
            time_t sec; long ns;
            if (parse_iso8601(optarg, &sec, &ns) != 0) {
                fprintf(stderr, "p3m-stats: --now: invalid timestamp '%s'\n", optarg);
                return 2;
            }
            g.now = sec;
            g.now_set = true;
            break;
        }
        case 'q': g.quiet = true; break;
        case 'h': usage(stdout); return 0;
        case 'V': printf("p3m-stats %s\n", P3M_STATS_VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }

    if (optind < argc) {
        g.inpath = argv[optind++];
        if (optind < argc) {
            fprintf(stderr, "p3m-stats: too many arguments\n");
            usage(stderr);
            return 2;
        }
    }

    if (!g.now_set) g.now = time(NULL);

    FILE *in = stdin;
    if (g.inpath) {
        in = fopen(g.inpath, "r");
        if (!in) {
            fprintf(stderr, "p3m-stats: %s: %s\n", g.inpath, strerror(errno));
            return 2;
        }
    }

    FILE *out = stdout;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-stats: %s: %s\n", g.outpath, strerror(errno));
            return 2;
        }
    }

    /* ---- header: locate columns by name ---- */
    enum { MAXCOLS = 32 };
    strbuf fields[MAXCOLS];
    for (int i = 0; i < MAXCOLS; i++) sb_init(&fields[i], 256);

    int nf = csv_read_record(in, fields, MAXCOLS);
    if (nf <= 0) {
        fprintf(stderr, "p3m-stats: empty or unreadable input\n");
        return 2;
    }

    int col_path = -1, col_size = -1, col_atime = -1, col_mtime = -1, col_type = -1;
    for (int i = 0; i < nf; i++) {
        const char *name = fields[i].buf;
        if      (!strcmp(name, "path"))  col_path  = i;
        else if (!strcmp(name, "size"))  col_size  = i;
        else if (!strcmp(name, "atime")) col_atime = i;
        else if (!strcmp(name, "mtime")) col_mtime = i;
        else if (!strcmp(name, "type"))  col_type  = i;
    }
    if (col_path < 0) {
        fprintf(stderr, "p3m-stats: no 'path' column in input header\n");
        return 2;
    }
    if (col_atime < 0 && col_mtime < 0) {
        fprintf(stderr,
            "p3m-stats: input has neither an 'atime' nor 'mtime' column — "
            "run p3m-ls with -m standard (mtime only) or -m full (both)\n");
        return 2;
    }
    int ncols = nf;

    /* ---- streaming aggregation ---- */
    age_hist ahist = {0}, mhist = {0};
    ahist.oldest_age = ahist.newest_age = -1;
    mhist.oldest_age = mhist.newest_age = -1;

    top_heap atop, mtop;
    heap_init(&atop, col_atime >= 0 ? g.topn : 0);
    heap_init(&mtop, col_mtime >= 0 ? g.topn : 0);

    uint64_t total_files = 0, total_bytes = 0;
    uint64_t n_malformed = 0;
    uint64_t line = 1;

    while ((nf = csv_read_record(in, fields, MAXCOLS)) > 0) {
        line++;
        if (nf < ncols) { n_malformed++; continue; }

        uint64_t size = 0;
        if (col_size >= 0 && fields[col_size].len)
            size = strtoull(fields[col_size].buf, NULL, 10);

        if (col_type >= 0 && fields[col_type].buf[0] == 'd') {
            /* directories carry no meaningful atime/mtime "coldness"
             * signal for this report — skip them, count files only */
            continue;
        }

        total_files++;
        total_bytes += size;

        if (col_atime >= 0 && fields[col_atime].len) {
            time_t sec; long ns;
            if (parse_iso8601(fields[col_atime].buf, &sec, &ns) == 0) {
                int64_t age = (int64_t)g.now - (int64_t)sec;
                hist_add(&ahist, age, size);
                heap_offer(&atop, fields[col_path].buf, sec, ns, size);
            } else {
                n_malformed++;
            }
        }
        if (col_mtime >= 0 && fields[col_mtime].len) {
            time_t sec; long ns;
            if (parse_iso8601(fields[col_mtime].buf, &sec, &ns) == 0) {
                int64_t age = (int64_t)g.now - (int64_t)sec;
                hist_add(&mhist, age, size);
                heap_offer(&mtop, fields[col_path].buf, sec, ns, size);
            } else {
                n_malformed++;
            }
        }
    }

    if (in != stdin) fclose(in);

    /* ---- report ---- */
    if (!g.quiet) {
        char nowbuf[64];
        struct timespec nowts = { .tv_sec = g.now, .tv_nsec = 0 };
        p3m_fmt_ts(&nowts, nowbuf, false);

        fprintf(out, "%sp3m-stats%s — age report", C_BOLD, C_RESET);
        if (g.inpath) fprintf(out, "  (%s)", g.inpath);
        fprintf(out, "\n  reference \"now\": %s\n", nowbuf);

        char nb[32], sb2[32];
        fprintf(out, "  files: %s   total size: %s",
                p3m_fmt_u64(total_files, nb), p3m_fmt_size(total_bytes, sb2));
        if (n_malformed)
            fprintf(out, "   %s%s rows skipped (malformed/short)%s",
                    C_DIM, p3m_fmt_u64(n_malformed, nb), C_RESET);
        fputc('\n', out);

        if (g.topn > 0) {
            if (col_atime >= 0)
                print_top(out, "Most recently accessed (atime)", &atop);
            if (col_mtime >= 0)
                print_top(out, "Most recently modified (mtime)", &mtop);
        }
        if (col_atime >= 0)
            print_hist(out, "Access age (atime) — data not read in over X",
                       &ahist, total_files, total_bytes);
        if (col_mtime >= 0)
            print_hist(out, "Modification age (mtime) — data not written in over X",
                       &mhist, total_files, total_bytes);
        fputc('\n', out);
    }

    if (g.csv_out)
        dump_csv_summary(g.csv_out, &ahist, &mhist);

    if (out != stdout) fclose(out);

    return n_malformed ? 1 : 0;
}
