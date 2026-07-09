/*
 * p3m-rm — parallel recursive remove
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Removes files and directory trees with a pool of worker threads.
 * Defaults to a dry run that lists everything which would be removed;
 * --apply performs the removal. Arguments may be literal paths or
 * glob masks. Symbolic links are never followed — a link is unlinked,
 * its target untouched — and a hard, non-overridable guard refuses to
 * operate on the filesystem root.
 *
 * Apply runs in two phases: the parallel walk unlinks files as it
 * scans and collects directories; the directories are then removed
 * deepest-first, level-parallel, so a parent is never attempted
 * before its children.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P3M_RM_VERSION "1.0.0"

static struct {
    bool        apply;
    int         nthreads;
    const char *outpath;    /* NULL => stdout */
    bool        progress;
} g;

static _Atomic uint64_t n_files;    /* non-directory entries scanned  */
static _Atomic uint64_t n_dirs;     /* directories scanned            */
static _Atomic uint64_t n_removed;  /* removed (apply) — files + dirs */

static p3m_stack stk;
static p3m_sink  sink;

#define CSV_HEADER "path,type,result\n"

static void emit_row(p3m_outbuf *ob, const char *path, char tc,
                     const char *result)
{
    if (!p3m_ob_room(ob, 2 * strlen(path) + 128)) {
        p3m_note_error(path, "emit", ENAMETOOLONG);
        return;
    }
    p3m_ob_csv(ob, path);
    p3m_ob_fmt(ob, ",%c,%s\n", tc, result);
}

/* ------------------------------------------------------------------ */
/* directory list for the deepest-first rmdir phase (apply mode)        */
/* ------------------------------------------------------------------ */

struct dirrec {
    char *path;
    int   depth;
};

static struct dirrec  *dlist;
static size_t          dlist_n, dlist_cap;
static pthread_mutex_t dlist_mu = PTHREAD_MUTEX_INITIALIZER;

static int slash_depth(const char *p)
{
    int d = 0;
    for (; *p; p++)
        if (*p == '/')
            d++;
    return d;
}

/* takes ownership of `path` */
static void dlist_add(char *path)
{
    pthread_mutex_lock(&dlist_mu);
    if (dlist_n == dlist_cap) {
        size_t nc = dlist_cap ? dlist_cap * 2 : 1024;
        struct dirrec *nd = realloc(dlist, nc * sizeof *nd);
        if (!nd) {
            pthread_mutex_unlock(&dlist_mu);
            p3m_note_error(path, "queue", ENOMEM);
            free(path);
            return;
        }
        dlist = nd;
        dlist_cap = nc;
    }
    dlist[dlist_n].path = path;
    dlist[dlist_n].depth = slash_depth(path);
    dlist_n++;
    pthread_mutex_unlock(&dlist_mu);
}

static int dirrec_cmp_depth_desc(const void *a, const void *b)
{
    return ((const struct dirrec *)b)->depth -
           ((const struct dirrec *)a)->depth;
}

/* ------------------------------------------------------------------ */
/* phase 1: parallel walk — list (dry run) or unlink files (apply)      */
/* ------------------------------------------------------------------ */

static void scan_dir(const char *dirpath, p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(dirpath);

    /* O_NOFOLLOW: refuse to traverse if the path has been swapped for
     * a symlink since it was queued — deletion must never escape */
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        p3m_note_error(dirpath, "opendir", errno);
        return;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        p3m_note_error(dirpath, "opendir", errno);
        close(fd);
        return;
    }
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
        if (dt == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                char *fp = p3m_path_join(dirpath, dlen, nm);
                p3m_note_error(fp ? fp : nm, "stat", errno);
                free(fp);
                errno = 0;
                continue;
            }
            dt = IFTODT(st.st_mode);
        }

        char *fp = p3m_path_join(dirpath, dlen, nm);
        if (!fp) {
            p3m_note_error(nm, "malloc", ENOMEM);
            errno = 0;
            continue;
        }

        if (dt == DT_DIR) {
            atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
            if (g.apply) {
                char *copy = strdup(fp);
                if (copy)
                    dlist_add(copy);
                else
                    p3m_note_error(fp, "malloc", ENOMEM);
            } else {
                emit_row(ob, fp, 'd', "pending");
            }
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
            atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);
            if (!g.apply) {
                emit_row(ob, fp, p3m_dt_char(dt), "pending");
            } else if (unlinkat(fd, nm, 0) == 0 || errno == ENOENT) {
                atomic_fetch_add_explicit(&n_removed, 1,
                                          memory_order_relaxed);
                emit_row(ob, fp, p3m_dt_char(dt), "removed");
            } else {
                int e = errno;
                p3m_note_error(fp, "unlink", e);
                char res[160];
                snprintf(res, sizeof res, "failed: unlink: %s",
                         strerror(e));
                emit_row(ob, fp, p3m_dt_char(dt), res);
            }
            free(fp);
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
/* phase 2: deepest-first, level-parallel rmdir (apply mode)            */
/* ------------------------------------------------------------------ */

static _Atomic size_t    rm_cursor;
static size_t            rm_level_end;
static atomic_bool       rm_done;
static pthread_barrier_t rm_bar;

static void rm_level_chunk(p3m_outbuf *ob)
{
    size_t i;
    while ((i = atomic_fetch_add(&rm_cursor, 1)) < rm_level_end) {
        const char *path = dlist[i].path;
        p3m_set_current(path);
        if (rmdir(path) == 0 || errno == ENOENT) {
            atomic_fetch_add_explicit(&n_removed, 1, memory_order_relaxed);
            emit_row(ob, path, 'd', "removed");
        } else {
            int e = errno;
            p3m_note_error(path, "rmdir", e);
            char res[160];
            snprintf(res, sizeof res, "failed: rmdir: %s", strerror(e));
            emit_row(ob, path, 'd', res);
        }
    }
}

static void *rm_worker(void *arg)
{
    p3m_outbuf ob;
    bool have_ob = p3m_ob_init(&ob, &sink) == 0;
    if (!have_ob)
        p3m_note_error("rm_worker", "malloc", ENOMEM);
    for (;;) {
        pthread_barrier_wait(&rm_bar);       /* level start */
        if (atomic_load(&rm_done))
            break;
        if (have_ob)
            rm_level_chunk(&ob);
        pthread_barrier_wait(&rm_bar);       /* level end */
    }
    if (have_ob) {
        p3m_ob_flush(&ob);
        p3m_ob_free(&ob);
    }
    return arg;
}

/* Remove all collected directories, deepest level first. */
static void remove_dirs(int nthreads, p3m_outbuf *mainob)
{
    if (!dlist_n)
        return;
    qsort(dlist, dlist_n, sizeof *dlist, dirrec_cmp_depth_desc);

    int nworkers = nthreads - 1;             /* main thread joins in */
    if (nworkers < 0)
        nworkers = 0;
    if ((size_t)nworkers > dlist_n / 64)     /* small trees: fewer threads */
        nworkers = (int)(dlist_n / 64);

    pthread_t *tids = NULL;
    int started = 0;
    if (nworkers > 0) {
        pthread_barrier_init(&rm_bar, NULL, (unsigned)nworkers + 1);
        tids = calloc((size_t)nworkers, sizeof *tids);
        if (tids) {
            for (int i = 0; i < nworkers; i++) {
                if (pthread_create(&tids[i], NULL, rm_worker, NULL) != 0)
                    break;
                started++;
            }
        }
        if (started < nworkers) {
            /* re-init the barrier for the threads that did start */
            if (started == 0) {
                pthread_barrier_destroy(&rm_bar);
            } else {
                /* cannot resize a barrier: fall back to single-threaded
                 * by releasing the started workers immediately */
                atomic_store(&rm_done, true);
                pthread_barrier_wait(&rm_bar);
                for (int i = 0; i < started; i++)
                    pthread_join(tids[i], NULL);
                pthread_barrier_destroy(&rm_bar);
                started = 0;
            }
        }
    }

    size_t pos = 0;
    while (pos < dlist_n) {
        size_t end = pos;
        int depth = dlist[pos].depth;
        while (end < dlist_n && dlist[end].depth == depth)
            end++;
        atomic_store(&rm_cursor, pos);
        rm_level_end = end;
        if (started > 0) {
            pthread_barrier_wait(&rm_bar);   /* release level */
            rm_level_chunk(mainob);
            pthread_barrier_wait(&rm_bar);   /* level complete */
        } else {
            rm_level_chunk(mainob);
        }
        pos = end;
    }

    if (started > 0) {
        atomic_store(&rm_done, true);
        pthread_barrier_wait(&rm_bar);
        for (int i = 0; i < started; i++)
            pthread_join(tids[i], NULL);
        pthread_barrier_destroy(&rm_bar);
    }
    free(tids);
}

/* ------------------------------------------------------------------ */
/* progress display                                                     */
/* ------------------------------------------------------------------ */

#define PROG_LINES 7

static double t_start;

static uint64_t prog_items(void)
{
    if (g.apply)
        return atomic_load_explicit(&n_removed, memory_order_relaxed);
    return atomic_load_explicit(&n_files, memory_order_relaxed) +
           atomic_load_explicit(&n_dirs, memory_order_relaxed);
}

static void prog_draw(double rate, int frame)
{
    uint64_t files = atomic_load_explicit(&n_files, memory_order_relaxed);
    uint64_t dirs  = atomic_load_explicit(&n_dirs, memory_order_relaxed);
    uint64_t rem   = atomic_load_explicit(&n_removed, memory_order_relaxed);
    uint64_t errs  = atomic_load_explicit(&p3m_nerrors, memory_order_relaxed);

    char cur[PATH_MAX];
    p3m_get_current(cur, sizeof cur);
    char ptr[512];
    int pmax = p3m_term_width() - 13;
    if (pmax > 500)
        pmax = 500;
    if (pmax < 20)
        pmax = 20;
    p3m_trunc_left(cur, (size_t)pmax, ptr, sizeof ptr);

    char fv[32], dv[32], mv[32], ev[32], rv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(g.apply ? rem : files + dirs, mv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_elapsed(p3m_mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-rm%s %s— parallel remove (%s)%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, g.apply ? "apply" : "dry-run", C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "stdout");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "action", C_RESET, g.apply ? "apply" : "dry-run");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s\n",
        C_DIM, g.apply ? "removed" : "pending", C_RESET, mv,
        C_DIM, "errors", C_RESET, errs ? C_RED : "", ev,
        errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "elapsed", C_RESET, el);
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
    uint64_t rem   = atomic_load(&n_removed);
    uint64_t errs  = atomic_load(&p3m_nerrors);

    char sv[32], mv[32], ev[32], rv[32], el[32];
    p3m_fmt_u64(files + dirs, sv);
    p3m_fmt_u64(g.apply ? rem : files + dirs, mv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-rm%s complete — %s%s%s · %s scanned · %s %s · "
            "%s%s error%s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET,
            C_CYAN, g.apply ? "apply" : "dry-run", C_RESET, sv, mv,
            g.apply ? "removed" : "pending removal",
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s)", el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
    if (!g.apply && files + dirs)
        fprintf(stderr, "  %sre-run with --apply to remove these items%s\n",
                C_DIM, C_RESET);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-rm [OPTIONS] PATH|MASK...\n"
"\n"
"Remove files and directory trees in parallel. Arguments are literal\n"
"paths or glob masks (quote masks to stop the shell expanding them),\n"
"e.g. p3m-rm '/data/tmp-*'.\n"
"\n"
"Without --apply this is a DRY RUN that only lists, as CSV, everything\n"
"that would be removed.\n"
"\n"
"Options:\n"
"      --apply           actually remove (default is a dry run)\n"
"  -j, --threads N       worker threads (default: number of online CPUs)\n"
"  -o, --output FILE     write CSV to FILE; a live progress display is shown\n"
"  -h, --help            show this help and exit\n"
"  -V, --version         show version and exit\n"
"\n"
"Safety:\n"
"  * Any path resolving to '/' is refused outright. This guard cannot\n"
"    be overridden.\n"
"  * Symbolic links are NEVER followed: the link itself is removed,\n"
"    its target is untouched, and traversal re-checks with O_NOFOLLOW.\n"
"  * '.' and '..' are refused.\n"
"\n"
"Exit status: 0 success, 1 errors were encountered, 2 usage error or\n"
"root-guard refusal.\n",
    to);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "apply",   no_argument,       NULL, 1000 },
        { "threads", required_argument, NULL, 'j' },
        { "output",  required_argument, NULL, 'o' },
        { "help",    no_argument,       NULL, 'h' },
        { "version", no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "j:o:hV", lopts, NULL)) != -1) {
        switch (c) {
        case 1000:
            g.apply = true;
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-rm: invalid thread count '%s' "
                        "(expected 1-512)\n", optarg);
                return 2;
            }
            g.nthreads = (int)v;
            break;
        }
        case 'o':
            g.outpath = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            printf("p3m-rm %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_RM_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "p3m-rm: no paths given\n\n");
        usage(stderr);
        return 2;
    }

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);
    g.progress = g.outpath && isatty(STDERR_FILENO);

    /* expand masks into the root list */
    char **roots = NULL;
    size_t nroots = 0, croots = 0;
    for (int i = optind; i < argc; i++) {
        glob_t gl;
        int r = glob(argv[i], GLOB_NOSORT, NULL, &gl);
        if (r == GLOB_NOMATCH) {
            p3m_note_error(argv[i], "no match", ENOENT);
            continue;
        }
        if (r != 0) {
            p3m_note_error(argv[i], "glob", errno ? errno : EINVAL);
            continue;
        }
        for (size_t k = 0; k < gl.gl_pathc; k++) {
            if (nroots == croots) {
                croots = croots ? croots * 2 : 16;
                char **nr = realloc(roots, croots * sizeof *nr);
                if (!nr) {
                    fprintf(stderr, "p3m-rm: out of memory\n");
                    return 2;
                }
                roots = nr;
            }
            char *root = strdup(gl.gl_pathv[k]);
            if (!root)
                continue;
            size_t len = strlen(root);
            while (len > 1 && root[len - 1] == '/')
                root[--len] = '\0';
            roots[nroots++] = root;
        }
        globfree(&gl);
    }

    /*
     * HARD GUARD — runs before anything is touched, cannot be
     * overridden: refuse any path that resolves to the filesystem
     * root, and refuse '.' and '..'. A symlink root is exempt from
     * resolution because only the link itself would be unlinked.
     */
    for (size_t i = 0; i < nroots; i++) {
        const char *root = roots[i];
        struct stat st;
        bool is_link = lstat(root, &st) == 0 && S_ISLNK(st.st_mode);
        if (!is_link) {             /* a link root only removes the link */
            char resolved[PATH_MAX];
            if (realpath(root, resolved) && !strcmp(resolved, "/")) {
                fprintf(stderr,
                        "p3m-rm: %sFATAL%s: '%s' resolves to '/' — refusing "
                        "to remove the filesystem root.\n"
                        "        This guard cannot be overridden.\n",
                        C_RED, C_RESET, root);
                return 2;
            }
        }
        const char *base = strrchr(root, '/');
        base = base ? base + 1 : root;
        if (!strcmp(base, ".") || !strcmp(base, "..") || !*base) {
            fprintf(stderr, "p3m-rm: refusing to remove '%s' "
                    "('.' and '..' are not valid targets)\n", root);
            return 2;
        }
    }

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-rm: cannot open '%s': %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    } else {
        out = stdout;
    }
    static char outvbuf[1 << 20];
    setvbuf(out, outvbuf, _IOFBF, sizeof outvbuf);
    p3m_sink_init(&sink, out);

    if (!g.apply)
        fprintf(stderr, "%sp3m-rm: dry run — nothing will be removed "
                "(use --apply)%s\n", C_DIM, C_RESET);

    fputs(CSV_HEADER, out);

    /* seed the queue; the named paths themselves are removed too */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-rm: out of memory\n");
        return 2;
    }
    for (size_t i = 0; i < nroots; i++) {
        char *root = roots[i];
        struct stat st;
        if (lstat(root, &st) != 0) {
            p3m_note_error(root, "stat", errno);
            free(root);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            atomic_fetch_add(&n_dirs, 1);
            if (g.apply) {
                char *copy = strdup(root);
                if (copy)
                    dlist_add(copy);
                else
                    p3m_note_error(root, "malloc", ENOMEM);
            } else {
                emit_row(&rootob, root, 'd', "pending");
            }
            p3m_stack_push_batch(&stk, &root, 1);
        } else {
            char tc = p3m_dt_char(IFTODT(st.st_mode));
            atomic_fetch_add(&n_files, 1);
            if (!g.apply) {
                emit_row(&rootob, root, tc, "pending");
            } else if (unlink(root) == 0 || errno == ENOENT) {
                atomic_fetch_add(&n_removed, 1);
                emit_row(&rootob, root, tc, "removed");
            } else {
                int e = errno;
                p3m_note_error(root, "unlink", e);
                char res[160];
                snprintf(res, sizeof res, "failed: unlink: %s",
                         strerror(e));
                emit_row(&rootob, root, tc, res);
            }
            free(root);
        }
    }
    free(roots);

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
        fprintf(stderr, "p3m-rm: out of memory\n");
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
            fprintf(stderr, "p3m-rm: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    /* phase 2: remove the (now empty) directories, deepest first */
    if (g.apply && !atomic_load(&sink.failed))
        remove_dirs(started, &rootob);
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    /* finish the output stream */
    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-rm: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);
    for (size_t i = 0; i < dlist_n; i++)
        free(dlist[i].path);
    free(dlist);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
