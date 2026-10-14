/*
 * p3m-sync — parallel one-way directory synchronisation
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Makes DEST an exact mirror of SOURCE, walking both trees in lockstep with
 * a pool of worker threads. Dry run unless --apply. Entries that differ are
 * created or updated (file data is written to a temporary name in the
 * destination directory and renamed into place, so a destination file is
 * never observed half-written); entries present only at the destination are
 * left alone unless --delete is given. Symbolic links are never followed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    bool         apply;
    bool         del;         /* --delete: remove destination-only entries */
    bool         checksum;    /* -c: compare content of same-size files    */
    bool         owner;       /* --owner: sync uid/gid (when permitted)    */
    long         window;      /* --modify-window: mtime tolerance, seconds */
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
} g;

static _Atomic uint64_t n_entries;  /* source entries examined            */
static _Atomic uint64_t n_create;
static _Atomic uint64_t n_update;
static _Atomic uint64_t n_meta;
static _Atomic uint64_t n_delete;
static _Atomic uint64_t n_same;
static _Atomic uint64_t n_skip;
static _Atomic uint64_t n_bytes;    /* bytes copied (apply) / to copy     */
static _Atomic bool     fixing;
static _Atomic uint64_t n_fixed;
static _Atomic uint64_t n_fix_total;
static _Atomic uint64_t tmp_seq;

static size_t    rootlen;           /* length of the resolved source root */
static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

/* source path -> path relative to the source root ("." for the root) */
static const char *rel(const char *sp)
{
    const char *r = sp + rootlen;
    if (*r == '/')
        r++;
    return *r ? r : ".";
}

/* relative path of NAME inside the directory whose source path is DSP */
static char *rel_join(const char *dsp, const char *name)
{
    const char *r = rel(dsp);
    if (!strcmp(r, "."))
        return strdup(name);
    return p3m_path_join(r, strlen(r), name);
}

static char *pack_item(const char *src, const char *dst, bool dest_exists)
{
    size_t sl = strlen(src), dl = strlen(dst);
    char *it = malloc(sl + 1 + dl + 1 + 1);
    if (!it)
        return NULL;
    memcpy(it, src, sl + 1);
    memcpy(it + sl + 1, dst, dl + 1);
    it[sl + 1 + dl + 1] = dest_exists ? 'E' : 'A';
    return it;
}

static void emit_row(p3m_outbuf *ob, const char *relp, char type,
                     uint64_t size, const char *action, const char *result)
{
    if (g.suppress)
        return;
    if (!p3m_ob_room(ob, 2 * strlen(relp) + 160)) {
        p3m_note_error(relp, "emit", ENAMETOOLONG);
        return;
    }
    p3m_ob_csv(ob, relp);
    p3m_ob_fmt(ob, ",%c,%" PRIu64 ",%s,", type, size, action);
    p3m_ob_csv(ob, result);
    p3m_ob_putc(ob, '\n');
}

static const char *done_str(void) { return g.apply ? "done" : "pending"; }

static char type_of(mode_t m) { return p3m_dt_char(IFTODT(m)); }

static bool mtime_equal(const struct timespec *a, const struct timespec *b)
{
    if (g.window > 0) {
        long d = (long)(a->tv_sec - b->tv_sec);
        return d >= -g.window && d <= g.window;
    }
    return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

static bool meta_differs(const struct stat *s, const struct stat *d)
{
    if ((s->st_mode & 07777) != (d->st_mode & 07777))
        return true;
    return g.owner && (s->st_uid != d->st_uid || s->st_gid != d->st_gid);
}

static void mk_tmpname(char out[64])
{
    snprintf(out, 64, ".p3m-sync.%ld.%" PRIu64, (long)getpid(),
             atomic_fetch_add(&tmp_seq, 1));
}

/* ------------------------------------------------------------------ */
/* directory fix-up list (final mode / owner / mtime, deepest first)    */
/* ------------------------------------------------------------------ */

struct dirfix {
    char           *path;
    mode_t          mode;
    uid_t           uid;
    gid_t           gid;
    struct timespec ts[2];
    int             depth;
};

static struct dirfix   *dfx;
static size_t           n_dfx, c_dfx;
static pthread_mutex_t  dfx_mu = PTHREAD_MUTEX_INITIALIZER;

static void dirfix_add(const char *path, const struct stat *st)
{
    char *p = strdup(path);
    if (!p) {
        p3m_note_error(path, "malloc", ENOMEM);
        return;
    }
    int depth = 0;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            depth++;

    pthread_mutex_lock(&dfx_mu);
    if (n_dfx == c_dfx) {
        size_t nc = c_dfx ? c_dfx * 2 : 256;
        struct dirfix *nd = realloc(dfx, nc * sizeof *nd);
        if (!nd) {
            pthread_mutex_unlock(&dfx_mu);
            free(p);
            p3m_note_error(path, "malloc", ENOMEM);
            return;
        }
        dfx = nd;
        c_dfx = nc;
    }
    struct dirfix *f = &dfx[n_dfx++];
    f->path  = p;
    f->mode  = st->st_mode & 07777;
    f->uid   = st->st_uid;
    f->gid   = st->st_gid;
    f->ts[0] = st->st_atim;
    f->ts[1] = st->st_mtim;
    f->depth = depth;
    pthread_mutex_unlock(&dfx_mu);
}

static int dfx_cmp(const void *a, const void *b)
{
    return ((const struct dirfix *)b)->depth - ((const struct dirfix *)a)->depth;
}

static void dirfix_one(void *e)
{
    struct dirfix *f = e;
    struct stat cur;
    p3m_set_current(f->path);
    if (fstatat(AT_FDCWD, f->path, &cur, AT_SYMLINK_NOFOLLOW) != 0) {
        p3m_note_error(f->path, "stat", errno);
    } else {
        if (g.owner && (cur.st_uid != f->uid || cur.st_gid != f->gid) &&
            chown(f->path, f->uid, f->gid) != 0 &&
            errno != EPERM && errno != EINVAL)
            p3m_note_error(f->path, "chown", errno);
        if ((cur.st_mode & 07777) != f->mode &&
            chmod(f->path, f->mode) != 0)
            p3m_note_error(f->path, "chmod", errno);
        if ((cur.st_mtim.tv_sec != f->ts[1].tv_sec ||
             cur.st_mtim.tv_nsec != f->ts[1].tv_nsec) &&
            utimensat(AT_FDCWD, f->path, f->ts, 0) != 0)
            p3m_note_error(f->path, "utimens", errno);
    }
    atomic_fetch_add_explicit(&n_fixed, 1, memory_order_relaxed);
    free(f->path);
}

static int dfx_depth(const void *e)
{
    return ((const struct dirfix *)e)->depth;
}

/* deepest level first; directories of one depth are fixed concurrently,
 * which matters on NFS where every call is a network round trip */
static void dirfix_apply(void)
{
    atomic_store(&n_fix_total, n_dfx);
    atomic_store(&fixing, true);
    qsort(dfx, n_dfx, sizeof *dfx, dfx_cmp);
    p3m_for_each_level(dfx, n_dfx, sizeof *dfx, dfx_depth, dirfix_one,
                       g.nthreads);
    free(dfx);
}

/* ------------------------------------------------------------------ */
/* removal (--delete)                                                   */
/* ------------------------------------------------------------------ */

/* Remove NAME (inside dfd) and everything below it, children first. RP is
 * the relative path shown in the CSV, DP the full destination path. In a
 * dry run nothing is removed but the same rows are produced. */
static void remove_tree(int dfd, const char *name, const char *rp,
                        const char *dp, p3m_outbuf *ob)
{
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT)
            p3m_note_error(dp, "stat", errno);
        return;
    }
    bool isdir = S_ISDIR(st.st_mode);
    uint64_t size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;

    if (isdir) {
        int fd = openat(dfd, name,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
        if (!d) {
            int e = errno;
            if (fd >= 0)
                close(fd);
            p3m_note_error(dp, "open", e);
            emit_row(ob, rp, 'd', 0, "delete", "failed: open");
            return;
        }
        size_t rl = strlen(rp), dl = strlen(dp);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            if (nm[0] == '.' &&
                (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
            char *crp = p3m_path_join(rp, rl, nm);
            char *cdp = p3m_path_join(dp, dl, nm);
            if (crp && cdp)
                remove_tree(dirfd(d), nm, crp, cdp, ob);
            else
                p3m_note_error(dp, "malloc", ENOMEM);
            free(crp);
            free(cdp);
        }
        closedir(d);
    }

    const char *res = done_str();
    if (g.apply && unlinkat(dfd, name, isdir ? AT_REMOVEDIR : 0) != 0 &&
        errno != ENOENT) {
        int e = errno;
        p3m_note_error(dp, isdir ? "rmdir" : "unlink", e);
        emit_row(ob, rp, type_of(st.st_mode), size, "delete",
                 isdir ? "failed: rmdir" : "failed: unlink");
        return;
    }
    atomic_fetch_add_explicit(&n_delete, 1, memory_order_relaxed);
    emit_row(ob, rp, type_of(st.st_mode), size, "delete", res);
}

/* ------------------------------------------------------------------ */
/* content comparison (-c)                                              */
/* ------------------------------------------------------------------ */

static __thread char *cmpbuf[2];

static ssize_t read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* 1 identical, 0 differ, -1 error (errno set) */
static int content_equal(int sfd, int dfd, const char *name)
{
    const size_t chunk = 1 << 20;
    if (!cmpbuf[0] && !(cmpbuf[0] = malloc(chunk)))
        return errno = ENOMEM, -1;
    if (!cmpbuf[1] && !(cmpbuf[1] = malloc(chunk)))
        return errno = ENOMEM, -1;
    int a = openat(sfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (a < 0)
        return -1;
    int b = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (b < 0) {
        int e = errno;
        close(a);
        errno = e;
        return -1;
    }
    int rc = 1;
    for (;;) {
        ssize_t ra = read_full(a, cmpbuf[0], chunk);
        ssize_t rb = read_full(b, cmpbuf[1], chunk);
        if (ra < 0 || rb < 0) {
            rc = -1;
            break;
        }
        if (ra != rb || memcmp(cmpbuf[0], cmpbuf[1], (size_t)ra)) {
            rc = 0;
            break;
        }
        if (ra < (ssize_t)chunk)
            break;
    }
    int e = errno;
    close(a);
    close(b);
    errno = e;
    return rc;
}

/* ------------------------------------------------------------------ */
/* writing entries (atomic: temp name in the destination dir + rename)  */
/* ------------------------------------------------------------------ */

static void chown_if(const char *dp, int dfd, const char *n,
                     const struct stat *st)
{
    if (g.owner &&
        fchownat(dfd, n, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno != EPERM && errno != EINVAL)
        p3m_note_error(dp, "chown", errno);
}

/* NULL on success, else a short failure description for the CSV */
static const char *write_file(int sfd, int dfd, const char *name,
                              const char *sp, const char *dp,
                              const struct stat *st)
{
    int in = openat(sfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) {
        p3m_note_error(sp, "open", errno);
        return "failed: open source";
    }
    char tmp[64];
    mk_tmpname(tmp);
    int out = openat(dfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                     O_CLOEXEC, (st->st_mode & 07777) | 0200);
    if (out < 0) {
        int e = errno;
        close(in);
        p3m_note_error(dp, "create", e);
        return "failed: create";
    }
    const char *fail = NULL;
    int e = 0;
    if (p3m_copy_fd(in, out, &n_bytes) != 0) {
        e = errno;
        fail = "failed: copy";
    } else {
        if (g.owner && fchown(out, st->st_uid, st->st_gid) != 0 &&
            errno != EPERM && errno != EINVAL)
            p3m_note_error(dp, "chown", errno);
        if (fchmod(out, st->st_mode & 07777) != 0)
            p3m_note_error(dp, "chmod", errno);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        if (futimens(out, ts) != 0)
            p3m_note_error(dp, "utimens", errno);
        if (close(out) != 0) {
            e = errno;
            fail = "failed: write";
        }
        out = -1;
    }
    close(in);
    if (!fail && renameat(dfd, tmp, dfd, name) != 0) {
        e = errno;
        fail = "failed: rename";
    }
    if (fail) {
        if (out >= 0)
            close(out);
        unlinkat(dfd, tmp, 0);
        p3m_note_error(dp, "write", e);
    }
    return fail;
}

static const char *write_symlink(int sfd, int dfd, const char *name,
                                 const char *sp, const char *dp,
                                 const struct stat *st)
{
    char target[PATH_MAX + 1];
    ssize_t tl = readlinkat(sfd, name, target, sizeof target - 1);
    if (tl < 0) {
        p3m_note_error(sp, "readlink", errno);
        return "failed: readlink";
    }
    target[tl] = '\0';
    char tmp[64];
    mk_tmpname(tmp);
    if (symlinkat(target, dfd, tmp) != 0) {
        p3m_note_error(dp, "symlink", errno);
        return "failed: symlink";
    }
    chown_if(dp, dfd, tmp, st);
    if (renameat(dfd, tmp, dfd, name) != 0) {
        int e = errno;
        unlinkat(dfd, tmp, 0);
        p3m_note_error(dp, "rename", e);
        return "failed: rename";
    }
    return NULL;
}

static const char *write_special(int dfd, const char *name, const char *dp,
                                 const struct stat *st)
{
    char tmp[64];
    mk_tmpname(tmp);
    mode_t m = st->st_mode & (S_IFMT | 07777);
    int rc = S_ISFIFO(st->st_mode) ? mkfifoat(dfd, tmp, m & 07777)
                                   : mknodat(dfd, tmp, m, st->st_rdev);
    if (rc != 0) {
        p3m_note_error(dp, "mknod", errno);
        return "failed: mknod";
    }
    chown_if(dp, dfd, tmp, st);
    fchmodat(dfd, tmp, st->st_mode & 07777, 0);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    utimensat(dfd, tmp, ts, AT_SYMLINK_NOFOLLOW);
    if (renameat(dfd, tmp, dfd, name) != 0) {
        int e = errno;
        unlinkat(dfd, tmp, 0);
        p3m_note_error(dp, "rename", e);
        return "failed: rename";
    }
    return NULL;
}

static const char *write_meta(int dfd, const char *name, const char *dp,
                              const struct stat *st)
{
    chown_if(dp, dfd, name, st);
    if (!S_ISLNK(st->st_mode)) {
        if (fchmodat(dfd, name, st->st_mode & 07777, 0) != 0) {
            p3m_note_error(dp, "chmod", errno);
            return "failed: chmod";
        }
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        if (utimensat(dfd, name, ts, AT_SYMLINK_NOFOLLOW) != 0) {
            p3m_note_error(dp, "utimens", errno);
            return "failed: utimens";
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* per-entry decisions                                                  */
/* ------------------------------------------------------------------ */

enum act { ACT_NONE, ACT_CREATE, ACT_UPDATE, ACT_META };

/* dest entry DT (present) vs source entry ST, same file type */
static enum act decide(int sfd, int dfd, const char *name, const char *dp,
                       const struct stat *st, const struct stat *dt,
                       const char **err)
{
    *err = NULL;
    if (S_ISREG(st->st_mode)) {
        bool mteq = mtime_equal(&st->st_mtim, &dt->st_mtim);
        if (st->st_size != dt->st_size)
            return ACT_UPDATE;
        if (g.checksum) {
            int r = content_equal(sfd, dfd, name);
            if (r < 0) {
                p3m_note_error(dp, "compare", errno);
                *err = "failed: compare";
                return ACT_NONE;
            }
            if (!r)
                return ACT_UPDATE;
        } else if (!mteq) {
            return ACT_UPDATE;
        }
        return (!mteq || meta_differs(st, dt)) ? ACT_META : ACT_NONE;
    }
    if (S_ISLNK(st->st_mode)) {
        char a[PATH_MAX + 1], b[PATH_MAX + 1];
        ssize_t la = readlinkat(sfd, name, a, sizeof a - 1);
        ssize_t lb = readlinkat(dfd, name, b, sizeof b - 1);
        if (la < 0 || lb < 0) {
            p3m_note_error(dp, "readlink", errno);
            *err = "failed: readlink";
            return ACT_NONE;
        }
        if (la != lb || memcmp(a, b, (size_t)la))
            return ACT_UPDATE;
        return (g.owner && (st->st_uid != dt->st_uid ||
                            st->st_gid != dt->st_gid)) ? ACT_META : ACT_NONE;
    }
    /* fifo / device node */
    if ((S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) &&
        st->st_rdev != dt->st_rdev)
        return ACT_UPDATE;
    return meta_differs(st, dt) ? ACT_META : ACT_NONE;
}

static void count_and_emit(p3m_outbuf *ob, const char *relp, char type,
                           uint64_t size, const char *action,
                           _Atomic uint64_t *counter, const char *res)
{
    if (!strcmp(res, done_str()))
        atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
    emit_row(ob, relp, type, size, action, res);
}

/*
 * Handle one non-directory source entry. dfd < 0 means the destination
 * directory does not exist (dry run under a directory that would be created).
 */
static void sync_entry(int sfd, int dfd, const char *name, const char *sp,
                       const char *dp, const struct stat *st, p3m_outbuf *ob)
{
    atomic_fetch_add_explicit(&n_entries, 1, memory_order_relaxed);
    const char *relp = rel(sp);
    char type = type_of(st->st_mode);
    uint64_t size = S_ISREG(st->st_mode) ? (uint64_t)st->st_size : 0;

    if (S_ISSOCK(st->st_mode)) {
        atomic_fetch_add_explicit(&n_skip, 1, memory_order_relaxed);
        emit_row(ob, relp, type, 0, "create", "skipped: socket");
        return;
    }

    struct stat dt;
    bool have = false;
    if (dfd >= 0) {
        if (fstatat(dfd, name, &dt, AT_SYMLINK_NOFOLLOW) == 0) {
            have = true;
        } else if (errno != ENOENT) {
            p3m_note_error(dp, "stat", errno);
            emit_row(ob, relp, type, size, "update", "failed: stat");
            return;
        }
    }

    enum act act = ACT_CREATE;
    if (have) {
        if ((st->st_mode & S_IFMT) != (dt.st_mode & S_IFMT)) {
            if (!g.del) {
                atomic_fetch_add_explicit(&n_skip, 1, memory_order_relaxed);
                emit_row(ob, relp, type, size, "update",
                         "skipped: type conflict (needs --delete)");
                return;
            }
            char *drp = strdup(relp);
            if (drp)
                remove_tree(dfd, name, drp, dp, ob);
            free(drp);
            have = false;
        } else {
            const char *err;
            act = decide(sfd, dfd, name, dp, st, &dt, &err);
            if (err) {
                emit_row(ob, relp, type, size, "update", err);
                return;
            }
            if (act == ACT_NONE) {
                atomic_fetch_add_explicit(&n_same, 1, memory_order_relaxed);
                return;
            }
        }
    }

    const char *action = act == ACT_CREATE ? "create"
                       : act == ACT_UPDATE ? "update" : "metadata";
    _Atomic uint64_t *counter = act == ACT_CREATE ? &n_create
                              : act == ACT_UPDATE ? &n_update : &n_meta;

    if (!g.apply) {
        if (act != ACT_META)
            atomic_fetch_add_explicit(&n_bytes, size, memory_order_relaxed);
        count_and_emit(ob, relp, type, size, action, counter, done_str());
        return;
    }

    const char *fail;
    if (act == ACT_META)
        fail = write_meta(dfd, name, dp, st);
    else if (S_ISREG(st->st_mode))
        fail = write_file(sfd, dfd, name, sp, dp, st);
    else if (S_ISLNK(st->st_mode))
        fail = write_symlink(sfd, dfd, name, sp, dp, st);
    else
        fail = write_special(dfd, name, dp, st);

    if (fail)
        emit_row(ob, relp, type, size, action, fail);
    else
        count_and_emit(ob, relp, type, size, action, counter, "done");
}

/*
 * Handle a source directory (or the root): create / adjust the destination
 * directory and report through *descend / *child_exists how to walk into it.
 * dfd/name locate the destination entry (AT_FDCWD for the root); dfd == -1:
 * the parent does not exist (dry run under a directory that would be created).
 */
static void sync_dir(int dfd, const char *name, const char *sp,
                     const char *dp, const struct stat *st, p3m_outbuf *ob,
                     bool *descend, bool *child_exists)
{
    *descend = true;
    *child_exists = false;
    atomic_fetch_add_explicit(&n_entries, 1, memory_order_relaxed);
    const char *relp = rel(sp);

    struct stat dt;
    bool have = false;
    if (dfd != -1) {                     /* -1: parent dir does not exist */
        if (fstatat(dfd, name, &dt, AT_SYMLINK_NOFOLLOW) == 0) {
            have = true;
        } else if (errno != ENOENT) {
            p3m_note_error(dp, "stat", errno);
            emit_row(ob, relp, 'd', 0, "update", "failed: stat");
            *descend = false;
            return;
        }
    }

    if (have && !S_ISDIR(dt.st_mode)) {
        if (!g.del) {
            atomic_fetch_add_explicit(&n_skip, 1, memory_order_relaxed);
            emit_row(ob, relp, 'd', 0, "create",
                     "skipped: type conflict (needs --delete)");
            *descend = false;
            return;
        }
        char *drp = strdup(relp);
        if (drp)
            remove_tree(dfd, name, drp, dp, ob);
        free(drp);
        have = false;
    }

    if (have) {
        *child_exists = true;
        if (meta_differs(st, &dt))
            count_and_emit(ob, relp, 'd', 0, "metadata", &n_meta, done_str());
        else
            atomic_fetch_add_explicit(&n_same, 1, memory_order_relaxed);
        if (g.apply)
            dirfix_add(dp, st);
        return;
    }

    if (!g.apply) {
        count_and_emit(ob, relp, 'd', 0, "create", &n_create, "pending");
        return;                          /* child_exists stays false */
    }
    /* owner rwx while populating; the final mode lands in the fix-up */
    if (mkdirat(dfd, name, (st->st_mode & 07777) | S_IRWXU) != 0) {
        p3m_note_error(dp, "mkdir", errno);
        emit_row(ob, relp, 'd', 0, "create", "failed: mkdir");
        *descend = false;
        return;
    }
    *child_exists = true;
    dirfix_add(dp, st);
    count_and_emit(ob, relp, 'd', 0, "create", &n_create, "done");
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_dir(const char *spath, const char *dpath, bool dest_exists,
                     p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;
    p3m_set_current(spath);

    int sfd0 = open(spath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *d = sfd0 >= 0 ? fdopendir(sfd0) : NULL;
    if (!d) {
        int e = errno;
        if (sfd0 >= 0)
            close(sfd0);
        p3m_note_error(spath, "open", e);
        return;
    }
    int sfd = dirfd(d);

    /* the full list of source names, sorted: drives iteration and lets the
     * --delete pass tell which destination names have no source */
    char **names = NULL;
    size_t nn = 0, cn = 0;
    bool complete = true;
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' &&
            (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) {
            errno = 0;
            continue;
        }
        if (nn == cn) {
            cn = cn ? cn * 2 : 64;
            char **t = realloc(names, cn * sizeof *t);
            if (!t) {
                complete = false;
                p3m_note_error(spath, "malloc", ENOMEM);
                break;
            }
            names = t;
        }
        char *dup = strdup(nm);
        if (!dup) {
            complete = false;
            p3m_note_error(spath, "malloc", ENOMEM);
            break;
        }
        names[nn++] = dup;
        errno = 0;
    }
    if (errno) {
        complete = false;
        p3m_note_error(spath, "readdir", errno);
    }
    qsort(names, nn, sizeof *names, cmp_names);

    int dfd = -1;
    if (dest_exists) {
        dfd = open(dpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) {
            p3m_note_error(dpath, "open", errno);
            goto out;                    /* nothing safe to do here */
        }
    }

    size_t slen = strlen(spath), dlen = strlen(dpath);

    /* --delete: destination names with no source counterpart. Skipped when
     * the source listing is incomplete, so a read error never deletes. */
    if (g.del && dfd >= 0 && complete) {
        DIR *dd = fdopendir(dup(dfd));
        if (dd) {
            struct dirent *e2;
            while ((e2 = readdir(dd)) != NULL) {
                const char *nm = e2->d_name;
                if (nm[0] == '.' &&
                    (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                    continue;
                if (bsearch(&nm, names, nn, sizeof *names, cmp_names))
                    continue;
                char *rp = rel_join(spath, nm);
                char *dp = p3m_path_join(dpath, dlen, nm);
                if (rp && dp)
                    remove_tree(dfd, nm, rp, dp, ob);
                else
                    p3m_note_error(dpath, "malloc", ENOMEM);
                free(rp);
                free(dp);
            }
            closedir(dd);
        }
    }

    char **subs = NULL;
    size_t nsub = 0, csub = 0;
    for (size_t i = 0; i < nn; i++) {
        const char *nm = names[i];
        struct stat st;
        char *sp = p3m_path_join(spath, slen, nm);
        char *dp = p3m_path_join(dpath, dlen, nm);
        if (!sp || !dp) {
            p3m_note_error(nm, "malloc", ENOMEM);
            free(sp);
            free(dp);
            continue;
        }
        if (fstatat(sfd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            p3m_note_error(sp, "stat", errno);
        } else if (S_ISDIR(st.st_mode)) {
            bool descend, child_exists;
            sync_dir(dfd, nm, sp, dp, &st, ob, &descend, &child_exists);
            if (descend) {
                char *it = pack_item(sp, dp, child_exists);
                if (it && nsub == csub) {
                    csub = csub ? csub * 2 : 32;
                    char **ns = realloc(subs, csub * sizeof *ns);
                    if (!ns) {
                        free(it);
                        it = NULL;
                    } else {
                        subs = ns;
                    }
                }
                if (it)
                    subs[nsub++] = it;
                else
                    p3m_note_error(sp, "queue", ENOMEM);
            }
        } else {
            sync_entry(sfd, dfd, nm, sp, dp, &st, ob);
        }
        free(sp);
        free(dp);
    }
    p3m_stack_push_batch(&stk, subs, nsub);
    free(subs);

out:
    if (dfd >= 0)
        close(dfd);
    for (size_t i = 0; i < nn; i++)
        free(names[i]);
    free(names);
    closedir(d);
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
    char *item;
    while ((item = p3m_stack_pop(&stk)) != NULL) {
        const char *src = item;
        const char *dst = item + strlen(item) + 1;
        bool dest_exists = dst[strlen(dst) + 1] == 'E';
        scan_dir(src, dst, dest_exists, &ob);
        free(item);
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
    return atomic_load_explicit(&n_entries, memory_order_relaxed) +
           atomic_load_explicit(&n_delete, memory_order_relaxed) +
           atomic_load_explicit(&n_fixed, memory_order_relaxed);
}

static void prog_draw(double rate, int frame)
{
    uint64_t ent  = atomic_load_explicit(&n_entries, memory_order_relaxed);
    uint64_t same = atomic_load_explicit(&n_same, memory_order_relaxed);
    uint64_t chg  = atomic_load_explicit(&n_create, memory_order_relaxed) +
                    atomic_load_explicit(&n_update, memory_order_relaxed) +
                    atomic_load_explicit(&n_meta, memory_order_relaxed);
    uint64_t del  = atomic_load_explicit(&n_delete, memory_order_relaxed);
    uint64_t errs = atomic_load_explicit(&p3m_nerrors, memory_order_relaxed);
    uint64_t bytes = atomic_load_explicit(&n_bytes, memory_order_relaxed);
    bool     fx   = atomic_load(&fixing);

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

    char ev[32], cv[32], dv[32], sv[32], rv[32], bv[32], tv[32], el[32];
    p3m_fmt_u64(ent, ev);
    p3m_fmt_u64(chg, cv);
    p3m_fmt_u64(del, dv);
    p3m_fmt_u64(same, sv);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_u64(errs, bv);
    p3m_fmt_size(bytes, tv);
    p3m_fmt_elapsed(el_s, el);

    char ratestr[80];
    snprintf(ratestr, sizeof ratestr, "%s items/s · %s", rv, tv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-sync%s %s— parallel sync%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "none (-q)");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s%s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "action", C_RESET,
        fx ? "finalizing dir metadata" : g.apply ? "apply" : "dry-run",
        g.del ? " +delete" : "");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "examined", C_RESET, ev, C_DIM, "changes", C_RESET, cv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "same", C_RESET, sv, C_DIM, "deletes", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s  %s\n",
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", bv, errs ? C_RESET : "", el);
#undef ADD
    fwrite(buf, 1, off, stderr);
}

/* ------------------------------------------------------------------ */
/* summary                                                              */
/* ------------------------------------------------------------------ */

static void print_summary(double elapsed, bool interrupted)
{
    uint64_t cr = atomic_load(&n_create), up = atomic_load(&n_update);
    uint64_t me = atomic_load(&n_meta), de = atomic_load(&n_delete);
    uint64_t sa = atomic_load(&n_same), sk = atomic_load(&n_skip);
    uint64_t errs = atomic_load(&p3m_nerrors);
    uint64_t bytes = atomic_load(&n_bytes);
    uint64_t total = cr + up + me + de + sa + sk;

    char a[32], b[32], c[32], d[32], e[32], f[32], sv[32], tv[32], rv[32], el[32];
    p3m_fmt_u64(cr, a);
    p3m_fmt_u64(up, b);
    p3m_fmt_u64(me, c);
    p3m_fmt_u64(de, d);
    p3m_fmt_u64(sa, e);
    p3m_fmt_u64(errs, f);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_size(elapsed > 0 ? (uint64_t)((double)bytes / elapsed) : 0, tv);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)total / elapsed) : 0, rv);
    p3m_fmt_elapsed(elapsed, el);

    const char *w = g.apply ? "" : "to ";
    fprintf(stderr,
            "%s%s%s %sp3m-sync%s %s — %s · %s%s create · %s%s update · "
            "%s%s metadata · %s%s delete · %s unchanged · "
            "%s%s error%s%s\n",
            interrupted ? C_RED : C_GREEN, interrupted ? "⚠" : "✓", C_RESET,
            C_BOLD, C_RESET, interrupted ? "interrupted" : "complete",
            g.apply ? "apply" : "dry-run",
            w, a, w, b, w, c, w, d, e,
            errs ? C_RED : "", f, errs == 1 ? "" : "s", errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s", el, rv);
    if (g.apply)
        fprintf(stderr, " · %s transferred · %s/s", sv, tv);
    else
        fprintf(stderr, " · %s to transfer", sv);
    fputc(')', stderr);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
    if (sk)
        fprintf(stderr, "  %s%" PRIu64 " entr%s skipped (type conflicts "
                "need --delete)%s\n", C_DIM, sk, sk == 1 ? "y" : "ies",
                C_RESET);
    if (!g.apply)
        fprintf(stderr, "  %sdry run — nothing was changed; "
                "add --apply to sync%s\n", C_DIM, C_RESET);
    if (!g.del)
        fprintf(stderr, "  %sentries only at the destination are left "
                "alone; add --delete to remove them%s\n", C_DIM, C_RESET);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-sync [OPTIONS] SOURCE DEST\n"
"\n"
"Make the directory DEST an exact mirror of the directory SOURCE, walking\n"
"both trees in parallel. DEST is created if it does not exist; SOURCE's\n"
"contents (not SOURCE itself) land directly in DEST. By default this is a\n"
"dry run listing everything that would change, as CSV; add --apply to sync.\n"
"\n"
"A file is updated when its size or modification time differs (mtimes are\n"
"preserved, so a synced tree compares equal on the next run). Mode, and\n"
"with --owner uid/gid, are kept in step. Updates are written to a temporary\n"
"name and renamed into place, so a destination file is never half-written.\n"
"\n"
"Options:\n"
"      --apply            actually sync (the default is a dry run)\n"
"      --delete           remove entries that exist only at DEST; without\n"
"                         it they are left alone. Also lets an entry whose\n"
"                         type differs (file vs directory) be replaced\n"
"  -c, --checksum         for same-size files compare contents byte for\n"
"                         byte instead of trusting the modification time\n"
"      --owner            also sync owner and group (when permitted)\n"
"      --modify-window S  treat mtimes within S seconds as equal (for file\n"
"                         systems with coarse timestamps; default 0)\n"
"  -j, --threads N        worker threads (default: number of online CPUs)\n"
"  -o, --output FILE      write CSV to FILE; a live progress display is shown\n"
"  -L, --lock-file F      exit 3 if lock file F exists; else create it for\n"
"                         the run and remove it on exit\n"
"  -q, --quiet            suppress the console listing (progress and the\n"
"                         summary are still shown; a -o file is still written)\n"
"  -h, --help             show this help and exit\n"
"  -V, --version          show version and exit\n"
"\n"
"CSV columns: path,type,size,action,result — path is relative to the roots;\n"
"action is create, update, metadata or delete.\n"
"\n"
"Symbolic links are never followed: they are recreated as links. SOURCE and\n"
"DEST may not contain one another, and DEST can never be the filesystem\n"
"root. With --delete an empty SOURCE is refused, so an unmounted source\n"
"cannot empty DEST.\n"
"\n"
"SIGINT/SIGTERM during --apply finish in-flight work before stopping (exit\n"
"130/143); a second signal forces an immediate exit.\n"
"Exit status: 0 success, 1 errors were encountered, 2 usage error.\n",
    to);
}

static int resolve_dest(const char *dest, char out[PATH_MAX])
{
    if (realpath(dest, out))
        return 0;
    if (errno != ENOENT)
        return -1;
    char tmp[PATH_MAX];
    if (strlen(dest) >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(tmp, dest);
    char *slash = strrchr(tmp, '/');
    const char *parent = ".", *base = tmp;
    if (slash) {
        base = slash + 1;
        if (slash == tmp)
            parent = "/";
        else
            *slash = '\0', parent = tmp;
    }
    char rp[PATH_MAX];
    if (!realpath(parent, rp))
        return -1;
    if (snprintf(out, PATH_MAX, "%s%s%s", rp,
                 strcmp(rp, "/") ? "/" : "", base) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* is b the same path as a, or below it? (both absolute and resolved) */
static bool inside(const char *a, const char *b)
{
    size_t al = strlen(a);
    if (!strcmp(a, "/"))
        return true;
    return !strncmp(a, b, al) && (b[al] == '\0' || b[al] == '/');
}

/* 1 holds at least one entry, 0 empty, -1 unreadable */
static int dir_nonempty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int r = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
            continue;
        r = 1;
        break;
    }
    closedir(d);
    return r;
}

int main(int argc, char **argv)
{
    const char *lockfile = NULL;
    static const struct option lopts[] = {
        { "apply",         no_argument,       NULL, 1000 },
        { "delete",        no_argument,       NULL, 1001 },
        { "owner",         no_argument,       NULL, 1002 },
        { "modify-window", required_argument, NULL, 1003 },
        { "checksum",      no_argument,       NULL, 'c' },
        { "threads",       required_argument, NULL, 'j' },
        { "output",        required_argument, NULL, 'o' },
        { "quiet",         no_argument,       NULL, 'q' },
        { "lock-file", required_argument, NULL, 'L' },
        { "help",          no_argument,       NULL, 'h' },
        { "version",       no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "cj:o:L:qhV", lopts, NULL)) != -1) {
        switch (c) {
        case 1000: g.apply = true; break;
        case 1001: g.del = true; break;
        case 1002: g.owner = true; break;
        case 1003: {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > 86400) {
                fprintf(stderr, "p3m-sync: invalid --modify-window '%s' "
                        "(expected 0-86400 seconds)\n", optarg);
                return 2;
            }
            g.window = v;
            break;
        }
        case 'c': g.checksum = true; break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-sync: invalid thread count '%s' "
                        "(expected 1-512)\n", optarg);
                return 2;
            }
            g.nthreads = (int)v;
            break;
        }
        case 'o': g.outpath = optarg; break;
        case 'L': lockfile = optarg; break;
        case 'q': g.quiet = true; break;
        case 'h': usage(stdout); return 0;
        case 'V': p3m_print_version("p3m-sync"); return 0;
        default:  usage(stderr); return 2;
        }
    }

    if (argc - optind != 2) {
        fprintf(stderr, "p3m-sync: expected SOURCE DEST\n");
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
    umask(0);                      /* modes are applied exactly from here */

    char rsrc[PATH_MAX], rdest[PATH_MAX];
    struct stat sst;
    if (!realpath(argv[optind], rsrc) || lstat(rsrc, &sst) != 0) {
        fprintf(stderr, "p3m-sync: cannot access source '%s': %s\n",
                argv[optind], strerror(errno));
        return 2;
    }
    if (!S_ISDIR(sst.st_mode)) {
        fprintf(stderr, "p3m-sync: source '%s' is not a directory\n",
                argv[optind]);
        return 2;
    }
    if (resolve_dest(argv[optind + 1], rdest) != 0) {
        fprintf(stderr, "p3m-sync: cannot resolve destination '%s': %s\n",
                argv[optind + 1], strerror(errno));
        return 2;
    }
    struct stat dst;
    bool dest_exists = lstat(rdest, &dst) == 0;
    if (dest_exists && !S_ISDIR(dst.st_mode)) {
        fprintf(stderr, "p3m-sync: destination '%s' exists and is not a "
                "directory\n", argv[optind + 1]);
        return 2;
    }

    /* guards, before any work or output starts */
    if (!strcmp(rdest, "/")) {
        fprintf(stderr, "p3m-sync: %sFATAL%s: the filesystem root can never "
                "be a destination\n", C_RED, C_RESET);
        return 2;
    }
    if (inside(rsrc, rdest) || inside(rdest, rsrc)) {
        fprintf(stderr, "p3m-sync: %sFATAL%s: source '%s' and destination "
                "'%s' overlap\n", C_RED, C_RESET, rsrc, rdest);
        return 2;
    }
    if (g.del && dest_exists && dir_nonempty(rsrc) == 0 &&
        dir_nonempty(rdest) != 0) {
        fprintf(stderr, "p3m-sync: %sFATAL%s: source '%s' is empty — refusing "
                "to --delete everything in '%s'\n", C_RED, C_RESET, rsrc,
                rdest);
        return 2;
    }
    rootlen = strlen(rsrc);

    if (lockfile) {
        int lk = p3m_lock_acquire("p3m-sync", lockfile);
        if (lk != 0)
            return lk == P3M_LOCK_HELD ? 3 : 2;
    }

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-sync: cannot open '%s': %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    } else {
        out = stdout;
    }
    static char outvbuf[1 << 20];
    setvbuf(out, outvbuf, _IOFBF, sizeof outvbuf);
    p3m_sink_init(&sink, out);

    if (!g.suppress)
        fputs("path,type,size,action,result\n", out);

    /* graceful stop: SIGINT/SIGTERM finish in-flight work rather than
     * killing threads mid-write */
    p3m_install_stop_handler();

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

    /* seed the queue with the root pair */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-sync: out of memory\n");
        return 2;
    }
    bool descend, child_exists;
    sync_dir(AT_FDCWD, rdest, rsrc, rdest, &sst, &rootob, &descend,
             &child_exists);
    if (descend) {
        char *it = pack_item(rsrc, rdest, child_exists);
        if (it)
            p3m_stack_push_batch(&stk, &it, 1);
        else
            p3m_note_error(rsrc, "queue", ENOMEM);
    }
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-sync: out of memory\n");
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
            fprintf(stderr, "p3m-sync: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    if (g.apply)
        dirfix_apply();

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-sync: %swrite error%s on %s — output is "
                "incomplete\n", C_RED, C_RESET,
                g.outpath ? g.outpath : "stdout");
        return 1;
    }

    int stopsig = p3m_stop_signal();
    print_summary(elapsed, stopsig != 0);
    if (stopsig)
        fprintf(stderr, "  %sremaining work was abandoned%s\n", C_DIM, C_RESET);
    p3m_print_errors();
    p3m_stack_destroy(&stk);

    if (stopsig)
        return 128 + stopsig;
    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
