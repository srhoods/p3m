/*
 * p3m-churn — estimate the change rate of a path
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Scans one or more trees repeatedly at a fixed interval and compares each
 * scan with the previous one to find files that were created, modified
 * (size or mtime), changed in metadata only (ctime) or deleted. Over the
 * observation window it reports the change rate, the data that a
 * replication tool would have to move, and the "hot" files and directories
 * that change most often — the information needed to decide whether and how
 * to add the path to a replication job.
 *
 * Memory: the state carried between scans is 40 bytes per file (a 64-bit
 * path hash plus size/mtime/ctime) in hash tables kept 35-70% full, so
 * roughly 60-115 bytes per file, plus the path of every file that has
 * actually changed (capped by --max-tracked).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct {
    double       interval;      /* seconds between scan starts            */
    long         count;         /* total scans incl. baseline; 0 = no cap */
    double       duration;      /* no new scan after this many seconds    */
    int          top;
    int          gdepth;        /* directory-group depth below each root  */
    long         max_tracked;
    int          nthreads;
    const char  *outpath;       /* per-scan CSV                           */
    const char  *hotcsv;        /* hot-file CSV                           */
    bool         quiet;
    const char **excl;
    int          nexcl;
} g = { .interval = 300, .top = 20, .gdepth = 2, .max_tracked = 1000000 };

static void fatal_oom(void)
{
    fprintf(stderr, "p3m-churn: out of memory\n");
    _exit(2);
}

/* ------------------------------------------------------------------ */
/* signals: 1st = stop after the current scan, 2nd = abort it, 3rd = die */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t sig_stop, sig_abort;

static void on_signal(int sig)
{
    static const char m1[] = "\np3m-churn: stopping after the current scan "
                             "(press again to abort it)\n";
    static const char m2[] = "\np3m-churn: aborting the current scan "
                             "(press again to quit immediately)\n";
    ssize_t r;
    if (!sig_stop) {
        sig_stop = 1;
        r = write(STDERR_FILENO, m1, sizeof m1 - 1);
    } else if (!sig_abort) {
        sig_abort = 1;
        r = write(STDERR_FILENO, m2, sizeof m2 - 1);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    (void)r;
}

/* ------------------------------------------------------------------ */
/* hashing                                                              */
/* ------------------------------------------------------------------ */

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ULL;               /* FNV-1a */
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;                                      /* murmur finaliser */
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h ? h : 1;
}

/* ------------------------------------------------------------------ */
/* directory groups: changes are rolled up per directory to --group-depth */
/* ------------------------------------------------------------------ */

struct group {
    char               *path;
    _Atomic uint64_t    created, modified, meta, deleted, bytes;
    _Atomic uint32_t    incomplete;     /* scan in which a dir read failed */
};

#define MAXG 131072
static struct group   *groups;          /* index 0 = overflow bucket        */
static uint32_t        ngroups = 1;
struct gslot { char *path; uint32_t id; };
static struct gslot   *greg;
static size_t          greg_cap, greg_used;
static pthread_mutex_t greg_mu = PTHREAD_MUTEX_INITIALIZER;

static void greg_grow(void)
{
    size_t ncap = greg_cap ? greg_cap * 2 : 1024;
    struct gslot *nt = calloc(ncap, sizeof *nt);
    if (!nt)
        fatal_oom();
    for (size_t i = 0; i < greg_cap; i++) {
        if (!greg[i].path)
            continue;
        size_t j = hash_str(greg[i].path) & (ncap - 1);
        while (nt[j].path)
            j = (j + 1) & (ncap - 1);
        nt[j] = greg[i];
    }
    free(greg);
    greg = nt;
    greg_cap = ncap;
}

static uint32_t group_id(const char *path)
{
    uint32_t id = 0;
    pthread_mutex_lock(&greg_mu);
    if (greg_used * 2 >= greg_cap)
        greg_grow();
    size_t i = hash_str(path) & (greg_cap - 1);
    while (greg[i].path) {
        if (!strcmp(greg[i].path, path)) {
            id = greg[i].id;
            goto out;
        }
        i = (i + 1) & (greg_cap - 1);
    }
    if (ngroups < MAXG) {
        char *dup = strdup(path);
        if (!dup)
            fatal_oom();
        id = ngroups++;
        greg[i].path = dup;
        greg[i].id = id;
        greg_used++;
        groups[id].path = dup;
    }
out:
    pthread_mutex_unlock(&greg_mu);
    return id;
}

/* ------------------------------------------------------------------ */
/* per-file state carried between scans (sharded open-addressing table)  */
/* ------------------------------------------------------------------ */

#define NSHARD   256
#define GEN_DEAD UINT32_MAX             /* tombstone; gen 0 = empty slot    */

struct ent {
    uint64_t hash;
    int64_t  size, mtime, ctime;
    uint32_t gen, gid;
};

struct shard {
    pthread_mutex_t mu;
    struct ent     *tab;
    size_t          cap, used, live;
};

static struct shard shards[NSHARD];
static uint32_t     cur_scan;

static void shard_resize(struct shard *s)
{
    size_t ncap = (s->live * 2 > s->cap) ? s->cap * 2 : s->cap;
    struct ent *nt = calloc(ncap, sizeof *nt);
    if (!nt)
        fatal_oom();
    for (size_t i = 0; i < s->cap; i++) {
        struct ent *e = &s->tab[i];
        if (e->gen == 0 || e->gen == GEN_DEAD)
            continue;
        size_t j = e->hash & (ncap - 1);
        while (nt[j].gen)
            j = (j + 1) & (ncap - 1);
        nt[j] = *e;
    }
    free(s->tab);
    s->tab = nt;
    s->cap = ncap;
    s->used = s->live;
}

static void shards_init(void)
{
    for (int i = 0; i < NSHARD; i++) {
        pthread_mutex_init(&shards[i].mu, NULL);
        shards[i].cap = 1024;
        shards[i].tab = calloc(shards[i].cap, sizeof(struct ent));
        if (!shards[i].tab)
            fatal_oom();
    }
}

enum ev { EV_SAME, EV_CREATE, EV_MODIFY, EV_META };

static inline int64_t ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static enum ev observe(uint64_t h, const struct stat *st, uint32_t gid)
{
    struct shard *s = &shards[h >> 56];
    int64_t size = S_ISLNK(st->st_mode) || S_ISREG(st->st_mode)
                       ? (int64_t)st->st_size : 0;
    int64_t mt = ts_ns(&st->st_mtim), ct = ts_ns(&st->st_ctim);
    enum ev ev;

    pthread_mutex_lock(&s->mu);
    size_t mask = s->cap - 1, i = h & mask;
    struct ent *tomb = NULL, *e;
    for (;;) {
        e = &s->tab[i];
        if (e->gen == 0)
            break;
        if (e->gen == GEN_DEAD) {
            if (!tomb)
                tomb = e;
        } else if (e->hash == h) {
            if (e->gen == cur_scan) {          /* seen twice this scan */
                pthread_mutex_unlock(&s->mu);
                return EV_SAME;
            }
            if (e->size != size || e->mtime != mt)
                ev = EV_MODIFY;
            else if (e->ctime != ct)
                ev = EV_META;
            else
                ev = EV_SAME;
            e->size = size;
            e->mtime = mt;
            e->ctime = ct;
            e->gen = cur_scan;
            e->gid = gid;
            pthread_mutex_unlock(&s->mu);
            return ev;
        }
        i = (i + 1) & mask;
    }
    struct ent *slot = tomb ? tomb : e;
    if (!tomb)
        s->used++;
    s->live++;
    slot->hash = h;
    slot->size = size;
    slot->mtime = mt;
    slot->ctime = ct;
    slot->gen = cur_scan;
    slot->gid = gid;
    if (s->used * 10 > s->cap * 7)
        shard_resize(s);
    pthread_mutex_unlock(&s->mu);
    return cur_scan == 1 ? EV_SAME : EV_CREATE;   /* scan 1 is the baseline */
}

/* ------------------------------------------------------------------ */
/* hot files: path -> how often it changed                              */
/* ------------------------------------------------------------------ */

struct hot {
    char    *path;
    uint64_t hash, bytes;
    uint32_t events, created, modified, last_scan;
    int64_t  size;
};

#define NHSHARD 64
struct hshard {
    pthread_mutex_t mu;
    struct hot     *tab;
    size_t          cap, used;
};
static struct hshard    hshards[NHSHARD];
static _Atomic uint64_t hot_total;
static _Atomic uint64_t hot_untracked;      /* events for paths over the cap */

static void hot_init(void)
{
    for (int i = 0; i < NHSHARD; i++) {
        pthread_mutex_init(&hshards[i].mu, NULL);
        hshards[i].cap = 256;
        hshards[i].tab = calloc(hshards[i].cap, sizeof(struct hot));
        if (!hshards[i].tab)
            fatal_oom();
    }
}

static void hot_update(const char *path, uint64_t h, enum ev ev,
                       int64_t size)
{
    struct hshard *s = &hshards[h >> 58];
    pthread_mutex_lock(&s->mu);
    size_t mask = s->cap - 1, i = h & mask;
    struct hot *e;
    for (;;) {
        e = &s->tab[i];
        if (!e->path)
            break;
        if (e->hash == h && !strcmp(e->path, path))
            goto found;
        i = (i + 1) & mask;
    }
    if (atomic_load(&hot_total) >= (uint64_t)g.max_tracked) {
        atomic_fetch_add(&hot_untracked, 1);
        pthread_mutex_unlock(&s->mu);
        return;
    }
    e->path = strdup(path);
    if (!e->path)
        fatal_oom();
    e->hash = h;
    atomic_fetch_add(&hot_total, 1);
    s->used++;
    if (s->used * 10 > s->cap * 7) {
        size_t ncap = s->cap * 2;
        struct hot *nt = calloc(ncap, sizeof *nt);
        if (!nt)
            fatal_oom();
        for (size_t k = 0; k < s->cap; k++) {
            if (!s->tab[k].path)
                continue;
            size_t j = s->tab[k].hash & (ncap - 1);
            while (nt[j].path)
                j = (j + 1) & (ncap - 1);
            nt[j] = s->tab[k];
        }
        free(s->tab);
        s->tab = nt;
        s->cap = ncap;
        /* locate the entry we just inserted in the new table */
        i = h & (ncap - 1);
        while (strcmp(nt[i].path, path))
            i = (i + 1) & (ncap - 1);
        e = &nt[i];
    }
found:
    e->events++;
    if (ev == EV_CREATE)
        e->created++;
    else
        e->modified++;
    e->bytes += (uint64_t)size;
    e->size = size;
    e->last_scan = cur_scan;
    pthread_mutex_unlock(&s->mu);
}

/* ------------------------------------------------------------------ */
/* scanning                                                             */
/* ------------------------------------------------------------------ */

struct tally {
    uint64_t files, bytes, dirs;
    uint64_t created, modified, meta, deleted, bchg, bdel;
};

static struct tally    scan_total;
static pthread_mutex_t tally_mu = PTHREAD_MUTEX_INITIALIZER;
static p3m_stack       stk;

struct hdr { uint32_t gid, depth; };

static char *pack_item(const char *path, uint32_t gid, uint32_t depth)
{
    size_t n = strlen(path) + 1;
    char *p = malloc(n + sizeof(struct hdr));
    if (!p)
        return NULL;
    struct hdr h = { gid, depth };
    memcpy(p, path, n);
    memcpy(p + n, &h, sizeof h);
    return p;
}

static bool excluded(const char *name, const char *path)
{
    for (int i = 0; i < g.nexcl; i++)
        if (!fnmatch(g.excl[i], name, 0) || !fnmatch(g.excl[i], path, 0))
            return true;
    return false;
}

static void note_file(const char *path, const struct stat *st, uint32_t gid,
                      struct tally *t)
{
    uint64_t h = hash_str(path);
    enum ev ev = observe(h, st, gid);
    int64_t size = S_ISLNK(st->st_mode) || S_ISREG(st->st_mode)
                       ? (int64_t)st->st_size : 0;
    t->files++;
    t->bytes += (uint64_t)size;
    switch (ev) {
    case EV_SAME:
        return;
    case EV_META:
        t->meta++;
        atomic_fetch_add(&groups[gid].meta, 1);
        return;
    case EV_CREATE:
        t->created++;
        atomic_fetch_add(&groups[gid].created, 1);
        break;
    case EV_MODIFY:
        t->modified++;
        atomic_fetch_add(&groups[gid].modified, 1);
        break;
    }
    t->bchg += (uint64_t)size;
    atomic_fetch_add(&groups[gid].bytes, (uint64_t)size);
    hot_update(path, h, ev, size);
}

static void scan_dir(const char *dirpath, uint32_t gid, uint32_t depth,
                     struct tally *t)
{
    if (sig_abort)
        return;
    p3m_set_current(dirpath);

    int fd = open(dirpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        if (e != ENOENT && e != ENOTDIR && e != ELOOP)
            p3m_note_error(dirpath, "open", e);
        atomic_store(&groups[gid].incomplete, cur_scan);
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
        char *fp = p3m_path_join(dirpath, dlen, nm);
        if (!fp) {
            p3m_note_error(nm, "malloc", ENOMEM);
            atomic_store(&groups[gid].incomplete, cur_scan);
            errno = 0;
            continue;
        }
        struct stat st;
        if (g.nexcl && excluded(nm, fp)) {
            free(fp);
        } else if (fstatat(dfd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT) {             /* vanished mid-scan: churn */
                p3m_note_error(fp, "stat", errno);
                atomic_store(&groups[gid].incomplete, cur_scan);
            }
            free(fp);
        } else if (S_ISDIR(st.st_mode)) {
            t->dirs++;
            uint32_t cg = depth + 1 <= (uint32_t)g.gdepth ? group_id(fp) : gid;
            char *it = pack_item(fp, cg, depth + 1);
            free(fp);
            if (it) {
                if (nsub == csub) {
                    csub = csub ? csub * 2 : 32;
                    char **ns = realloc(subs, csub * sizeof *ns);
                    if (!ns)
                        fatal_oom();
                    subs = ns;
                }
                subs[nsub++] = it;
            }
        } else {
            note_file(fp, &st, gid, t);
            free(fp);
        }
        errno = 0;
    }
    if (errno) {
        p3m_note_error(dirpath, "readdir", errno);
        atomic_store(&groups[gid].incomplete, cur_scan);
    }
    closedir(d);
    p3m_stack_push_batch(&stk, subs, nsub);
    free(subs);
}

static void *worker(void *arg)
{
    struct tally t = {0};
    char *item;
    while ((item = p3m_stack_pop(&stk)) != NULL) {
        size_t n = strlen(item) + 1;
        struct hdr h;
        memcpy(&h, item + n, sizeof h);
        scan_dir(item, h.gid, h.depth, &t);
        free(item);
    }
    pthread_mutex_lock(&tally_mu);
    scan_total.files   += t.files;
    scan_total.bytes   += t.bytes;
    scan_total.dirs    += t.dirs;
    scan_total.created += t.created;
    scan_total.modified += t.modified;
    scan_total.meta    += t.meta;
    scan_total.bchg    += t.bchg;
    pthread_mutex_unlock(&tally_mu);
    return arg;
}

/* entries not seen in this scan were deleted — except in groups where a
 * directory could not be read, where absence proves nothing */
static void sweep_deleted(struct tally *t)
{
    for (int i = 0; i < NSHARD; i++) {
        struct shard *s = &shards[i];
        for (size_t k = 0; k < s->cap; k++) {
            struct ent *e = &s->tab[k];
            if (e->gen == 0 || e->gen == GEN_DEAD || e->gen == cur_scan)
                continue;
            if (atomic_load(&groups[e->gid].incomplete) == cur_scan)
                continue;
            t->deleted++;
            t->bdel += (uint64_t)e->size;
            atomic_fetch_add(&groups[e->gid].deleted, 1);
            e->gen = GEN_DEAD;
            s->live--;
        }
    }
}

/* ------------------------------------------------------------------ */
/* scan history                                                         */
/* ------------------------------------------------------------------ */

struct scanrec {
    struct timespec wall;
    double          mono_start, dur;
    struct tally    t;
    uint64_t        errors;
};

static struct scanrec *recs;
static size_t          nrecs, crecs;

static double mono_now(void) { return p3m_mono_now(); }

/* returns false if the scan was aborted */
static bool run_scan(char **roots, int nroots, struct scanrec *rec)
{
    memset(&scan_total, 0, sizeof scan_total);
    uint64_t err0 = atomic_load(&p3m_nerrors);
    clock_gettime(CLOCK_REALTIME, &rec->wall);
    rec->mono_start = mono_now();

    p3m_stack_init(&stk, g.nthreads);
    struct tally top = {0};
    for (int i = 0; i < nroots; i++) {
        struct stat st;
        if (lstat(roots[i], &st) != 0) {
            p3m_note_error(roots[i], "stat", errno);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char *it = pack_item(roots[i], group_id(roots[i]), 0);
            if (it)
                p3m_stack_push_batch(&stk, &it, 1);
        } else {
            note_file(roots[i], &st, group_id(roots[i]), &top);
        }
    }
    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids)
        fatal_oom();
    int started = 0;
    for (int i = 0; i < g.nthreads; i++) {
        if (pthread_create(&tids[started], NULL, worker, NULL) == 0)
            started++;
    }
    if (started == 0) {
        fprintf(stderr, "p3m-churn: could not create worker threads\n");
        exit(2);
    }
    if (started < g.nthreads)
        p3m_stack_set_threads(&stk, started);
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);
    for (size_t i = 0; i < stk.len; i++)       /* drain after an abort */
        free(stk.items[i]);
    p3m_stack_destroy(&stk);

    scan_total.files += top.files;
    scan_total.bytes += top.bytes;
    scan_total.created += top.created;
    scan_total.modified += top.modified;
    scan_total.meta += top.meta;
    scan_total.bchg += top.bchg;

    if (sig_abort)
        return false;
    sweep_deleted(&scan_total);
    rec->t = scan_total;
    rec->dur = mono_now() - rec->mono_start;
    rec->errors = atomic_load(&p3m_nerrors) - err0;
    return true;
}

/* ------------------------------------------------------------------ */
/* reporting                                                            */
/* ------------------------------------------------------------------ */

static const char *fmt_dur(double s, char out[32])
{
    if (s < 60)
        snprintf(out, 32, "%.1fs", s);
    else if (s < 3600)
        snprintf(out, 32, "%dm %02ds", (int)(s / 60), (int)s % 60);
    else
        snprintf(out, 32, "%dh %02dm", (int)(s / 3600), (int)s % 3600 / 60);
    return out;
}

static void log_scan(size_t idx, const struct scanrec *r, bool overran)
{
    if (g.quiet)
        return;
    char ts[64], f[32], c[32], m[32], d[32], b[32], du[32];
    p3m_fmt_ts(&r->wall, ts, false);
    p3m_fmt_u64(r->t.files, f);
    fprintf(stderr, "scan %zu  %s  %s files", idx, ts + 11, f);
    if (idx == 1) {
        char sz[32];
        fprintf(stderr, "  %s  (baseline)", p3m_fmt_size(r->t.bytes, sz));
    } else {
        p3m_fmt_u64(r->t.created, c);
        p3m_fmt_u64(r->t.modified, m);
        p3m_fmt_u64(r->t.deleted, d);
        fprintf(stderr, "  +%s created  ~%s modified  -%s deleted  %s changed",
                c, m, d, p3m_fmt_size(r->t.bchg, b));
    }
    fprintf(stderr, "  (%s%s)%s\n", fmt_dur(r->dur, du),
            overran ? ", overran the interval" : "",
            r->errors ? "  [errors]" : "");
}

struct hotref { const struct hot *h; };

static int hot_cmp(const void *a, const void *b)
{
    const struct hot *x = ((const struct hotref *)a)->h;
    const struct hot *y = ((const struct hotref *)b)->h;
    if (x->events != y->events)
        return x->events > y->events ? -1 : 1;
    if (x->bytes != y->bytes)
        return x->bytes > y->bytes ? -1 : 1;
    return strcmp(x->path, y->path);
}

static uint64_t gscore(const struct group *g_)
{
    return atomic_load(&g_->created) + atomic_load(&g_->modified) +
           atomic_load(&g_->deleted);
}

static int group_cmp(const void *a, const void *b)
{
    const struct group *x = *(const struct group *const *)a;
    const struct group *y = *(const struct group *const *)b;
    uint64_t sx = gscore(x), sy = gscore(y);
    if (sx != sy)
        return sx > sy ? -1 : 1;
    uint64_t bx = atomic_load(&x->bytes), by = atomic_load(&y->bytes);
    if (bx != by)
        return bx > by ? -1 : 1;
    return strcmp(x->path, y->path);
}

static void fmt_avg(double v, char out[32])
{
    if (v >= 100)
        p3m_fmt_u64((uint64_t)(v + 0.5), out);
    else
        snprintf(out, 32, "%.1f", v);
}

static void row(const char *label, double avg, uint64_t peak, double perday,
                bool bytes)
{
    char a[32], p[32], d[32];
    if (bytes) {
        p3m_fmt_size((uint64_t)avg, a);
        p3m_fmt_size(peak, p);
        p3m_fmt_size((uint64_t)perday, d);
    } else {
        fmt_avg(avg, a);
        p3m_fmt_u64(peak, p);
        p3m_fmt_u64((uint64_t)perday, d);
    }
    printf("  %-22s %14s %14s %16s\n", label, a, p, d);
}

static const char *relpath(const char *p, char **roots, int nroots)
{
    if (nroots != 1)
        return p;
    size_t rl = strlen(roots[0]);
    if (!strncmp(p, roots[0], rl)) {
        if (p[rl] == '/')
            return p + rl + 1;
        if (p[rl] == '\0')
            return ".";
    }
    return p;
}

static void report(char **roots, int nroots)
{
    p3m_color = isatty(STDOUT_FILENO);
    size_t nint = nrecs > 1 ? nrecs - 1 : 0;
    printf("%sp3m-churn%s — change-rate report\n", C_BOLD, C_RESET);
    if (nint == 0) {
        printf("  not enough data: at least two complete scans are needed "
               "(%zu completed)\n", nrecs);
        return;
    }
    const struct scanrec *first = &recs[0], *last = &recs[nrecs - 1];
    double window = last->mono_start - first->mono_start;
    char t0[64], t1[64], wd[32], iv[32], nb[32], sb[32], ld[32], dbuf[32];
    p3m_fmt_ts(&first->wall, t0, false);
    p3m_fmt_ts(&last->wall, t1, false);

    printf("  path        %s", roots[0]);
    if (nroots > 1)
        printf(" (+%d more)", nroots - 1);
    printf("\n  window      %s .. %s  (%s, %zu scans, every %s)\n", t0, t1,
           fmt_dur(window, wd), nrecs, fmt_dur(g.interval, iv));
    printf("  scanned     %s files · %s · %s dirs  (last scan took %s)\n",
           p3m_fmt_u64(last->t.files, nb), p3m_fmt_size(last->t.bytes, sb),
           p3m_fmt_u64(last->t.dirs, dbuf), fmt_dur(last->dur, ld));

    /* totals and peaks over the observed intervals */
    uint64_t tc = 0, tm = 0, tmeta = 0, td = 0, tb = 0, tbd = 0;
    uint64_t pc = 0, pm = 0, pmeta = 0, pd = 0, pb = 0, pf = 0, pbd = 0;
    uint64_t sumfiles = 0;
    uint64_t errs = 0;
    for (size_t i = 1; i < nrecs; i++) {
        const struct tally *t = &recs[i].t;
        tc += t->created; tm += t->modified; tmeta += t->meta;
        td += t->deleted; tb += t->bchg; tbd += t->bdel;
        uint64_t chg = t->created + t->modified + t->deleted;
        if (t->created > pc) pc = t->created;
        if (t->modified > pm) pm = t->modified;
        if (t->meta > pmeta) pmeta = t->meta;
        if (t->deleted > pd) pd = t->deleted;
        if (t->bchg > pb) pb = t->bchg;
        if (t->bdel > pbd) pbd = t->bdel;
        if (chg > pf) pf = chg;
        sumfiles += t->files;
        errs += recs[i].errors;
    }
    uint64_t tchg = tc + tm + td;
    double perday = window > 0 ? 86400.0 / window : 0;

    printf("\n%sChange rate%s (%zu interval%s observed)\n", C_BOLD, C_RESET,
           nint, nint == 1 ? "" : "s");
    printf("  %-22s %14s %14s %16s\n", "", "avg/interval", "peak/interval",
           "extrapolated/day");
    double n = (double)nint;
    row("files changed", (double)tchg / n, pf, (double)tchg * perday, false);
    row("  created", (double)tc / n, pc, (double)tc * perday, false);
    row("  modified", (double)tm / n, pm, (double)tm * perday, false);
    row("  deleted", (double)td / n, pd, (double)td * perday, false);
    row("  metadata only", (double)tmeta / n, pmeta, (double)tmeta * perday,
        false);
    row("data to re-replicate", (double)tb / n, pb, (double)tb * perday, true);
    row("data deleted", (double)tbd / n, pbd, (double)tbd * perday, true);
    printf("  %sdata to re-replicate = size of created + modified files "
           "(whole-file replication);\n  block/delta-based replication will "
           "move less.%s\n", C_DIM, C_RESET);

    uint64_t avgfiles = sumfiles / nint;
    uint64_t uniq = atomic_load(&hot_total);
    char ub[32];
    printf("\n  files changed per interval: %.3f%% of the tree on average\n",
           avgfiles ? 100.0 * ((double)tchg / n) / (double)avgfiles : 0.0);
    printf("  distinct files created/modified over the window: %s "
           "(%.2f%% of the %s files in the last scan)\n",
           p3m_fmt_u64(uniq, ub),
           last->t.files ? 100.0 * (double)uniq / (double)last->t.files : 0.0,
           p3m_fmt_u64(last->t.files, nb));
    uint64_t untr = atomic_load(&hot_untracked);
    if (untr)
        printf("  %s%s change events were not tracked per file "
               "(--max-tracked reached)%s\n", C_DIM,
               p3m_fmt_u64(untr, nb), C_RESET);
    if (errs)
        printf("  %s%s scan error%s occurred; directories that could not be "
               "read are excluded from deletion counts%s\n", C_RED,
               p3m_fmt_u64(errs, nb), errs == 1 ? "" : "s", C_RESET);

    /* gather and sort tracked files */
    size_t nh = 0, ch = (size_t)uniq;
    struct hotref *hv = malloc((ch ? ch : 1) * sizeof *hv);
    if (!hv)
        fatal_oom();
    for (int i = 0; i < NHSHARD; i++)
        for (size_t k = 0; k < hshards[i].cap; k++)
            if (hshards[i].tab[k].path && nh < ch)
                hv[nh++].h = &hshards[i].tab[k];
    qsort(hv, nh, sizeof *hv, hot_cmp);

    uint64_t tot_ev = 0, always = 0;
    for (size_t i = 0; i < nh; i++) {
        tot_ev += hv[i].h->events;
        if ((double)hv[i].h->events >= 0.9 * (double)nint)
            always++;
    }
    size_t topn = (size_t)g.top < nh ? (size_t)g.top : nh;

    printf("\n%sHot files%s (changed in the most scans%s)\n", C_BOLD, C_RESET,
           nroots == 1 ? "; paths relative to the scanned path" : "");
    if (!nh) {
        printf("  (no files were created or modified)\n");
    } else {
        printf("  %7s %6s %5s %5s %12s %12s  %s\n", "scans", "%scans",
               "new", "mod", "bytes/chg", "last size", "path");
        for (size_t i = 0; i < topn; i++) {
            const struct hot *h = hv[i].h;
            char sz[32], av[32];
            printf("  %7u %5.0f%% %5u %5u %12s %12s  %s\n", h->events,
                   100.0 * h->events / (double)nint, h->created, h->modified,
                   p3m_fmt_size(h->bytes / (h->events ? h->events : 1), av),
                   p3m_fmt_size((uint64_t)h->size, sz),
                   relpath(h->path, roots, nroots));
        }
        if (nh > topn)
            printf("  %s… and %zu more changed files (see --hot-csv)%s\n",
                   C_DIM, nh - topn, C_RESET);
    }

    /* directory groups */
    struct group **gv = malloc(ngroups * sizeof *gv);
    size_t ngv = 0;
    if (!gv)
        fatal_oom();
    for (uint32_t i = 0; i < ngroups; i++)
        if (groups[i].path && gscore(&groups[i]) + atomic_load(&groups[i].meta))
            gv[ngv++] = &groups[i];
    qsort(gv, ngv, sizeof *gv, group_cmp);
    size_t topg = (size_t)g.top < ngv ? (size_t)g.top : ngv;
    printf("\n%sHot directories%s (changes rolled up to depth %d)\n", C_BOLD,
           C_RESET, g.gdepth);
    if (!ngv) {
        printf("  (no changes)\n");
    } else {
        printf("  %9s %9s %9s %9s %12s  %s\n", "events", "created",
               "modified", "deleted", "data chg", "path");
        for (size_t i = 0; i < topg; i++) {
            const struct group *q = gv[i];
            char a[32], b[32], c[32], d[32];
            printf("  %9s %9s %9s %9s %12s  %s\n",
                   p3m_fmt_u64(gscore(q), a),
                   p3m_fmt_u64(atomic_load(&q->created), b),
                   p3m_fmt_u64(atomic_load(&q->modified), c),
                   p3m_fmt_u64(atomic_load(&q->deleted), d),
                   p3m_fmt_size(atomic_load(&q->bytes), sb),
                   relpath(q->path, roots, nroots));
        }
        if (ngv > topg)
            printf("  %s… and %zu more directories%s\n", C_DIM, ngv - topg,
                   C_RESET);
    }

    /* what it means for replication */
    printf("\n%sReplication notes%s\n", C_BOLD, C_RESET);
    if (nh && tot_ev) {
        uint64_t acc = 0;
        size_t n80 = 0;
        for (; n80 < nh && acc * 10 < tot_ev * 8; n80++)
            acc += hv[n80].h->events;
        printf("  - 80%% of the tracked change events come from %zu file%s "
               "(%.2f%% of the tree)\n", n80, n80 == 1 ? "" : "s",
               last->t.files ? 100.0 * (double)n80 / (double)last->t.files
                             : 0.0);
    }
    if (always)
        printf("  - %s file%s changed in at least 90%% of the intervals "
               "(continuously written: logs, databases, journals?) — these "
               "will be re-sent on every replication cycle; consider "
               "excluding them or replicating from a snapshot\n",
               p3m_fmt_u64(always, nb), always == 1 ? "" : "s");
    if (tc > 0 && td * 2 >= tc && td > 0)
        printf("  - creates and deletes are of similar volume (%s created, "
               "%s deleted): likely temporary or rotated files; check the hot "
               "directories\n", p3m_fmt_u64(tc, nb), p3m_fmt_u64(td, ub));
    if (tchg == 0)
        printf("  - no data changes were seen in the window: the path looks "
               "static\n");
    if (window < 3600)
        printf("  - %sthe window is short (%s); per-day figures are "
               "extrapolated and may not reflect daily or weekly cycles — run "
               "longer, or at several times of day%s\n", C_DIM,
               fmt_dur(window, wd), C_RESET);
    printf("  - changes between two scans are collapsed (a file rewritten "
           "several times in one interval counts once), so rates are lower "
           "bounds; shorten --interval to resolve bursts\n");

    if (g.hotcsv) {
        FILE *f = fopen(g.hotcsv, "w");
        if (!f) {
            fprintf(stderr, "p3m-churn: cannot open '%s': %s\n", g.hotcsv,
                    strerror(errno));
        } else {
            fputs("path,events,pct_scans,created,modified,bytes_changed,"
                  "last_size,last_scan\n", f);
            for (size_t i = 0; i < nh; i++) {
                const struct hot *h = hv[i].h;
                const char *p = h->path;
                bool q = p[strcspn(p, ",\"\n\r")] != '\0';
                if (q) {
                    fputc('"', f);
                    for (; *p; p++) {
                        if (*p == '"')
                            fputc('"', f);
                        fputc(*p, f);
                    }
                    fputc('"', f);
                } else {
                    fputs(p, f);
                }
                fprintf(f, ",%u,%.1f,%u,%u,%" PRIu64 ",%" PRId64 ",%u\n",
                        h->events, 100.0 * h->events / (double)nint,
                        h->created, h->modified, h->bytes, h->size,
                        h->last_scan);
            }
            fclose(f);
        }
    }
    free(hv);
    free(gv);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-churn [OPTIONS] PATH...\n"
"\n"
"Estimate how fast a path changes, for deciding whether (and how) to add it\n"
"to a replication job. PATH is scanned repeatedly at a fixed interval; each\n"
"scan is compared with the previous one to find files that were created,\n"
"modified (size or mtime), changed in metadata only (ctime) or deleted. The\n"
"first scan is the baseline. A report is printed when the run ends: change\n"
"rate per interval and per day, the data a replication would have to move,\n"
"the hottest (most often changing) files and directories, and notes on what\n"
"that means for replication.\n"
"\n"
"Options:\n"
"  -i, --interval SECS    seconds between scan starts (default 300; min 0.1);\n"
"                         a scan that overruns it is followed immediately by\n"
"                         the next\n"
"  -c, --count N          stop after N scans in total, including the baseline\n"
"                         (at least 2; default: run until stopped)\n"
"  -t, --duration SECS    start no new scan after SECS seconds\n"
"  -n, --top N            length of the hot-file and hot-directory lists\n"
"                         (default 20)\n"
"  -d, --group-depth N    roll directory changes up to N levels below each\n"
"                         PATH (default 2)\n"
"      --exclude PATTERN  skip entries whose name or path matches the glob\n"
"                         PATTERN (repeatable); excluded directories are not\n"
"                         entered\n"
"      --max-tracked N    remember at most N distinct changed file paths\n"
"                         (default 1000000); further changes are counted but\n"
"                         not listed per file\n"
"  -j, --threads N        scan threads (default: number of online CPUs)\n"
"  -o, --output FILE      write one CSV row per scan to FILE\n"
"      --hot-csv FILE     write every tracked changed file to FILE\n"
"  -q, --quiet            do not print a line per scan on stderr\n"
"  -h, --help             show this help and exit\n"
"  -V, --version          show version and exit\n"
"\n"
"SIGINT/SIGTERM: the first lets the scan in progress finish, then prints the\n"
"report; a second aborts the scan in progress; a third quits at once.\n"
"Symbolic links are never followed.\n"
"Exit status: 0 success, 1 fewer than two complete scans or scan errors,\n"
"2 usage error.\n",
    to);
}

static bool parse_num(const char *s, double *out, double min)
{
    char *end;
    double v = strtod(s, &end);
    if (*end || end == s || v < min || v > 1e9)
        return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "interval",    required_argument, NULL, 'i' },
        { "count",       required_argument, NULL, 'c' },
        { "duration",    required_argument, NULL, 't' },
        { "top",         required_argument, NULL, 'n' },
        { "group-depth", required_argument, NULL, 'd' },
        { "exclude",     required_argument, NULL, 1000 },
        { "max-tracked", required_argument, NULL, 1001 },
        { "hot-csv",     required_argument, NULL, 1002 },
        { "threads",     required_argument, NULL, 'j' },
        { "output",      required_argument, NULL, 'o' },
        { "quiet",       no_argument,       NULL, 'q' },
        { "help",        no_argument,       NULL, 'h' },
        { "version",     no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };
    const char **excl = calloc((size_t)argc + 1, sizeof *excl);
    if (!excl)
        fatal_oom();

    int c;
    while ((c = getopt_long(argc, argv, "i:c:t:n:d:j:o:qhV", lopts, NULL)) != -1) {
        double v;
        switch (c) {
        case 'i':
            if (!parse_num(optarg, &v, 0.1)) {
                fprintf(stderr, "p3m-churn: invalid interval '%s' "
                        "(expected seconds >= 0.1)\n", optarg);
                return 2;
            }
            g.interval = v;
            break;
        case 'c':
            if (!parse_num(optarg, &v, 2) || v != (long)v) {
                fprintf(stderr, "p3m-churn: invalid count '%s' "
                        "(expected an integer >= 2)\n", optarg);
                return 2;
            }
            g.count = (long)v;
            break;
        case 't':
            if (!parse_num(optarg, &v, 0.1)) {
                fprintf(stderr, "p3m-churn: invalid duration '%s'\n", optarg);
                return 2;
            }
            g.duration = v;
            break;
        case 'n':
            if (!parse_num(optarg, &v, 0) || v != (long)v || v > 100000) {
                fprintf(stderr, "p3m-churn: invalid --top '%s'\n", optarg);
                return 2;
            }
            g.top = (int)v;
            break;
        case 'd':
            if (!parse_num(optarg, &v, 0) || v != (long)v || v > 64) {
                fprintf(stderr, "p3m-churn: invalid --group-depth '%s' "
                        "(expected 0-64)\n", optarg);
                return 2;
            }
            g.gdepth = (int)v;
            break;
        case 1000:
            excl[g.nexcl++] = optarg;
            break;
        case 1001:
            if (!parse_num(optarg, &v, 1) || v != (long)v) {
                fprintf(stderr, "p3m-churn: invalid --max-tracked '%s'\n",
                        optarg);
                return 2;
            }
            g.max_tracked = (long)v;
            break;
        case 1002:
            g.hotcsv = optarg;
            break;
        case 'j':
            if (!parse_num(optarg, &v, 1) || v != (long)v || v > 512) {
                fprintf(stderr, "p3m-churn: invalid thread count '%s' "
                        "(expected 1-512)\n", optarg);
                return 2;
            }
            g.nthreads = (int)v;
            break;
        case 'o': g.outpath = optarg; break;
        case 'q': g.quiet = true; break;
        case 'h': usage(stdout); return 0;
        case 'V': p3m_print_version("p3m-churn"); return 0;
        default:  usage(stderr); return 2;
        }
    }
    g.excl = excl;
    if (optind >= argc) {
        fprintf(stderr, "p3m-churn: expected at least one PATH\n");
        usage(stderr);
        return 2;
    }
    int nroots = argc - optind;
    char **roots = calloc((size_t)nroots, sizeof *roots);
    if (!roots)
        fatal_oom();
    for (int i = 0; i < nroots; i++) {
        roots[i] = strdup(argv[optind + i]);
        if (!roots[i])
            fatal_oom();
        size_t n = strlen(roots[i]);
        while (n > 1 && roots[i][n - 1] == '/')
            roots[i][--n] = '\0';
        struct stat st;
        if (lstat(roots[i], &st) != 0) {
            fprintf(stderr, "p3m-churn: cannot access '%s': %s\n", roots[i],
                    strerror(errno));
            return 2;
        }
    }
    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);

    FILE *scancsv = NULL;
    if (g.outpath) {
        scancsv = fopen(g.outpath, "w");
        if (!scancsv) {
            fprintf(stderr, "p3m-churn: cannot open '%s': %s\n", g.outpath,
                    strerror(errno));
            return 2;
        }
        fputs("scan,start,duration_s,files,bytes,dirs,created,modified,"
              "metadata,deleted,bytes_changed,bytes_deleted,errors\n",
              scancsv);
    }

    groups = calloc(MAXG, sizeof *groups);
    if (!groups)
        fatal_oom();
    groups[0].path = "(other)";
    shards_init();
    hot_init();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    double t0 = mono_now();
    bool warned_overrun = false;
    for (uint32_t scan = 1;; scan++) {
        cur_scan = scan;
        struct scanrec rec;
        memset(&rec, 0, sizeof rec);
        bool ok = run_scan(roots, nroots, &rec);
        if (!ok) {
            if (!g.quiet)
                fprintf(stderr, "scan %u aborted; its partial results are "
                        "discarded from the totals\n", scan);
            break;
        }
        if (nrecs == crecs) {
            crecs = crecs ? crecs * 2 : 64;
            recs = realloc(recs, crecs * sizeof *recs);
            if (!recs)
                fatal_oom();
        }
        recs[nrecs++] = rec;

        double next = rec.mono_start + g.interval;
        bool overran = mono_now() > next;
        log_scan(nrecs, &rec, overran);
        if (overran && !warned_overrun && !g.quiet) {
            warned_overrun = true;
            fprintf(stderr, "p3m-churn: a scan took longer than the "
                    "interval; effective sampling is slower than requested\n");
        }
        if (scancsv) {
            char ts[64];
            p3m_fmt_ts(&rec.wall, ts, false);
            fprintf(scancsv, "%zu,%s,%.3f,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", nrecs, ts,
                    rec.dur, rec.t.files, rec.t.bytes, rec.t.dirs,
                    rec.t.created, rec.t.modified, rec.t.meta, rec.t.deleted,
                    rec.t.bchg, rec.t.bdel, rec.errors);
            fflush(scancsv);
        }

        if (sig_stop)
            break;
        if (g.count && (long)nrecs >= g.count)
            break;
        if (g.duration > 0 && next - t0 > g.duration)
            break;
        while (!sig_stop && mono_now() < next) {
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (sig_stop)
            break;
    }
    if (scancsv)
        fclose(scancsv);

    report(roots, nroots);

    p3m_print_errors();
    if (nrecs < 2)
        return 1;
    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
