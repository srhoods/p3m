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
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define P3M_LS_VERSION "1.0.0"

enum detail_mode { MODE_BASIC, MODE_STANDARD, MODE_FULL };

static const char *mode_names[] = { "basic", "standard", "full" };

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static struct {
    enum detail_mode mode;
    int              nthreads;
    bool             no_dirs;
    unsigned         typemask;   /* output filter, one bit per DT_* value */
    const char      *outpath;    /* NULL => stdout */
    FILE            *out;
    bool             progress;   /* live progress display active */
    bool             color;      /* ANSI colour on stderr */
} g = { .mode = MODE_BASIC, .typemask = ~0u };

/* ------------------------------------------------------------------ */
/* shared counters                                                     */
/* ------------------------------------------------------------------ */

static _Atomic uint64_t n_files;   /* non-directory entries scanned      */
static _Atomic uint64_t n_dirs;    /* directories scanned                */
static _Atomic uint64_t n_errors;
static _Atomic uint64_t n_bytes;   /* aggregate size of regular files    */
static atomic_bool      write_failed;
static atomic_bool      scanning;

/* ------------------------------------------------------------------ */
/* error log: keep the first few messages for the end-of-run report    */
/* ------------------------------------------------------------------ */

#define ERRLOG_MAX 24
static char           *errlog[ERRLOG_MAX];
static int             errlog_n;
static pthread_mutex_t errlog_mu = PTHREAD_MUTEX_INITIALIZER;

static void note_error(const char *path, const char *what, int err)
{
    atomic_fetch_add_explicit(&n_errors, 1, memory_order_relaxed);
    pthread_mutex_lock(&errlog_mu);
    if (errlog_n < ERRLOG_MAX) {
        char buf[PATH_MAX + 128];
        snprintf(buf, sizeof buf, "%s: %s: %s", what, path, strerror(err));
        errlog[errlog_n] = strdup(buf);
        if (errlog[errlog_n])
            errlog_n++;
    }
    pthread_mutex_unlock(&errlog_mu);
}

/* ------------------------------------------------------------------ */
/* "current path" shown by the progress display                        */
/* ------------------------------------------------------------------ */

static char            cur_path[PATH_MAX];
static pthread_mutex_t cur_mu = PTHREAD_MUTEX_INITIALIZER;

static void set_current(const char *p)
{
    pthread_mutex_lock(&cur_mu);
    snprintf(cur_path, sizeof cur_path, "%s", p);
    pthread_mutex_unlock(&cur_mu);
}

static void get_current(char *dst, size_t n)
{
    pthread_mutex_lock(&cur_mu);
    snprintf(dst, n, "%s", cur_path);
    pthread_mutex_unlock(&cur_mu);
}

/* ------------------------------------------------------------------ */
/* work queue: LIFO stack of directory paths (LIFO keeps memory low)   */
/* ------------------------------------------------------------------ */

static struct {
    char           **items;
    size_t           len, cap;
    int              idle;
    int              nthreads;
    bool             done;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
} stk = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static void stack_push_batch(char **paths, size_t n)
{
    if (!n)
        return;
    pthread_mutex_lock(&stk.mu);
    if (stk.len + n > stk.cap) {
        size_t nc = stk.cap ? stk.cap : 256;
        while (nc < stk.len + n)
            nc *= 2;
        char **ni = realloc(stk.items, nc * sizeof *ni);
        if (!ni) {
            pthread_mutex_unlock(&stk.mu);
            for (size_t i = 0; i < n; i++) {
                note_error(paths[i], "queue", ENOMEM);
                free(paths[i]);
            }
            return;
        }
        stk.items = ni;
        stk.cap = nc;
    }
    memcpy(stk.items + stk.len, paths, n * sizeof *paths);
    stk.len += n;
    if (n == 1)
        pthread_cond_signal(&stk.cv);
    else
        pthread_cond_broadcast(&stk.cv);
    pthread_mutex_unlock(&stk.mu);
}

/* Pop a directory to scan; returns NULL when the whole walk is finished. */
static char *stack_pop(void)
{
    pthread_mutex_lock(&stk.mu);
    while (stk.len == 0 && !stk.done) {
        stk.idle++;
        if (stk.idle == stk.nthreads) {
            stk.done = true;
            pthread_cond_broadcast(&stk.cv);
        } else {
            pthread_cond_wait(&stk.cv, &stk.mu);
        }
        stk.idle--;
    }
    char *p = NULL;
    if (stk.len > 0)
        p = stk.items[--stk.len];
    pthread_mutex_unlock(&stk.mu);
    return p;
}

/* ------------------------------------------------------------------ */
/* buffered CSV output: per-thread buffers, single mutex on flush      */
/* ------------------------------------------------------------------ */

#define OB_CAP ((size_t)1 << 20)

typedef struct {
    char  *buf;
    size_t len;
} outbuf_t;

static pthread_mutex_t out_mu = PTHREAD_MUTEX_INITIALIZER;

static void ob_flush(outbuf_t *ob)
{
    if (!ob->len)
        return;
    pthread_mutex_lock(&out_mu);
    size_t w = fwrite(ob->buf, 1, ob->len, g.out);
    pthread_mutex_unlock(&out_mu);
    if (w != ob->len)
        atomic_store(&write_failed, true);
    ob->len = 0;
}

static inline void ob_puts(outbuf_t *ob, const char *s)
{
    size_t n = strlen(s);
    memcpy(ob->buf + ob->len, s, n);
    ob->len += n;
}

static inline void ob_putc(outbuf_t *ob, char c)
{
    ob->buf[ob->len++] = c;
}

static void ob_fmt(outbuf_t *ob, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ob->buf + ob->len, OB_CAP - ob->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        ob->len += (size_t)n;
}

/* RFC 4180 quoting: quote fields containing comma, quote, CR or LF. */
static void ob_csv(outbuf_t *ob, const char *s)
{
    if (!s[strcspn(s, ",\"\n\r")]) {
        ob_puts(ob, s);
        return;
    }
    ob_putc(ob, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"')
            ob_putc(ob, '"');
        ob_putc(ob, *p);
    }
    ob_putc(ob, '"');
}

/* ------------------------------------------------------------------ */
/* uid/gid -> name cache (immutable entries, tiny linear scan)         */
/* ------------------------------------------------------------------ */

#define IDCACHE_MAX 256
struct ident { uint32_t id; char name[64]; };
struct idcache {
    pthread_mutex_t mu;
    int             n;
    struct ident    e[IDCACHE_MAX];
};
static struct idcache users  = { .mu = PTHREAD_MUTEX_INITIALIZER };
static struct idcache groups = { .mu = PTHREAD_MUTEX_INITIALIZER };

static const char *id_name(struct idcache *c, uint32_t id, bool is_user,
                           char fallback[32])
{
    pthread_mutex_lock(&c->mu);
    for (int i = 0; i < c->n; i++) {
        if (c->e[i].id == id) {
            const char *nm = c->e[i].name;   /* entries never change */
            pthread_mutex_unlock(&c->mu);
            return nm;
        }
    }
    pthread_mutex_unlock(&c->mu);

    char buf[4096];
    const char *nm = NULL;
    if (is_user) {
        struct passwd pw, *res = NULL;
        if (!getpwuid_r(id, &pw, buf, sizeof buf, &res) && res)
            nm = pw.pw_name;
    } else {
        struct group gr, *res = NULL;
        if (!getgrgid_r(id, &gr, buf, sizeof buf, &res) && res)
            nm = gr.gr_name;
    }

    pthread_mutex_lock(&c->mu);
    for (int i = 0; i < c->n; i++) {           /* lost a race? reuse it */
        if (c->e[i].id == id) {
            const char *s = c->e[i].name;
            pthread_mutex_unlock(&c->mu);
            return s;
        }
    }
    if (c->n < IDCACHE_MAX) {
        struct ident *e = &c->e[c->n];
        e->id = id;
        if (nm)
            snprintf(e->name, sizeof e->name, "%s", nm);
        else
            snprintf(e->name, sizeof e->name, "%u", id);
        c->n++;
        pthread_mutex_unlock(&c->mu);
        return e->name;
    }
    pthread_mutex_unlock(&c->mu);
    snprintf(fallback, 32, "%u", id);
    return fallback;
}

/* ------------------------------------------------------------------ */
/* small formatting helpers                                            */
/* ------------------------------------------------------------------ */

static char dt_char(unsigned char dt)
{
    switch (dt) {
    case DT_REG:  return 'f';
    case DT_DIR:  return 'd';
    case DT_LNK:  return 'l';
    case DT_BLK:  return 'b';
    case DT_CHR:  return 'c';
    case DT_FIFO: return 'p';
    case DT_SOCK: return 's';
    default:      return '?';
    }
}

static void fmt_perms(mode_t m, char out[11])
{
    char t = '?';
    if      (S_ISREG(m))  t = '-';
    else if (S_ISDIR(m))  t = 'd';
    else if (S_ISLNK(m))  t = 'l';
    else if (S_ISBLK(m))  t = 'b';
    else if (S_ISCHR(m))  t = 'c';
    else if (S_ISFIFO(m)) t = 'p';
    else if (S_ISSOCK(m)) t = 's';
    out[0] = t;
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_ISUID) ? ((m & S_IXUSR) ? 's' : 'S')
                           : ((m & S_IXUSR) ? 'x' : '-');
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_ISGID) ? ((m & S_IXGRP) ? 's' : 'S')
                           : ((m & S_IXGRP) ? 'x' : '-');
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_ISVTX) ? ((m & S_IXOTH) ? 't' : 'T')
                           : ((m & S_IXOTH) ? 'x' : '-');
    out[10] = '\0';
}

static void fmt_ts(const struct timespec *ts, char out[64], bool with_ns)
{
    struct tm tm;
    time_t s = ts->tv_sec;
    gmtime_r(&s, &tm);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    if (with_ns)
        snprintf(out, 64, "%s.%09ldZ", base, (long)ts->tv_nsec);
    else
        snprintf(out, 64, "%sZ", base);
}

/* 1234567 -> "1,234,567" */
static char *fmt_u64(uint64_t v, char out[32])
{
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    int m = n + (n - 1) / 3;
    out[m] = '\0';
    for (int ti = n - 1, oi = m - 1, cnt = 0; ti >= 0; ) {
        out[oi--] = tmp[ti--];
        if (++cnt == 3 && ti >= 0) {
            out[oi--] = ',';
            cnt = 0;
        }
    }
    return out;
}

static char *fmt_size(uint64_t b, char out[32])
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, 32, "%llu B", (unsigned long long)b);
    else
        snprintf(out, 32, "%.2f %s", v, unit[i]);
    return out;
}

static char *fmt_elapsed(double s, char out[32])
{
    if (s < 60.0)
        snprintf(out, 32, "%.1fs", s);
    else
        snprintf(out, 32, "%dm %02ds", (int)(s / 60.0), (int)s % 60);
    return out;
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ */
/* CSV row emission                                                    */
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

static void emit_entry(outbuf_t *ob, const char *path, unsigned char dt,
                       const struct stat *st)
{
    size_t need = 2 * strlen(path) + 1024;
    if (need > OB_CAP) {
        note_error(path, "emit", ENAMETOOLONG);
        return;
    }
    if (OB_CAP - ob->len < need)
        ob_flush(ob);

    ob_csv(ob, path);
    if (g.mode == MODE_BASIC) {
        ob_putc(ob, '\n');
        return;
    }

    char ufb[32], gfb[32];
    const char *owner = id_name(&users, st->st_uid, true, ufb);
    const char *group = id_name(&groups, st->st_gid, false, gfb);

    if (g.mode == MODE_STANDARD) {
        char mt[64];
        fmt_ts(&st->st_mtim, mt, false);
        ob_fmt(ob, ",%c,%jd,%04o,", dt_char(dt), (intmax_t)st->st_size,
               (unsigned)(st->st_mode & 07777));
        ob_csv(ob, owner);
        ob_putc(ob, ',');
        ob_csv(ob, group);
        ob_fmt(ob, ",%s\n", mt);
        return;
    }

    char perms[11], at[64], mt[64], ct[64];
    fmt_perms(st->st_mode, perms);
    fmt_ts(&st->st_atim, at, true);
    fmt_ts(&st->st_mtim, mt, true);
    fmt_ts(&st->st_ctim, ct, true);
    ob_fmt(ob, ",%c,%04o,%s,%ju,", dt_char(dt),
           (unsigned)(st->st_mode & 07777), perms, (uintmax_t)st->st_nlink);
    ob_csv(ob, owner);
    ob_fmt(ob, ",%ju,", (uintmax_t)st->st_uid);
    ob_csv(ob, group);
    ob_fmt(ob, ",%ju,%jd,%jd,%jd,%ju,%ju,%ju,%s,%s,%s\n",
           (uintmax_t)st->st_gid, (intmax_t)st->st_size,
           (intmax_t)st->st_blksize, (intmax_t)st->st_blocks,
           (uintmax_t)st->st_dev, (uintmax_t)st->st_ino,
           (uintmax_t)st->st_rdev, at, mt, ct);
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                  */
/* ------------------------------------------------------------------ */

static char *path_join(const char *dir, size_t dlen, const char *name)
{
    size_t nlen = strlen(name);
    bool slash = dlen && dir[dlen - 1] == '/';
    char *p = malloc(dlen + (slash ? 0 : 1) + nlen + 1);
    if (!p)
        return NULL;
    memcpy(p, dir, dlen);
    size_t o = dlen;
    if (!slash)
        p[o++] = '/';
    memcpy(p + o, name, nlen + 1);
    return p;
}

/*
 * Classify, count, filter and emit a single entry; returns true if the
 * caller should descend into it (i.e. it is a directory).
 * `fullpath` is only built when needed.
 */
static void scan_dir(const char *dirpath, outbuf_t *ob)
{
    if (atomic_load_explicit(&write_failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    set_current(dirpath);

    DIR *d = opendir(dirpath);
    if (!d) {
        note_error(dirpath, "opendir", errno);
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
                char *fp = path_join(dirpath, dlen, nm);
                note_error(fp ? fp : nm, "stat", errno);
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
            char *fp = path_join(dirpath, dlen, nm);
            if (!fp) {
                note_error(nm, "malloc", ENOMEM);
            } else {
                if (emit)
                    emit_entry(ob, fp, dt, have_st ? &st : NULL);
                if (isdir) {
                    if (nsub == csub) {
                        csub = csub ? csub * 2 : 32;
                        char **ns = realloc(subs, csub * sizeof *ns);
                        if (!ns) {
                            note_error(fp, "queue", ENOMEM);
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
        note_error(dirpath, "readdir", errno);
    closedir(d);

    stack_push_batch(subs, nsub);
    free(subs);
}

static void *worker(void *arg)
{
    outbuf_t ob = { .buf = malloc(OB_CAP), .len = 0 };
    if (!ob.buf) {
        note_error("worker", "malloc", ENOMEM);
        /* still drain the queue so termination detection works */
        char *p;
        while ((p = stack_pop()) != NULL)
            free(p);
        return arg;
    }
    char *path;
    while ((path = stack_pop()) != NULL) {
        scan_dir(path, &ob);
        free(path);
    }
    ob_flush(&ob);
    free(ob.buf);
    return arg;
}

/* ------------------------------------------------------------------ */
/* progress display                                                    */
/* ------------------------------------------------------------------ */

#define C_RESET (g.color ? "\x1b[0m"  : "")
#define C_BOLD  (g.color ? "\x1b[1m"  : "")
#define C_DIM   (g.color ? "\x1b[2m"  : "")
#define C_RED   (g.color ? "\x1b[31m" : "")
#define C_GREEN (g.color ? "\x1b[32m" : "")
#define C_CYAN  (g.color ? "\x1b[36m" : "")

#define PROG_LINES 7

static double t_start;

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* Keep the tail of the path, prefixing an ellipsis, to fit `max` columns. */
static void trunc_left(const char *s, size_t max, char *out, size_t outsz)
{
    size_t len = strlen(s);
    if (max >= outsz)
        max = outsz - 1;
    if (len <= max) {
        memcpy(out, s, len + 1);
        return;
    }
    /* keep the tail, prefix a single-column UTF-8 ellipsis (3 bytes) */
    size_t keep = (max > 4) ? max - 1 : 3;
    if (keep + 4 > outsz)
        keep = outsz - 4;
    memcpy(out, "…", 3);
    memcpy(out + 3, s + (len - keep), keep + 1);
}

static void progress_draw(bool first, double rate, int frame)
{
    static const char *spin[] = { "⠋", "⠙", "⠹", "⠸", "⠼",
                                  "⠴", "⠦", "⠧", "⠇", "⠏" };
    uint64_t files = atomic_load_explicit(&n_files, memory_order_relaxed);
    uint64_t dirs  = atomic_load_explicit(&n_dirs, memory_order_relaxed);
    uint64_t errs  = atomic_load_explicit(&n_errors, memory_order_relaxed);
    uint64_t bytes = atomic_load_explicit(&n_bytes, memory_order_relaxed);

    char cur[PATH_MAX];
    get_current(cur, sizeof cur);
    char ptr[512];
    int pmax = term_width() - 13;
    if (pmax > 500)
        pmax = 500;
    if (pmax < 20)
        pmax = 20;
    trunc_left(cur, (size_t)pmax, ptr, sizeof ptr);

    char fv[32], dv[32], ev[32], rv[32], sv[32], el[32];
    fmt_u64(files, fv);
    fmt_u64(dirs, dv);
    fmt_u64(errs, ev);
    fmt_u64((uint64_t)(rate + 0.5), rv);
    fmt_size(bytes, sv);
    fmt_elapsed(mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    if (!first)
        ADD("\x1b[%dA", PROG_LINES);
    ADD("\x1b[K%s%s%s %sp3m-ls%s %s— parallel scan%s\n",
        C_CYAN, spin[frame % 10], C_RESET, C_BOLD, C_RESET, C_DIM, C_RESET);
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
    fflush(stderr);
}

static void *progress_fn(void *arg)
{
    fputs("\x1b[?25l", stderr);       /* hide cursor */
    bool first = true;
    int frame = 0;
    double prev_t = mono_now(), rate = 0.0;
    uint64_t prev_items = 0;

    while (atomic_load(&scanning)) {
        double t = mono_now();
        uint64_t items =
            atomic_load_explicit(&n_files, memory_order_relaxed) +
            atomic_load_explicit(&n_dirs, memory_order_relaxed);
        double dt = t - prev_t;
        if (dt > 1e-4) {
            double inst = (double)(items - prev_items) / dt;
            rate = first ? inst : rate * 0.7 + inst * 0.3;
        }
        prev_t = t;
        prev_items = items;
        progress_draw(first, rate, frame++);
        first = false;
        struct timespec ts = { 0, 125 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* replace the live block with the final summary printed by main() */
    if (!first)
        fprintf(stderr, "\x1b[%dA\x1b[J", PROG_LINES);
    fputs("\x1b[?25h", stderr);       /* show cursor */
    fflush(stderr);
    return arg;
}

static void on_signal(int sig)
{
    /* restore the cursor before dying mid-progress-display */
    ssize_t r = write(STDERR_FILENO, "\x1b[?25h\n", 7);
    (void)r;
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ------------------------------------------------------------------ */
/* summary                                                             */
/* ------------------------------------------------------------------ */

static void print_summary(double elapsed)
{
    uint64_t files = atomic_load(&n_files);
    uint64_t dirs  = atomic_load(&n_dirs);
    uint64_t errs  = atomic_load(&n_errors);
    uint64_t bytes = atomic_load(&n_bytes);

    char fv[32], dv[32], ev[32], rv[32], sv[32], el[32];
    fmt_u64(files, fv);
    fmt_u64(dirs, dv);
    fmt_u64(errs, ev);
    fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0, rv);
    fmt_size(bytes, sv);
    fmt_elapsed(elapsed, el);

    fprintf(stderr, "%s✓%s %sp3m-ls%s complete — %s files · %s dirs · %s%s error%s%s",
            C_GREEN, C_RESET, C_BOLD, C_RESET, fv, dv,
            errs ? C_RED : "", ev, errs == 1 ? "" : "s", errs ? C_RESET : "");
    if (g.mode != MODE_BASIC)
        fprintf(stderr, " · %s", sv);
    fprintf(stderr, "\n  %s in %s (%s items/s)", mode_names[g.mode], el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
}

static void print_errors(void)
{
    uint64_t errs = atomic_load(&n_errors);
    if (!errs)
        return;
    fprintf(stderr, "%s%llu error%s encountered:%s\n", C_RED,
            (unsigned long long)errs, errs == 1 ? "" : "s", C_RESET);
    for (int i = 0; i < errlog_n; i++)
        fprintf(stderr, "  %s\n", errlog[i]);
    if (errs > (uint64_t)errlog_n)
        fprintf(stderr, "  … and %llu more\n",
                (unsigned long long)(errs - (uint64_t)errlog_n));
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                             */
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
    g.color = isatty(STDERR_FILENO);
    g.progress = g.outpath && isatty(STDERR_FILENO);

    /* open the output */
    if (g.outpath) {
        g.out = fopen(g.outpath, "w");
        if (!g.out) {
            fprintf(stderr, "p3m-ls: cannot open '%s': %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    } else {
        g.out = stdout;
    }
    static char outvbuf[1 << 20];
    setvbuf(g.out, outvbuf, _IOFBF, sizeof outvbuf);

    fputs(csv_header(), g.out);

    /* seed the work queue with the root paths */
    stk.nthreads = g.nthreads;
    outbuf_t rootob = { .buf = malloc(OB_CAP), .len = 0 };
    if (!rootob.buf) {
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
            note_error(root, "stat", errno);
            free(root);
        } else if (S_ISDIR(st.st_mode)) {
            stack_push_batch(&root, 1);
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
    ob_flush(&rootob);
    free(rootob.buf);

    if (g.progress)
        set_current("…");

    /* launch */
    t_start = mono_now();
    atomic_store(&scanning, true);

    pthread_t prog;
    if (g.progress) {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        if (pthread_create(&prog, NULL, progress_fn, NULL) != 0)
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
        /* fewer workers than planned: fix the termination threshold */
        pthread_mutex_lock(&stk.mu);
        stk.nthreads = started > 0 ? started : 1;
        pthread_cond_broadcast(&stk.cv);
        pthread_mutex_unlock(&stk.mu);
        if (started == 0) {
            fprintf(stderr, "p3m-ls: could not create any worker threads\n");
            return 2;
        }
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    double elapsed = mono_now() - t_start;

    atomic_store(&scanning, false);
    if (g.progress)
        pthread_join(prog, NULL);

    /* finish the output stream */
    if (fflush(g.out) != 0 || ferror(g.out))
        atomic_store(&write_failed, true);
    if (g.outpath && fclose(g.out) != 0)
        atomic_store(&write_failed, true);

    if (atomic_load(&write_failed)) {
        fprintf(stderr, "p3m-ls: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    if (g.outpath)
        print_summary(elapsed);
    print_errors();
    free(stk.items);

    return atomic_load(&n_errors) ? 1 : 0;
}
