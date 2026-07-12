/*
 * p3m-comp — parallel directory tree comparison
 *
 * Walks two directory trees in lockstep with a pool of worker threads
 * and reports every difference: entries present on only one side, type
 * mismatches, size mismatches, metadata differences (mode, owner,
 * group, mtime) and — with -c — file content differences.
 *
 * Content comparison is byte-exact: both files are read in chunks and
 * compared directly, stopping at the first differing byte, which is
 * stronger than comparing checksums and never produces a false match.
 * File size is always checked first; when the sizes differ the content
 * read is skipped, since the difference is already established.
 *
 * Symbolic links are never followed: they are compared as links (by
 * target, owner and group) and never descended into.
 *
 * Exit status is diff-like: 0 = no differences, 1 = differences found,
 * 2 = usage error or the comparison hit errors (verdict unreliable).
 */
#include "p3mcore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define P3M_COMP_VERSION "1.0.0"

static struct {
    bool         checksum;    /* -c: also compare file contents        */
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
    char        *left;        /* the two roots, trailing slashes trimmed */
    char        *right;
    size_t       llen, rlen;
} g;

static _Atomic uint64_t n_files;    /* non-directory entries examined   */
static _Atomic uint64_t n_dirs;    /* directories examined              */
static _Atomic uint64_t n_diffs;   /* total difference rows             */
static _Atomic uint64_t n_onlyl;   /* entries only in LEFT              */
static _Atomic uint64_t n_onlyr;   /* entries only in RIGHT             */
static _Atomic uint64_t n_type;    /* type mismatches                   */
static _Atomic uint64_t n_size;    /* size mismatches                   */
static _Atomic uint64_t n_content; /* content mismatches (-c)           */
static _Atomic uint64_t n_meta;    /* metadata mismatches               */
static _Atomic uint64_t n_bytes;   /* content bytes compared (-c)       */

static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* difference rows: path,difference,left,right                          */
/* ------------------------------------------------------------------ */

static void emit_row(p3m_outbuf *ob, const char *rel, const char *what,
                     const char *lv, const char *rv)
{
    atomic_fetch_add_explicit(&n_diffs, 1, memory_order_relaxed);
    if (g.suppress)
        return;
    if (!p3m_ob_room(ob, 2 * strlen(rel) + strlen(lv) + strlen(rv) + 64)) {
        p3m_note_error(rel, "emit", ENAMETOOLONG);
        return;
    }
    p3m_ob_csv(ob, rel);
    p3m_ob_putc(ob, ',');
    p3m_ob_puts(ob, what);
    p3m_ob_putc(ob, ',');
    p3m_ob_csv(ob, lv);
    p3m_ob_putc(ob, ',');
    p3m_ob_csv(ob, rv);
    p3m_ob_putc(ob, '\n');
}

/* ------------------------------------------------------------------ */
/* path helpers                                                         */
/* ------------------------------------------------------------------ */

/* root + "/" + rel; rel == "" means the root itself */
static char *root_join(const char *root, size_t rootlen, const char *rel)
{
    if (rel[0] == '\0')
        return strdup(root);
    return p3m_path_join(root, rootlen, rel);
}

/* rel + "/" + name; rel == "" means top level */
static char *rel_join(const char *rel, const char *name)
{
    if (rel[0] == '\0')
        return strdup(name);
    return p3m_path_join(rel, strlen(rel), name);
}

/* ------------------------------------------------------------------ */
/* content comparison (-c): byte-exact, first difference wins           */
/* ------------------------------------------------------------------ */

#define CMP_CHUNK ((size_t)1 << 20)

static __thread char *cmp_bufa, *cmp_bufb;

static ssize_t read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* 1 = differ (*off = first differing byte), 0 = identical, -1 = error */
static int fd_differs(int fa, int fb, uint64_t *off)
{
    if (!cmp_bufa) {
        cmp_bufa = malloc(CMP_CHUNK);
        cmp_bufb = malloc(CMP_CHUNK);
        if (!cmp_bufa || !cmp_bufb) {
            errno = ENOMEM;
            return -1;
        }
    }
    uint64_t pos = 0;
    for (;;) {
        ssize_t ra = read_full(fa, cmp_bufa, CMP_CHUNK);
        if (ra < 0)
            return -1;
        ssize_t rb = read_full(fb, cmp_bufb, CMP_CHUNK);
        if (rb < 0)
            return -1;
        size_t n = ra < rb ? (size_t)ra : (size_t)rb;
        if (n && memcmp(cmp_bufa, cmp_bufb, n) != 0) {
            size_t k = 0;
            while (k < n && cmp_bufa[k] == cmp_bufb[k])
                k++;
            *off = pos + k;
            return 1;
        }
        atomic_fetch_add_explicit(&n_bytes, n, memory_order_relaxed);
        pos += n;
        if (ra != rb) {           /* file changed size mid-comparison */
            *off = pos;
            return 1;
        }
        if (ra == 0)
            return 0;
    }
}

static void compare_content(int dfa, int dfb, const char *name,
                            const char *rel, p3m_outbuf *ob)
{
    int fa = openat(dfa, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fa < 0) {
        p3m_note_error(rel, "open (left)", errno);
        return;
    }
    int fb = openat(dfb, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fb < 0) {
        p3m_note_error(rel, "open (right)", errno);
        close(fa);
        return;
    }
    uint64_t off = 0;
    int rc = fd_differs(fa, fb, &off);
    if (rc < 0) {
        p3m_note_error(rel, "read", errno);
    } else if (rc == 1) {
        char ov[48];
        snprintf(ov, sizeof ov, "differ at byte %" PRIu64, off);
        atomic_fetch_add_explicit(&n_content, 1, memory_order_relaxed);
        emit_row(ob, rel, "content", ov, "");
    }
    close(fa);
    close(fb);
}

/* ------------------------------------------------------------------ */
/* metadata comparison (both sides same type)                           */
/* ------------------------------------------------------------------ */

static void meta_note(p3m_outbuf *ob, const char *rel, const char *what,
                      const char *lv, const char *rv)
{
    atomic_fetch_add_explicit(&n_meta, 1, memory_order_relaxed);
    emit_row(ob, rel, what, lv, rv);
}

static void compare_meta(const char *rel, const struct stat *sa,
                         const struct stat *sb, p3m_outbuf *ob)
{
    if ((sa->st_mode & 07777) != (sb->st_mode & 07777)) {
        char lv[16], rv[16];
        snprintf(lv, sizeof lv, "%04o", sa->st_mode & 07777);
        snprintf(rv, sizeof rv, "%04o", sb->st_mode & 07777);
        meta_note(ob, rel, "mode", lv, rv);
    }
    if (sa->st_uid != sb->st_uid) {
        char fa[32], fb[32];
        meta_note(ob, rel, "owner", p3m_uid_name(sa->st_uid, fa),
                  p3m_uid_name(sb->st_uid, fb));
    }
    if (sa->st_gid != sb->st_gid) {
        char fa[32], fb[32];
        meta_note(ob, rel, "group", p3m_gid_name(sa->st_gid, fa),
                  p3m_gid_name(sb->st_gid, fb));
    }
    if (sa->st_mtim.tv_sec != sb->st_mtim.tv_sec ||
        sa->st_mtim.tv_nsec != sb->st_mtim.tv_nsec) {
        char lv[64], rv[64];
        p3m_fmt_ts(&sa->st_mtim, lv, true);
        p3m_fmt_ts(&sb->st_mtim, rv, true);
        meta_note(ob, rel, "mtime", lv, rv);
    }
}

/* ------------------------------------------------------------------ */
/* per-entry comparison                                                 */
/* ------------------------------------------------------------------ */

static char type_char(mode_t m)
{
    if (S_ISREG(m))  return 'f';
    if (S_ISDIR(m))  return 'd';
    if (S_ISLNK(m))  return 'l';
    if (S_ISBLK(m))  return 'b';
    if (S_ISCHR(m))  return 'c';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    return '?';
}

static void count_entry(mode_t m)
{
    if (S_ISDIR(m))
        atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);
}

/* entry present on one side only; dfd/name identify it for typing */
static void only_one(int dfd, const char *name, unsigned char dt,
                     const char *rel, const char *parent, bool left,
                     p3m_outbuf *ob)
{
    char tc = p3m_dt_char(dt);
    mode_t m = dt == DT_DIR ? S_IFDIR : S_IFREG;
    if (dt == DT_UNKNOWN) {
        struct stat st;
        if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            tc = type_char(st.st_mode);
            m  = st.st_mode;
        } else {
            tc = '?';
        }
    } else if (dt == DT_DIR) {
        tc = 'd';
    }
    (void)parent;
    count_entry(m);
    char tv[2] = { tc, '\0' };
    if (left) {
        atomic_fetch_add_explicit(&n_onlyl, 1, memory_order_relaxed);
        emit_row(ob, rel, "only-left", tv, "");
    } else {
        atomic_fetch_add_explicit(&n_onlyr, 1, memory_order_relaxed);
        emit_row(ob, rel, "only-right", tv, "");
    }
}

/* entry present on both sides */
static void compare_common(int dfa, int dfb, const char *name,
                           const char *rel, p3m_outbuf *ob, bool *descend)
{
    *descend = false;

    struct stat sa, sb;
    if (fstatat(dfa, name, &sa, AT_SYMLINK_NOFOLLOW) != 0) {
        p3m_note_error(rel, "stat (left)", errno);
        return;
    }
    if (fstatat(dfb, name, &sb, AT_SYMLINK_NOFOLLOW) != 0) {
        p3m_note_error(rel, "stat (right)", errno);
        return;
    }
    count_entry(sa.st_mode);

    if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
        char lv[2] = { type_char(sa.st_mode), '\0' };
        char rv[2] = { type_char(sb.st_mode), '\0' };
        atomic_fetch_add_explicit(&n_type, 1, memory_order_relaxed);
        emit_row(ob, rel, "type", lv, rv);
        return;                   /* nothing further is comparable */
    }

    if (S_ISLNK(sa.st_mode)) {
        /* links: compare target, owner and group; mode is meaningless
         * on Linux and mtimes are rarely preserved — skipped */
        char ta[PATH_MAX], tb[PATH_MAX];
        ssize_t la = readlinkat(dfa, name, ta, sizeof ta - 1);
        ssize_t lb = readlinkat(dfb, name, tb, sizeof tb - 1);
        if (la < 0 || lb < 0) {
            p3m_note_error(rel, "readlink", errno);
            return;
        }
        ta[la] = tb[lb] = '\0';
        if (strcmp(ta, tb) != 0) {
            atomic_fetch_add_explicit(&n_meta, 1, memory_order_relaxed);
            emit_row(ob, rel, "target", ta, tb);
        }
        if (sa.st_uid != sb.st_uid) {
            char fa[32], fb[32];
            meta_note(ob, rel, "owner", p3m_uid_name(sa.st_uid, fa),
                      p3m_uid_name(sb.st_uid, fb));
        }
        if (sa.st_gid != sb.st_gid) {
            char fa[32], fb[32];
            meta_note(ob, rel, "group", p3m_gid_name(sa.st_gid, fa),
                      p3m_gid_name(sb.st_gid, fb));
        }
        return;
    }

    compare_meta(rel, &sa, &sb, ob);

    if (S_ISREG(sa.st_mode)) {
        if (sa.st_size != sb.st_size) {
            /* size differs: the files differ — the -c content read is
             * skipped, there is nothing left to prove */
            char lv[32], rv[32];
            snprintf(lv, sizeof lv, "%" PRIu64, (uint64_t)sa.st_size);
            snprintf(rv, sizeof rv, "%" PRIu64, (uint64_t)sb.st_size);
            atomic_fetch_add_explicit(&n_size, 1, memory_order_relaxed);
            emit_row(ob, rel, "size", lv, rv);
        } else if (g.checksum && sa.st_size > 0) {
            compare_content(dfa, dfb, name, rel, ob);
        }
        return;
    }

    if (S_ISBLK(sa.st_mode) || S_ISCHR(sa.st_mode)) {
        if (sa.st_rdev != sb.st_rdev) {
            char lv[32], rv[32];
            snprintf(lv, sizeof lv, "%u,%u",
                     major(sa.st_rdev), minor(sa.st_rdev));
            snprintf(rv, sizeof rv, "%u,%u",
                     major(sb.st_rdev), minor(sb.st_rdev));
            atomic_fetch_add_explicit(&n_meta, 1, memory_order_relaxed);
            emit_row(ob, rel, "device", lv, rv);
        }
        return;
    }

    if (S_ISDIR(sa.st_mode))
        *descend = true;
}

/* ------------------------------------------------------------------ */
/* directory pair comparison                                            */
/* ------------------------------------------------------------------ */

struct ent {
    char          *name;
    unsigned char  dt;
};

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const struct ent *)a)->name,
                  ((const struct ent *)b)->name);
}

/* read all entries of `dir`; returns the open DIR* (caller closes) or
 * NULL with the error already noted */
static DIR *read_entries(const char *dir, const char *side,
                         struct ent **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    DIR *d = opendir(dir);
    if (!d) {
        char what[32];
        snprintf(what, sizeof what, "opendir (%s)", side);
        p3m_note_error(dir, what, errno);
        return NULL;
    }
    struct ent *v = NULL;
    size_t nv = 0, cv = 0;
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' &&
            (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) {
            errno = 0;
            continue;
        }
        if (nv == cv) {
            size_t nc = cv ? cv * 2 : 64;
            struct ent *nvp = realloc(v, nc * sizeof *nvp);
            if (!nvp) {
                p3m_note_error(dir, "malloc", ENOMEM);
                errno = 0;
                continue;
            }
            v = nvp;
            cv = nc;
        }
        v[nv].name = strdup(nm);
        if (!v[nv].name) {
            p3m_note_error(dir, "malloc", ENOMEM);
            errno = 0;
            continue;
        }
        v[nv].dt = de->d_type;
        nv++;
        errno = 0;
    }
    if (errno)
        p3m_note_error(dir, "readdir", errno);
    qsort(v, nv, sizeof *v, ent_cmp);
    *out = v;
    *n = nv;
    return d;
}

static void free_entries(struct ent *v, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(v[i].name);
    free(v);
}

static void comp_dir(const char *rel, p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                   /* abort: let the queue drain */

    char *ap = root_join(g.left, g.llen, rel);
    char *bp = root_join(g.right, g.rlen, rel);
    if (!ap || !bp) {
        p3m_note_error(rel, "malloc", ENOMEM);
        goto out;
    }
    p3m_set_current(ap);

    struct ent *ea, *eb;
    size_t na, nb;
    DIR *da = read_entries(ap, "left", &ea, &na);
    DIR *db = read_entries(bp, "right", &eb, &nb);
    if (!da || !db) {             /* one side unreadable: not comparable */
        if (da) { free_entries(ea, na); closedir(da); }
        if (db) { free_entries(eb, nb); closedir(db); }
        goto out;
    }
    int dfa = dirfd(da), dfb = dirfd(db);

    char **subs = NULL;
    size_t nsub = 0, csub = 0;

    size_t i = 0, j = 0;
    while (i < na || j < nb) {
        int cmp = i >= na ? 1 : j >= nb ? -1
                : strcmp(ea[i].name, eb[j].name);
        char *crel = rel_join(rel, cmp <= 0 ? ea[i].name : eb[j].name);
        if (!crel) {
            p3m_note_error(rel, "malloc", ENOMEM);
            if (cmp <= 0) i++;
            if (cmp >= 0) j++;
            continue;
        }
        if (cmp < 0) {
            only_one(dfa, ea[i].name, ea[i].dt, crel, ap, true, ob);
            i++;
        } else if (cmp > 0) {
            only_one(dfb, eb[j].name, eb[j].dt, crel, bp, false, ob);
            j++;
        } else {
            bool descend = false;
            compare_common(dfa, dfb, ea[i].name, crel, ob, &descend);
            if (descend) {
                if (nsub == csub) {
                    size_t nc = csub ? csub * 2 : 32;
                    char **ns = realloc(subs, nc * sizeof *ns);
                    if (!ns) {
                        p3m_note_error(crel, "queue", ENOMEM);
                        free(crel);
                        i++; j++;
                        continue;
                    }
                    subs = ns;
                    csub = nc;
                }
                subs[nsub++] = crel;
                crel = NULL;      /* ownership moved to the queue */
            }
            i++; j++;
        }
        free(crel);
    }

    free_entries(ea, na);
    free_entries(eb, nb);
    closedir(da);
    closedir(db);
    p3m_stack_push_batch(&stk, subs, nsub);
    free(subs);
out:
    free(ap);
    free(bp);
}

static void *worker(void *arg)
{
    (void)arg;
    p3m_outbuf ob;
    if (p3m_ob_init(&ob, &sink) != 0) {
        p3m_note_error("worker", "malloc", ENOMEM);
        char *rel;
        while ((rel = p3m_stack_pop(&stk)) != NULL)
            free(rel);
        return NULL;
    }
    char *rel;
    while ((rel = p3m_stack_pop(&stk)) != NULL) {
        comp_dir(rel, &ob);
        free(rel);
    }
    p3m_ob_flush(&ob);
    p3m_ob_free(&ob);
    free(cmp_bufa);
    free(cmp_bufb);
    return NULL;
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
    uint64_t diffs = atomic_load_explicit(&n_diffs, memory_order_relaxed);
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

    double el_s = p3m_mono_now() - t_start;

    char fv[32], dv[32], xv[32], ev[32], rv[32], sv[32], tv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(diffs, xv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_size(el_s > 0 ? (uint64_t)((double)bytes / el_s) : 0, tv);
    p3m_fmt_elapsed(el_s, el);

    char botstr[80];
    if (g.checksum)
        snprintf(botstr, sizeof botstr, "%s · %s/s", sv, tv);
    else
        snprintf(botstr, sizeof botstr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-comp%s %s— parallel compare%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "none (-q)");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "check", C_RESET,
        g.checksum ? "names+meta+size+content" : "names+meta+size");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %s%-14s%s %s%-8s%s %s%s%s\n",
        C_DIM, "diffs", C_RESET,
        diffs ? C_BOLD : "", xv, diffs ? C_RESET : "",
        C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %-22s %s%-8s%s %s\n",
        C_DIM, g.checksum ? "checked" : "rate", C_RESET, botstr,
        C_DIM, "elapsed", C_RESET, el);
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
    uint64_t diffs = atomic_load(&n_diffs);
    uint64_t errs  = atomic_load(&p3m_nerrors);
    uint64_t bytes = atomic_load(&n_bytes);

    char fv[32], dv[32], xv[32], ev[32], rv[32], sv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(diffs, xv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_elapsed(elapsed, el);

    bool same = diffs == 0;
    fprintf(stderr,
            "%s%s%s %sp3m-comp%s complete — %s files · %s dirs · "
            "%s%s difference%s%s · %s%s error%s%s\n",
            same ? C_GREEN : C_RED, same ? "✓" : "✗", C_RESET,
            C_BOLD, C_RESET, fv, dv,
            diffs ? C_BOLD : "", xv, diffs == 1 ? "" : "s",
            diffs ? C_RESET : "",
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");

    if (diffs) {
        uint64_t ol = atomic_load(&n_onlyl), or_ = atomic_load(&n_onlyr);
        uint64_t ty = atomic_load(&n_type),  sz = atomic_load(&n_size);
        uint64_t co = atomic_load(&n_content), me = atomic_load(&n_meta);
        char b[6][32];
        fprintf(stderr, "  %s only in left · %s only in right · %s type · "
                "%s size · %s content · %s metadata\n",
                p3m_fmt_u64(ol, b[0]), p3m_fmt_u64(or_, b[1]),
                p3m_fmt_u64(ty, b[2]), p3m_fmt_u64(sz, b[3]),
                p3m_fmt_u64(co, b[4]), p3m_fmt_u64(me, b[5]));
    }

    fprintf(stderr, "  %s (%s items/s", el, rv);
    if (g.checksum)
        fprintf(stderr, " · %s contents compared", sv);
    fputc(')', stderr);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);

    /* the verdict */
    if (errs)
        fprintf(stderr, "  %sverdict withheld — %s error%s during "
                "comparison, coverage is incomplete%s\n",
                C_RED, ev, errs == 1 ? "" : "s", C_RESET);
    else if (diffs)
        fprintf(stderr, "  trees %sdiffer%s — %s difference%s listed above\n",
                C_BOLD, C_RESET, xv, diffs == 1 ? " is" : "s are");
    else if (g.checksum)
        fprintf(stderr, "  trees are %sidentical%s — names, metadata, "
                "sizes and file contents all match (contents verified "
                "byte-for-byte)\n", C_BOLD, C_RESET);
    else
        fprintf(stderr, "  trees are %svery likely identical%s — names, "
                "types, sizes and metadata all match %s(add -c to verify "
                "file contents)%s\n", C_BOLD, C_RESET, C_DIM, C_RESET);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-comp [OPTIONS] LEFT RIGHT\n"
"\n"
"Compare two directory trees in parallel and report the likelihood\n"
"that their contents are the same. By default entry names, types,\n"
"sizes and metadata (mode, owner, group, mtime) are compared; every\n"
"difference is listed as CSV and summarised at the end.\n"
"\n"
"Options:\n"
"  -c, --checksum      also compare file contents, byte for byte; a\n"
"                      file's size is checked first and the content\n"
"                      read is skipped when the sizes already differ\n"
"  -j, --threads N     worker threads (default: number of CPUs)\n"
"  -o, --output FILE   write the CSV listing to FILE; a live progress\n"
"                      display is shown\n"
"  -q, --quiet         suppress the console listing (progress and the\n"
"                      summary verdict are still shown)\n"
"  -h, --help          show this help\n"
"  -V, --version       show version\n"
"\n"
"CSV columns: path,difference,left,right — path is relative to the\n"
"roots; difference is one of only-left, only-right, type, size,\n"
"content, mode, owner, group, mtime, target, device.\n"
"\n"
"Symbolic links are never followed: they are compared by target,\n"
"owner and group, and never descended into.\n"
"\n"
"Exit status: 0 no differences · 1 differences found · 2 usage error\n"
"or comparison errors (verdict withheld).\n",
        to);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "checksum", no_argument,       NULL, 'c' },
        { "threads",  required_argument, NULL, 'j' },
        { "output",   required_argument, NULL, 'o' },
        { "quiet",    no_argument,       NULL, 'q' },
        { "help",     no_argument,       NULL, 'h' },
        { "version",  no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "cj:o:qhV", lopts, NULL)) != -1) {
        switch (c) {
        case 'c':
            g.checksum = true;
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-comp: invalid thread count '%s'\n",
                        optarg);
                return 2;
            }
            g.nthreads = (int)v;
            break;
        }
        case 'o':
            g.outpath = optarg;
            break;
        case 'q':
            g.quiet = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            printf("p3m-comp %s\n", P3M_COMP_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (argc - optind != 2) {
        fprintf(stderr, "p3m-comp: expected exactly two directories, "
                "LEFT and RIGHT\n");
        usage(stderr);
        return 2;
    }

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);
    g.suppress = g.quiet && !g.outpath;
    g.progress = (g.outpath || g.quiet) && isatty(STDERR_FILENO);

    /* the two roots */
    g.left  = strdup(argv[optind]);
    g.right = strdup(argv[optind + 1]);
    if (!g.left || !g.right) {
        fprintf(stderr, "p3m-comp: out of memory\n");
        return 2;
    }
    for (int k = 0; k < 2; k++) {
        char *p = k ? g.right : g.left;
        size_t len = strlen(p);
        while (len > 1 && p[len - 1] == '/')
            p[--len] = '\0';
    }
    g.llen = strlen(g.left);
    g.rlen = strlen(g.right);

    struct stat sl, sr;
    if (lstat(g.left, &sl) != 0) {
        fprintf(stderr, "p3m-comp: %s: %s\n", g.left, strerror(errno));
        return 2;
    }
    if (lstat(g.right, &sr) != 0) {
        fprintf(stderr, "p3m-comp: %s: %s\n", g.right, strerror(errno));
        return 2;
    }
    if (!S_ISDIR(sl.st_mode) || !S_ISDIR(sr.st_mode)) {
        fprintf(stderr, "p3m-comp: both arguments must be directories\n");
        return 2;
    }
    if (sl.st_dev == sr.st_dev && sl.st_ino == sr.st_ino) {
        fprintf(stderr, "p3m-comp: LEFT and RIGHT are the same "
                "directory\n");
        return 2;
    }

    /* output stream */
    FILE *out = stdout;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-comp: cannot open %s: %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    }
    static char outvbuf[1 << 20];
    setvbuf(out, outvbuf, _IOFBF, sizeof outvbuf);
    p3m_sink_init(&sink, out);

    if (!g.suppress)
        fputs("path,difference,left,right\n", out);

    if (g.progress)
        p3m_set_current("…");
    t_start = p3m_mono_now();
    if (g.progress) {
        p3m_progress_cfg cfg = {
            .draw = prog_draw, .items = prog_items, .lines = PROG_LINES
        };
        if (p3m_progress_start(&cfg) != 0)
            g.progress = false;
    }

    /* the roots themselves count as one compared directory pair */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-comp: out of memory\n");
        return 2;
    }
    atomic_fetch_add(&n_dirs, 1);
    compare_meta(".", &sl, &sr, &rootob);
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    char *rootrel = strdup("");
    if (!rootrel) {
        fprintf(stderr, "p3m-comp: out of memory\n");
        return 2;
    }
    p3m_stack_push_batch(&stk, &rootrel, 1);

    /* launch */
    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-comp: out of memory\n");
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
            fprintf(stderr, "p3m-comp: could not create any worker "
                    "threads\n");
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
        fprintf(stderr, "p3m-comp: %swrite error%s on %s — output is "
                "incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 2;
    }

    print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);
    free(g.left);
    free(g.right);

    if (atomic_load(&p3m_nerrors))
        return 2;
    return atomic_load(&n_diffs) ? 1 : 0;
}
