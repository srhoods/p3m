/*
 * p3m-ls — parallel recursive directory lister
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Walks one or more directory trees with a pool of worker threads and
 * emits the entries found as CSV, at three levels of detail. Designed to
 * outperform single-threaded tools (ls -R, find) on large trees and on
 * high-latency storage by keeping many metadata operations in flight.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P3M_LS_VERSION "1.1.0"

enum detail_mode { MODE_BASIC, MODE_STANDARD, MODE_FULL };

static const char *mode_names[] = { "basic", "standard", "full" };

static struct {
    enum detail_mode mode;
    int              nthreads;
    bool             no_dirs;
    unsigned         typemask;   /* output filter, one bit per DT_* value */
    const char      *outpath;    /* NULL => stdout */
    bool             progress;
} g = { .mode = MODE_BASIC, .typemask = ~0u };

static _Atomic uint64_t n_files;   /* non-directory entries scanned      */
static _Atomic uint64_t n_dirs;    /* directories scanned                */
static _Atomic uint64_t n_bytes;   /* aggregate size of regular files    */

static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* CSV row emission                                                     */
/* ------------------------------------------------------------------ */

static const char *csv_header(void)
{
    switch (g.mode) {
    case MODE_STANDARD:
        return "path,type,size,mode,owner,group,mtime\n";
    case MODE_FULL:
        return "path,type,mode,perms,nlink,owner,uid,group,gid,"
               "size,blksize,blocks,dev,ino,rdev,atime,mtime,ctime\n";
    default:
        return "path\n";
    }
}

static void emit_entry(p3m_outbuf *ob, const char *path, unsigned char dt,
                       const struct stat *st)
{
    if (!p3m_ob_room(ob, 2 * strlen(path) + 1024)) {
        p3m_note_error(path, "emit", ENAMETOOLONG);
        return;
    }

    p3m_ob_csv(ob, path);
    if (g.mode == MODE_BASIC) {
        p3m_ob_putc(ob, '\n');
        return;
    }

    char ufb[32], gfb[32];
    const char *owner = p3m_uid_name(st->st_uid, ufb);
    const char *group = p3m_gid_name(st->st_gid, gfb);

    if (g.mode == MODE_STANDARD) {
        char mt[64];
        p3m_fmt_ts(&st->st_mtim, mt, false);
        p3m_ob_fmt(ob, ",%c,%jd,%04o,", p3m_dt_char(dt),
                   (intmax_t)st->st_size, (unsigned)(st->st_mode & 07777));
        p3m_ob_csv(ob, owner);
        p3m_ob_putc(ob, ',');
        p3m_ob_csv(ob, group);
        p3m_ob_fmt(ob, ",%s\n", mt);
        return;
    }

    char perms[11], at[64], mt[64], ct[64];
    p3m_fmt_perms(st->st_mode, perms);
    p3m_fmt_ts(&st->st_atim, at, true);
    p3m_fmt_ts(&st->st_mtim, mt, true);
    p3m_fmt_ts(&st->st_ctim, ct, true);
    p3m_ob_fmt(ob, ",%c,%04o,%s,%ju,", p3m_dt_char(dt),
               (unsigned)(st->st_mode & 07777), perms,
               (uintmax_t)st->st_nlink);
    p3m_ob_csv(ob, owner);
    p3m_ob_fmt(ob, ",%ju,", (uintmax_t)st->st_uid);
    p3m_ob_csv(ob, group);
    p3m_ob_fmt(ob, ",%ju,%jd,%jd,%jd,%ju,%ju,%ju,%s,%s,%s\n",
               (uintmax_t)st->st_gid, (intmax_t)st->st_size,
               (intmax_t)st->st_blksize, (intmax_t)st->st_blocks,
               (uintmax_t)st->st_dev, (uintmax_t)st->st_ino,
               (uintmax_t)st->st_rdev, at, mt, ct);
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static void scan_dir(const char *dirpath, p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(dirpath);

    DIR *d = opendir(dirpath);
    if (!d) {
        p3m_note_error(dirpath, "opendir", errno);
        return;
    }
    int dfd = dirfd(d);
    size_t dlen = strlen(dirpath);

    char **subs = NULL;
    size_t nsub = 0, csub = 0;

    struct dirent *de;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' &&
            (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) {
            errno = 0;
            continue;
        }

        unsigned char dt = de->d_type;
        struct stat st;
        bool have_st = false;

        if (g.mode != MODE_BASIC || dt == DT_UNKNOWN) {
            if (fstatat(dfd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                char *fp = p3m_path_join(dirpath, dlen, nm);
                p3m_note_error(fp ? fp : nm, "stat", errno);
                free(fp);
                errno = 0;
                continue;
            }
            have_st = true;
            dt = IFTODT(st.st_mode);
        }

        bool isdir = (dt == DT_DIR);
        if (isdir)
            atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);
        if (have_st && S_ISREG(st.st_mode))
            atomic_fetch_add_explicit(&n_bytes, (uint64_t)st.st_size,
                                      memory_order_relaxed);

        bool emit = !(isdir && g.no_dirs) &&
                    (dt < 32 && (g.typemask & (1u << dt)));

        if (emit || isdir) {
            char *fp = p3m_path_join(dirpath, dlen, nm);
            if (!fp) {
                p3m_note_error(nm, "malloc", ENOMEM);
            } else {
                if (emit)
                    emit_entry(ob, fp, dt, have_st ? &st : NULL);
                if (isdir) {
                    if (nsub == csub) {
                        csub = csub ? csub * 2 : 32;
                        char **ns = realloc(subs, csub * sizeof *ns);
                        if (!ns) {
                            p3m_note_error(fp, "queue", ENOMEM);
                            free(fp);
                            errno = 0;
                            continue;
                        }
                        subs = ns;
                    }
                    subs[nsub++] = fp;
                } else {
                    free(fp);
                }
            }
        }
        errno = 0;
    }
    if (errno)
        p3m_note_error(dirpath, "readdir", errno);
    closedir(d);

    p3m_stack_push_batch(&stk, subs, nsub);
    free(subs);
}

static void *worker(void *arg)
{
    p3m_outbuf ob;
    if (p3m_ob_init(&ob, &sink) != 0) {
        p3m_note_error("worker", "malloc", ENOMEM);
        char *p;
        while ((p = p3m_stack_pop(&stk)) != NULL)
            free(p);
        return arg;
    }
    char *path;
    while ((path = p3m_stack_pop(&stk)) != NULL) {
        scan_dir(path, &ob);
        free(path);
    }
    p3m_ob_flush(&ob);
    p3m_ob_free(&ob);
    return arg;
}

/* ------------------------------------------------------------------ */
/* progress display                                                     */
/* ------------------------------------------------------------------ */

#define PROG_LINES 7

static double t_start;

static uint64_t prog_items(void)
{
    return atomic_load_explicit(&n_files, memory_order_relaxed) +
           atomic_load_explicit(&n_dirs, memory_order_relaxed);
}

static void prog_draw(double rate, int frame)
{
    uint64_t files = atomic_load_explicit(&n_files, memory_order_relaxed);
    uint64_t dirs  = atomic_load_explicit(&n_dirs, memory_order_relaxed);
    uint64_t errs  = atomic_load_explicit(&p3m_nerrors, memory_order_relaxed);
    uint64_t bytes = atomic_load_explicit(&n_bytes, memory_order_relaxed);

    char cur[PATH_MAX];
    p3m_get_current(cur, sizeof cur);
    char ptr[512];
    int pmax = p3m_term_width() - 13;
    if (pmax > 500)
        pmax = 500;
    if (pmax < 20)
        pmax = 20;
    p3m_trunc_left(cur, (size_t)pmax, ptr, sizeof ptr);

    char fv[32], dv[32], ev[32], rv[32], sv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_elapsed(p3m_mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-ls%s %s— parallel scan%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET, g.outpath);
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "mode", C_RESET, mode_names[g.mode]);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s\n",
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    if (g.mode == MODE_BASIC)
        ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "elapsed", C_RESET, el);
    else
        ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
            C_DIM, "size", C_RESET, sv, C_DIM, "elapsed", C_RESET, el);
#undef ADD
    fwrite(buf, 1, off, stderr);
}

/* ------------------------------------------------------------------ */
/* summary                                                              */
/* ------------------------------------------------------------------ */

static void print_summary(double elapsed)
{
    uint64_t files = atomic_load(&n_files);
    uint64_t dirs  = atomic_load(&n_dirs);
    uint64_t errs  = atomic_load(&p3m_nerrors);
    uint64_t bytes = atomic_load(&n_bytes);

    char fv[32], dv[32], ev[32], rv[32], sv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-ls%s complete — %s files · %s dirs · %s%s error%s%s",
            C_GREEN, C_RESET, C_BOLD, C_RESET, fv, dv,
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    if (g.mode != MODE_BASIC)
        fprintf(stderr, " · %s", sv);
    fprintf(stderr, "\n  %s in %s (%s items/s)", mode_names[g.mode], el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static int parse_types(const char *s, unsigned *mask)
{
    unsigned m = 0;
    for (; *s; s++) {
        switch (tolower((unsigned char)*s)) {
        case 'f': m |= 1u << DT_REG;  break;
        case 'd': m |= 1u << DT_DIR;  break;
        case 'l': m |= 1u << DT_LNK;  break;
        case 'b': m |= 1u << DT_BLK;  break;
        case 'c': m |= 1u << DT_CHR;  break;
        case 'p': m |= 1u << DT_FIFO; break;
        case 's': m |= 1u << DT_SOCK; break;
        case ',': break;
        default:  return -1;
        }
    }
    if (!m)
        return -1;
    *mask = m;
    return 0;
}

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-ls [OPTIONS] [PATH...]\n"
"\n"
"Recursively list directory trees in parallel, emitting CSV.\n"
"With no PATH, the current directory is scanned.\n"
"\n"
"Options:\n"
"  -m, --mode LEVEL    detail level: basic (default), standard, full\n"
"                        basic     path only (no stat calls needed)\n"
"                        standard  path, type, size, mode, owner, group, mtime\n"
"                        full      every available stat(2) field\n"
"  -j, --threads N     worker threads (default: number of online CPUs)\n"
"  -t, --type TYPES    only output these entry types; combine freely, e.g. -t fl\n"
"                        f file  d dir  l symlink  b block  c char\n"
"                        p fifo  s socket\n"
"      --no-dirs       omit directories from the output\n"
"  -o, --output FILE   write CSV to FILE; a live progress display is shown\n"
"  -h, --help          show this help and exit\n"
"  -V, --version       show version and exit\n"
"\n"
"Output order is non-deterministic (parallel walk); pipe through sort(1)\n"
"if a stable order is required. Symbolic links are reported, not followed.\n",
    to);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "mode",    required_argument, NULL, 'm' },
        { "threads", required_argument, NULL, 'j' },
        { "type",    required_argument, NULL, 't' },
        { "no-dirs", no_argument,       NULL, 1000 },
        { "output",  required_argument, NULL, 'o' },
        { "help",    no_argument,       NULL, 'h' },
        { "version", no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:j:t:o:hV", lopts, NULL)) != -1) {
        switch (c) {
        case 'm':
            if      (!strcmp(optarg, "basic")    || !strcmp(optarg, "b"))
                g.mode = MODE_BASIC;
            else if (!strcmp(optarg, "standard") || !strcmp(optarg, "s"))
                g.mode = MODE_STANDARD;
            else if (!strcmp(optarg, "full")     || !strcmp(optarg, "f"))
                g.mode = MODE_FULL;
            else {
                fprintf(stderr, "p3m-ls: invalid mode '%s' "
                        "(expected basic, standard or full)\n", optarg);
                return 2;
            }
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-ls: invalid thread count '%s' "
                        "(expected 1-512)\n", optarg);
                return 2;
            }
            g.nthreads = (int)v;
            break;
        }
        case 't':
            if (parse_types(optarg, &g.typemask) != 0) {
                fprintf(stderr, "p3m-ls: invalid type list '%s' "
                        "(valid: f d l b c p s)\n", optarg);
                return 2;
            }
            break;
        case 1000:
            g.no_dirs = true;
            break;
        case 'o':
            g.outpath = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            printf("p3m-ls %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_LS_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);
    g.progress = g.outpath && isatty(STDERR_FILENO);

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-ls: cannot open '%s': %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    } else {
        out = stdout;
    }
    static char outvbuf[1 << 20];
    setvbuf(out, outvbuf, _IOFBF, sizeof outvbuf);
    p3m_sink_init(&sink, out);

    fputs(csv_header(), out);

    /* seed the work queue with the root paths */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-ls: out of memory\n");
        return 2;
    }
    static char *dot = ".";
    char **roots = (optind < argc) ? &argv[optind] : &dot;
    int nroots = (optind < argc) ? argc - optind : 1;
    for (int i = 0; i < nroots; i++) {
        char *root = strdup(roots[i]);
        if (!root)
            continue;
        size_t len = strlen(root);
        while (len > 1 && root[len - 1] == '/')   /* normalise trailing / */
            root[--len] = '\0';

        struct stat st;
        if (lstat(root, &st) != 0) {
            p3m_note_error(root, "stat", errno);
            free(root);
        } else if (S_ISDIR(st.st_mode)) {
            p3m_stack_push_batch(&stk, &root, 1);
        } else {
            /* a non-directory root is simply reported as an entry */
            unsigned char dt = IFTODT(st.st_mode);
            atomic_fetch_add(&n_files, 1);
            if (S_ISREG(st.st_mode))
                atomic_fetch_add(&n_bytes, (uint64_t)st.st_size);
            if (dt < 32 && (g.typemask & (1u << dt)))
                emit_entry(&rootob, root, dt, &st);
            free(root);
        }
    }
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    if (g.progress)
        p3m_set_current("…");

    /* launch */
    t_start = p3m_mono_now();

    if (g.progress) {
        p3m_progress_cfg cfg = {
            .draw = prog_draw, .items = prog_items, .lines = PROG_LINES
        };
        if (p3m_progress_start(&cfg) != 0)
            g.progress = false;
    }

    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-ls: out of memory\n");
        return 2;
    }
    int started = 0;
    for (int i = 0; i < g.nthreads; i++) {
        if (pthread_create(&tids[i], NULL, worker, NULL) != 0)
            break;
        started++;
    }
    if (started < g.nthreads) {
        if (started == 0) {
            fprintf(stderr, "p3m-ls: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    /* finish the output stream */
    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-ls: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    if (g.outpath)
        print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
