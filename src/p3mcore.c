/* p3mcore — shared engine for the p3m tool suite */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

bool p3m_color;

/* ------------------------------------------------------------------ */
/* formatting helpers                                                   */
/* ------------------------------------------------------------------ */

double p3m_mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

char *p3m_fmt_u64(uint64_t v, char out[32])
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

char *p3m_fmt_size(uint64_t b, char out[32])
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

char *p3m_fmt_elapsed(double s, char out[32])
{
    if (s < 60.0)
        snprintf(out, 32, "%.1fs", s);
    else
        snprintf(out, 32, "%dm %02ds", (int)(s / 60.0), (int)s % 60);
    return out;
}

void p3m_fmt_ts(const struct timespec *ts, char out[64], bool with_ns)
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

void p3m_fmt_perms(mode_t m, char out[11])
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

char p3m_dt_char(unsigned char dt)
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

int p3m_term_width(void)
{
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

void p3m_trunc_left(const char *s, size_t max, char *out, size_t outsz)
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

char *p3m_path_join(const char *dir, size_t dlen, const char *name)
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

/* ------------------------------------------------------------------ */
/* error accounting                                                     */
/* ------------------------------------------------------------------ */

_Atomic uint64_t p3m_nerrors;

#define ERRLOG_MAX 24
static char           *errlog[ERRLOG_MAX];
static int             errlog_n;
static pthread_mutex_t errlog_mu = PTHREAD_MUTEX_INITIALIZER;

void p3m_note_error(const char *path, const char *what, int err)
{
    atomic_fetch_add_explicit(&p3m_nerrors, 1, memory_order_relaxed);
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

void p3m_print_errors(void)
{
    uint64_t errs = atomic_load(&p3m_nerrors);
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
/* "current path"                                                       */
/* ------------------------------------------------------------------ */

static char            cur_path[PATH_MAX];
static pthread_mutex_t cur_mu = PTHREAD_MUTEX_INITIALIZER;

void p3m_set_current(const char *p)
{
    pthread_mutex_lock(&cur_mu);
    snprintf(cur_path, sizeof cur_path, "%s", p);
    pthread_mutex_unlock(&cur_mu);
}

void p3m_get_current(char *dst, size_t n)
{
    pthread_mutex_lock(&cur_mu);
    snprintf(dst, n, "%s", cur_path);
    pthread_mutex_unlock(&cur_mu);
}

/* ------------------------------------------------------------------ */
/* work stack                                                           */
/* ------------------------------------------------------------------ */

void p3m_stack_init(p3m_stack *s, int nthreads)
{
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->nthreads = nthreads;
}

void p3m_stack_set_threads(p3m_stack *s, int nthreads)
{
    pthread_mutex_lock(&s->mu);
    s->nthreads = nthreads;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

void p3m_stack_push_batch(p3m_stack *s, char **paths, size_t n)
{
    if (!n)
        return;
    pthread_mutex_lock(&s->mu);
    if (s->len + n > s->cap) {
        size_t nc = s->cap ? s->cap : 256;
        while (nc < s->len + n)
            nc *= 2;
        char **ni = realloc(s->items, nc * sizeof *ni);
        if (!ni) {
            pthread_mutex_unlock(&s->mu);
            for (size_t i = 0; i < n; i++) {
                p3m_note_error(paths[i], "queue", ENOMEM);
                free(paths[i]);
            }
            return;
        }
        s->items = ni;
        s->cap = nc;
    }
    memcpy(s->items + s->len, paths, n * sizeof *paths);
    s->len += n;
    if (n == 1)
        pthread_cond_signal(&s->cv);
    else
        pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

char *p3m_stack_pop(p3m_stack *s)
{
    pthread_mutex_lock(&s->mu);
    while (s->len == 0 && !s->done) {
        s->idle++;
        if (s->idle >= s->nthreads) {
            s->done = true;
            pthread_cond_broadcast(&s->cv);
        } else {
            pthread_cond_wait(&s->cv, &s->mu);
        }
        s->idle--;
    }
    char *p = NULL;
    if (s->len > 0)
        p = s->items[--s->len];
    pthread_mutex_unlock(&s->mu);
    return p;
}

void p3m_stack_destroy(p3m_stack *s)
{
    free(s->items);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
}

/* ------------------------------------------------------------------ */
/* buffered output                                                      */
/* ------------------------------------------------------------------ */

void p3m_sink_init(p3m_sink *k, FILE *f)
{
    k->f = f;
    pthread_mutex_init(&k->mu, NULL);
    atomic_store(&k->failed, false);
}

int p3m_ob_init(p3m_outbuf *ob, p3m_sink *k)
{
    ob->buf = malloc(P3M_OB_CAP);
    ob->len = 0;
    ob->sink = k;
    return ob->buf ? 0 : -1;
}

void p3m_ob_free(p3m_outbuf *ob)
{
    free(ob->buf);
    ob->buf = NULL;
}

void p3m_ob_flush(p3m_outbuf *ob)
{
    if (!ob->len)
        return;
    p3m_sink *k = ob->sink;
    pthread_mutex_lock(&k->mu);
    size_t w = fwrite(ob->buf, 1, ob->len, k->f);
    pthread_mutex_unlock(&k->mu);
    if (w != ob->len)
        atomic_store(&k->failed, true);
    ob->len = 0;
}

bool p3m_ob_room(p3m_outbuf *ob, size_t need)
{
    if (need > P3M_OB_CAP)
        return false;
    if (P3M_OB_CAP - ob->len < need)
        p3m_ob_flush(ob);
    return true;
}

void p3m_ob_puts(p3m_outbuf *ob, const char *s)
{
    size_t n = strlen(s);
    memcpy(ob->buf + ob->len, s, n);
    ob->len += n;
}

void p3m_ob_putc(p3m_outbuf *ob, char c)
{
    ob->buf[ob->len++] = c;
}

void p3m_ob_fmt(p3m_outbuf *ob, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(ob->buf + ob->len, P3M_OB_CAP - ob->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        ob->len += (size_t)n;
}

void p3m_ob_csv(p3m_outbuf *ob, const char *s)
{
    if (!s[strcspn(s, ",\"\n\r")]) {
        p3m_ob_puts(ob, s);
        return;
    }
    p3m_ob_putc(ob, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"')
            p3m_ob_putc(ob, '"');
        p3m_ob_putc(ob, *p);
    }
    p3m_ob_putc(ob, '"');
}

/* ------------------------------------------------------------------ */
/* uid/gid name caches (immutable entries, tiny linear scan)            */
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

const char *p3m_uid_name(uid_t id, char fallback[32])
{
    return id_name(&users, (uint32_t)id, true, fallback);
}

const char *p3m_gid_name(gid_t id, char fallback[32])
{
    return id_name(&groups, (uint32_t)id, false, fallback);
}

static bool all_digits(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

int p3m_resolve_user(const char *s, uid_t *out)
{
    char buf[4096];
    struct passwd pw, *res = NULL;
    if (!getpwnam_r(s, &pw, buf, sizeof buf, &res) && res) {
        *out = pw.pw_uid;
        return 0;
    }
    if (all_digits(s)) {
        *out = (uid_t)strtoul(s, NULL, 10);
        return 0;
    }
    return -1;
}

int p3m_resolve_group(const char *s, gid_t *out)
{
    char buf[4096];
    struct group gr, *res = NULL;
    if (!getgrnam_r(s, &gr, buf, sizeof buf, &res) && res) {
        *out = gr.gr_gid;
        return 0;
    }
    if (all_digits(s)) {
        *out = (gid_t)strtoul(s, NULL, 10);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* progress display                                                     */
/* ------------------------------------------------------------------ */

const char *p3m_spinner[10] = { "⠋", "⠙", "⠹", "⠸", "⠼",
                                "⠴", "⠦", "⠧", "⠇", "⠏" };

static struct {
    p3m_progress_cfg cfg;
    pthread_t        tid;
    atomic_bool      running;
    bool             active;
} prog;

static void prog_signal(int sig)
{
    /* restore the cursor before dying mid-display */
    ssize_t r = write(STDERR_FILENO, "\x1b[?25h\n", 7);
    (void)r;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void *prog_fn(void *arg)
{
    fputs("\x1b[?25l", stderr);       /* hide cursor */
    bool first = true;
    int frame = 0;
    double prev_t = p3m_mono_now(), rate = 0.0;
    uint64_t prev_items = 0;

    while (atomic_load(&prog.running)) {
        double t = p3m_mono_now();
        uint64_t items = prog.cfg.items();
        double dt = t - prev_t;
        if (dt > 1e-4) {
            double inst = (double)(items - prev_items) / dt;
            rate = first ? inst : rate * 0.7 + inst * 0.3;
        }
        prev_t = t;
        prev_items = items;
        if (!first)
            fprintf(stderr, "\x1b[%dA", prog.cfg.lines);
        prog.cfg.draw(rate, frame++);
        fflush(stderr);
        first = false;
        struct timespec ts = { 0, 125 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* clear the live block; the tool prints its final summary after */
    if (!first)
        fprintf(stderr, "\x1b[%dA\x1b[J", prog.cfg.lines);
    fputs("\x1b[?25h", stderr);       /* show cursor */
    fflush(stderr);
    return arg;
}

int p3m_progress_start(const p3m_progress_cfg *cfg)
{
    prog.cfg = *cfg;
    atomic_store(&prog.running, true);
    signal(SIGINT, prog_signal);
    signal(SIGTERM, prog_signal);
    if (pthread_create(&prog.tid, NULL, prog_fn, NULL) != 0)
        return -1;
    prog.active = true;
    return 0;
}

void p3m_progress_stop(void)
{
    if (!prog.active)
        return;
    atomic_store(&prog.running, false);
    pthread_join(prog.tid, NULL);
    prog.active = false;
}
