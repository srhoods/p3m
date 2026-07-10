/*
 * p3m-mv — parallel move
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Moves files and directory trees. On the same filesystem a subtree
 * moves with a single rename(2) — instant, regardless of size. Across
 * filesystems (where mv is single-threaded copy+delete) the tree is
 * copied and the source removed by a pool of worker threads. Unlike
 * mv, an existing destination directory is merged: entries move into
 * it individually and emptied source directories are removed.
 *
 * Suite safety rules: dry run by default (--apply to move), existing
 * destination entries are skipped unless --overwrite (a skipped entry
 * always stays in the source), the filesystem root can never be a
 * source, and symbolic links are never followed.
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

#define P3M_MV_VERSION "1.0.0"

/*
 * Walk context, carried in each work item:
 *   CTX_RENAME  same-parent-fs unknown: try rename first, check dest
 *   CTX_XCOPY   cross-device, merging into an existing dest dir: copy,
 *               check dest per entry
 *   CTX_COPY    cross-device into a freshly created dest dir: copy,
 *               no existence checks needed
 */
enum { CTX_RENAME = 'E', CTX_XCOPY = 'X', CTX_COPY = 'A' };

static struct {
    bool         apply;
    bool         overwrite;
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
} g;

static _Atomic uint64_t n_files;    /* non-directory entries processed  */
static _Atomic uint64_t n_dirs;     /* directories processed            */
static _Atomic uint64_t n_renames;  /* successful rename(2) moves       */
static _Atomic uint64_t n_bytes;    /* bytes copied (cross-device)      */
static _Atomic uint64_t n_skipped;  /* existing destinations untouched  */
static _Atomic uint64_t n_left;     /* source dirs left (skips inside)  */

static p3m_stack stk;
static p3m_sink  sink;

static char *pack_item(const char *src, const char *dst, char ctx)
{
    size_t sl = strlen(src), dl = strlen(dst);
    char *it = malloc(sl + 1 + dl + 1 + 1);
    if (!it)
        return NULL;
    memcpy(it, src, sl + 1);
    memcpy(it + sl + 1, dst, dl + 1);
    it[sl + 1 + dl + 1] = ctx;
    return it;
}

/* ------------------------------------------------------------------ */
/* source directories awaiting removal, and created destination        */
/* directories awaiting their final metadata — both applied after the  */
/* walk, deepest first                                                  */
/* ------------------------------------------------------------------ */

static int path_depth(const char *p)
{
    int d = 0;
    for (; *p; p++)
        if (*p == '/')
            d++;
    return d;
}

struct srcdir { char *path; int depth; };

static struct srcdir   *rml;
static size_t           n_rml, c_rml;
static pthread_mutex_t  rml_mu = PTHREAD_MUTEX_INITIALIZER;

static void rml_add(const char *path)
{
    char *p = strdup(path);
    if (!p) {
        p3m_note_error(path, "malloc", ENOMEM);
        return;
    }
    pthread_mutex_lock(&rml_mu);
    if (n_rml == c_rml) {
        size_t nc = c_rml ? c_rml * 2 : 256;
        struct srcdir *nr = realloc(rml, nc * sizeof *nr);
        if (!nr) {
            pthread_mutex_unlock(&rml_mu);
            free(p);
            p3m_note_error(path, "malloc", ENOMEM);
            return;
        }
        rml = nr;
        c_rml = nc;
    }
    rml[n_rml].path  = p;
    rml[n_rml].depth = path_depth(p);
    n_rml++;
    pthread_mutex_unlock(&rml_mu);
}

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
    f->depth = path_depth(p);
    pthread_mutex_unlock(&dfx_mu);
}

static int depth_desc_dfx(const void *a, const void *b)
{
    return ((const struct dirfix *)b)->depth -
           ((const struct dirfix *)a)->depth;
}

static int depth_desc_rml(const void *a, const void *b)
{
    return ((const struct srcdir *)b)->depth -
           ((const struct srcdir *)a)->depth;
}

static void dirfix_apply(void)
{
    qsort(dfx, n_dfx, sizeof *dfx, depth_desc_dfx);
    for (size_t i = 0; i < n_dfx; i++) {
        struct dirfix *f = &dfx[i];
        if (chown(f->path, f->uid, f->gid) != 0 &&
            errno != EPERM && errno != EINVAL)
            p3m_note_error(f->path, "chown", errno);
        if (chmod(f->path, f->mode) != 0)
            p3m_note_error(f->path, "chmod", errno);
        if (utimensat(AT_FDCWD, f->path, f->ts, 0) != 0)
            p3m_note_error(f->path, "utimens", errno);
        free(f->path);
    }
    free(dfx);
}

/* remove now-empty source directories, deepest first; a directory
 * still holding skipped or failed entries is deliberately left */
static void remove_srcdirs(void)
{
    qsort(rml, n_rml, sizeof *rml, depth_desc_rml);
    bool had_leftovers = atomic_load(&n_skipped) > 0 ||
                         atomic_load(&p3m_nerrors) > 0;
    for (size_t i = 0; i < n_rml; i++) {
        if (rmdir(rml[i].path) != 0 && errno != ENOENT) {
            if (errno == ENOTEMPTY && had_leftovers)
                atomic_fetch_add(&n_left, 1);
            else
                p3m_note_error(rml[i].path, "rmdir", errno);
        }
        free(rml[i].path);
    }
    free(rml);
}

/* ------------------------------------------------------------------ */
/* CSV row emission                                                     */
/* ------------------------------------------------------------------ */

static void emit_row(p3m_outbuf *ob, const char *src, char type,
                     uint64_t size, const char *dst, const char *result)
{
    if (g.suppress)
        return;
    if (!p3m_ob_room(ob, 2 * (strlen(src) + strlen(dst)) + 128)) {
        p3m_note_error(src, "emit", ENAMETOOLONG);
        return;
    }
    p3m_ob_csv(ob, src);
    p3m_ob_fmt(ob, ",%c,%" PRIu64 ",", type, size);
    p3m_ob_csv(ob, dst);
    p3m_ob_putc(ob, ',');
    p3m_ob_csv(ob, result);
    p3m_ob_putc(ob, '\n');
}

/* ------------------------------------------------------------------ */
/* rename with NOREPLACE semantics                                      */
/* ------------------------------------------------------------------ */

static int rename_noreplace(const char *src, const char *dst)
{
    int rc = renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE);
    if (rc != 0 && (errno == EINVAL || errno == ENOSYS)) {
        /* filesystem without RENAME_NOREPLACE: check-then-rename */
        struct stat st;
        if (fstatat(AT_FDCWD, dst, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            errno = EEXIST;
            return -1;
        }
        return rename(src, dst);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* cross-device copy of one non-directory entry, then unlink source    */
/* ------------------------------------------------------------------ */

static void preserve_fd(int fd, const char *dpath, const struct stat *st)
{
    if (fchown(fd, st->st_uid, st->st_gid) != 0) {
        if (errno != EPERM && errno != EINVAL)
            p3m_note_error(dpath, "chown", errno);
    }
    if (fchmod(fd, st->st_mode & 07777) != 0)
        p3m_note_error(dpath, "chmod", errno);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    if (futimens(fd, ts) != 0)
        p3m_note_error(dpath, "utimens", errno);
}

static void copy_move_entry(int sdirfd, const char *name, const char *sp,
                            const char *dp, const struct stat *st,
                            char type, uint64_t size, p3m_outbuf *ob)
{
    /* ---- regular file -------------------------------------------- */
    if (S_ISREG(st->st_mode)) {
        int in = openat(sdirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (in < 0) {
            p3m_note_error(sp, "open", errno);
            emit_row(ob, sp, type, size, dp, "failed: open source");
            return;
        }
        int out = open(dp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       (st->st_mode & 07777) | 0200);
        if (out < 0 && errno == EEXIST) {
            if (!g.overwrite) {
                close(in);
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, sp, type, size, dp, "skipped: exists");
                return;
            }
            if (unlink(dp) != 0 && errno != ENOENT) {
                int e = errno;
                close(in);
                p3m_note_error(dp, "unlink", e);
                emit_row(ob, sp, type, size, dp, "failed: unlink dest");
                return;
            }
            out = open(dp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       (st->st_mode & 07777) | 0200);
        }
        if (out < 0) {
            int e = errno;
            close(in);
            p3m_note_error(dp, "create", e);
            emit_row(ob, sp, type, size, dp, "failed: create");
            return;
        }
        int rc = p3m_copy_fd(in, out, &n_bytes);
        int e = errno;
        close(in);
        if (rc != 0) {
            close(out);
            unlink(dp);                       /* don't leave a torso */
            p3m_note_error(dp, "copy", e);
            emit_row(ob, sp, type, size, dp, "failed: copy");
            return;
        }
        preserve_fd(out, dp, st);
        if (close(out) != 0) {
            e = errno;
            p3m_note_error(dp, "close", e);
            emit_row(ob, sp, type, size, dp, "failed: write");
            return;
        }
        if (unlinkat(sdirfd, name, 0) != 0 && errno != ENOENT) {
            p3m_note_error(sp, "unlink source", errno);
            emit_row(ob, sp, type, size, dp, "failed: unlink source");
            return;
        }
        emit_row(ob, sp, type, size, dp, "copied");
        return;
    }

    /* ---- symbolic link: recreated, never followed ------------------ */
    if (S_ISLNK(st->st_mode)) {
        char target[PATH_MAX + 1];
        ssize_t tl = readlinkat(sdirfd, name, target, sizeof target - 1);
        if (tl < 0) {
            p3m_note_error(sp, "readlink", errno);
            emit_row(ob, sp, type, 0, dp, "failed: readlink");
            return;
        }
        target[tl] = '\0';
        int rc = symlink(target, dp);
        if (rc != 0 && errno == EEXIST) {
            if (!g.overwrite) {
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, sp, type, 0, dp, "skipped: exists");
                return;
            }
            if (unlink(dp) == 0 || errno == ENOENT)
                rc = symlink(target, dp);
        }
        if (rc != 0) {
            p3m_note_error(dp, "symlink", errno);
            emit_row(ob, sp, type, 0, dp, "failed: symlink");
            return;
        }
        if (lchown(dp, st->st_uid, st->st_gid) != 0 &&
            errno != EPERM && errno != EINVAL)
            p3m_note_error(dp, "chown", errno);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        utimensat(AT_FDCWD, dp, ts, AT_SYMLINK_NOFOLLOW);
        if (unlinkat(sdirfd, name, 0) != 0 && errno != ENOENT) {
            p3m_note_error(sp, "unlink source", errno);
            emit_row(ob, sp, type, 0, dp, "failed: unlink source");
            return;
        }
        emit_row(ob, sp, type, 0, dp, "copied");
        return;
    }

    /* ---- FIFOs and device nodes ------------------------------------ */
    if (S_ISFIFO(st->st_mode) || S_ISCHR(st->st_mode) ||
        S_ISBLK(st->st_mode)) {
        int rc = S_ISFIFO(st->st_mode)
                     ? mkfifo(dp, st->st_mode & 07777)
                     : mknod(dp, st->st_mode & (S_IFMT | 07777),
                             st->st_rdev);
        if (rc != 0 && errno == EEXIST) {
            if (!g.overwrite) {
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, sp, type, 0, dp, "skipped: exists");
                return;
            }
            if (unlink(dp) == 0 || errno == ENOENT)
                rc = S_ISFIFO(st->st_mode)
                         ? mkfifo(dp, st->st_mode & 07777)
                         : mknod(dp, st->st_mode & (S_IFMT | 07777),
                                 st->st_rdev);
        }
        if (rc != 0) {
            p3m_note_error(dp, "mknod", errno);
            emit_row(ob, sp, type, 0, dp, "failed: mknod");
            return;
        }
        if (chown(dp, st->st_uid, st->st_gid) != 0 &&
            errno != EPERM && errno != EINVAL)
            p3m_note_error(dp, "chown", errno);
        chmod(dp, st->st_mode & 07777);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        utimensat(AT_FDCWD, dp, ts, 0);
        if (unlinkat(sdirfd, name, 0) != 0 && errno != ENOENT) {
            p3m_note_error(sp, "unlink source", errno);
            emit_row(ob, sp, type, 0, dp, "failed: unlink source");
            return;
        }
        emit_row(ob, sp, type, 0, dp, "copied");
        return;
    }

    /* sockets cannot be copied across filesystems; the source stays */
    p3m_note_error(sp, "socket: cannot move across filesystems",
                   EOPNOTSUPP);
    emit_row(ob, sp, type, 0, dp, "failed: socket unsupported");
}

/* ------------------------------------------------------------------ */
/* move one non-directory entry                                         */
/* ------------------------------------------------------------------ */

static void mv_entry(int sdirfd, const char *name, const char *sp,
                     const char *dp, const struct stat *st, char ctx,
                     dev_t ddev, p3m_outbuf *ob)
{
    char type = p3m_dt_char(IFTODT(st->st_mode));
    uint64_t size = S_ISREG(st->st_mode) ? (uint64_t)st->st_size : 0;

    atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);

    /* ---- dry run: predict ------------------------------------------ */
    if (!g.apply) {
        const char *res;
        struct stat dst;
        bool exists = ctx != CTX_COPY &&
            fstatat(AT_FDCWD, dp, &dst, AT_SYMLINK_NOFOLLOW) == 0;
        if (exists && !g.overwrite) {
            res = "exists";
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
        } else if (ctx == CTX_RENAME && st->st_dev == ddev) {
            res = exists ? "overwrite" : "pending: rename";
        } else {
            res = exists ? "overwrite" : "pending: copy";
            atomic_fetch_add_explicit(&n_bytes, size, memory_order_relaxed);
        }
        emit_row(ob, sp, type, size, dp, res);
        return;
    }

    /* ---- apply: rename first where it might work -------------------- */
    if (ctx == CTX_RENAME) {
        int rc = g.overwrite ? rename(sp, dp) : rename_noreplace(sp, dp);
        if (rc == 0) {
            atomic_fetch_add_explicit(&n_renames, 1, memory_order_relaxed);
            emit_row(ob, sp, type, size, dp, "renamed");
            return;
        }
        if (errno == EEXIST) {                /* only without --overwrite */
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
            emit_row(ob, sp, type, size, dp, "skipped: exists");
            return;
        }
        if (errno == EISDIR || errno == ENOTEMPTY) {
            /* destination is a directory; even --overwrite only
             * replaces an *empty* one */
            if (g.overwrite && rmdir(dp) == 0 && rename(sp, dp) == 0) {
                atomic_fetch_add_explicit(&n_renames, 1,
                                          memory_order_relaxed);
                emit_row(ob, sp, type, size, dp, "renamed");
                return;
            }
            p3m_note_error(dp, "destination is a non-empty directory",
                           EISDIR);
            emit_row(ob, sp, type, size, dp, "failed: is a directory");
            return;
        }
        if (errno != EXDEV) {
            p3m_note_error(sp, "rename", errno);
            emit_row(ob, sp, type, size, dp, "failed: rename");
            return;
        }
        /* EXDEV: fall through to copy + delete */
    }
    copy_move_entry(sdirfd, name, sp, dp, st, type, size, ob);
}

/* ------------------------------------------------------------------ */
/* move one directory; reports whether and how to walk into it          */
/* ------------------------------------------------------------------ */

static void mv_dir(const char *sp, const char *dp, const struct stat *st,
                   char ctx, dev_t ddev, p3m_outbuf *ob,
                   bool *descend, char *childctx)
{
    *descend = false;
    atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);

    /* ---- dry run: predict ------------------------------------------ */
    if (!g.apply) {
        struct stat dst;
        bool exists = ctx != CTX_COPY &&
            fstatat(AT_FDCWD, dp, &dst, AT_SYMLINK_NOFOLLOW) == 0;
        if (exists && S_ISDIR(dst.st_mode)) {
            emit_row(ob, sp, 'd', 0, dp, "merge");
            *descend = true;
            *childctx = ctx == CTX_RENAME ? CTX_RENAME : CTX_XCOPY;
        } else if (exists && !g.overwrite) {
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
            emit_row(ob, sp, 'd', 0, dp, "exists");
        } else if (ctx == CTX_RENAME && st->st_dev == ddev) {
            emit_row(ob, sp, 'd', 0, dp,
                     exists ? "overwrite" : "pending: rename");
        } else {
            emit_row(ob, sp, 'd', 0, dp,
                     exists ? "overwrite" : "pending: copy");
            *descend = true;
            *childctx = CTX_COPY;
        }
        return;
    }

    /* ---- apply ------------------------------------------------------ */
    if (ctx == CTX_RENAME) {
        int rc = g.overwrite ? rename(sp, dp) : rename_noreplace(sp, dp);
        if (rc == 0) {
            atomic_fetch_add_explicit(&n_renames, 1, memory_order_relaxed);
            emit_row(ob, sp, 'd', 0, dp, "renamed");
            return;
        }
        if (errno == EEXIST || errno == ENOTEMPTY || errno == EISDIR) {
            struct stat dst;
            if (fstatat(AT_FDCWD, dp, &dst, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISDIR(dst.st_mode)) {
                emit_row(ob, sp, 'd', 0, dp, "merge");
                rml_add(sp);
                *descend = true;
                *childctx = CTX_RENAME;       /* children may rename in */
                return;
            }
            if (g.overwrite && (unlink(dp) == 0 || errno == ENOENT) &&
                rename(sp, dp) == 0) {
                atomic_fetch_add_explicit(&n_renames, 1,
                                          memory_order_relaxed);
                emit_row(ob, sp, 'd', 0, dp, "renamed");
                return;
            }
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
            emit_row(ob, sp, 'd', 0, dp, "skipped: exists");
            return;
        }
        if (errno != EXDEV) {
            p3m_note_error(sp, "rename", errno);
            emit_row(ob, sp, 'd', 0, dp, "failed: rename");
            return;
        }
        /* EXDEV: fall through to copy + delete */
    }

    /* cross-device: create or merge the destination directory */
    int rc = mkdir(dp, (st->st_mode & 0777) | S_IRWXU);
    if (rc != 0 && errno == EEXIST && ctx != CTX_COPY) {
        struct stat dst;
        if (fstatat(AT_FDCWD, dp, &dst, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(dst.st_mode)) {
            emit_row(ob, sp, 'd', 0, dp, "merge");
            rml_add(sp);
            *descend = true;
            *childctx = CTX_XCOPY;
            return;
        }
        if (!g.overwrite) {
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
            emit_row(ob, sp, 'd', 0, dp, "skipped: exists");
            return;
        }
        if (unlink(dp) == 0 || errno == ENOENT)
            rc = mkdir(dp, (st->st_mode & 0777) | S_IRWXU);
    }
    if (rc != 0) {
        p3m_note_error(dp, "mkdir", errno);
        emit_row(ob, sp, 'd', 0, dp, "failed: mkdir");
        return;
    }
    dirfix_add(dp, st);
    rml_add(sp);
    emit_row(ob, sp, 'd', 0, dp, "copied");
    *descend = true;
    *childctx = CTX_COPY;
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static void scan_dir(const char *spath, const char *dpath, char ctx,
                     p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(spath);

    /* the destination dir exists in rename/merge contexts; its device
     * drives the dry-run rename-vs-copy prediction */
    dev_t ddev = 0;
    if (ctx == CTX_RENAME) {
        struct stat dst;
        if (stat(dpath, &dst) == 0)
            ddev = dst.st_dev;
    }

    int fd = open(spath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        p3m_note_error(spath, "open", errno);
        return;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        int e = errno;
        close(fd);
        p3m_note_error(spath, "fdopendir", e);
        return;
    }
    size_t slen = strlen(spath), dlen = strlen(dpath);

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

        struct stat st;
        if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            char *sp = p3m_path_join(spath, slen, nm);
            p3m_note_error(sp ? sp : nm, "stat", errno);
            free(sp);
            errno = 0;
            continue;
        }

        char *sp = p3m_path_join(spath, slen, nm);
        char *dp = p3m_path_join(dpath, dlen, nm);
        if (!sp || !dp) {
            p3m_note_error(nm, "malloc", ENOMEM);
            free(sp);
            free(dp);
            errno = 0;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            bool descend;
            char childctx = ctx;
            mv_dir(sp, dp, &st, ctx, ddev, ob, &descend, &childctx);
            if (descend) {
                char *it = pack_item(sp, dp, childctx);
                if (it) {
                    if (nsub == csub) {
                        csub = csub ? csub * 2 : 32;
                        char **ns = realloc(subs, csub * sizeof *ns);
                        if (!ns) {
                            p3m_note_error(sp, "queue", ENOMEM);
                            free(it);
                            it = NULL;
                        } else {
                            subs = ns;
                        }
                    }
                    if (it)
                        subs[nsub++] = it;
                } else {
                    p3m_note_error(sp, "queue", ENOMEM);
                }
            }
        } else {
            mv_entry(fd, nm, sp, dp, &st, ctx, ddev, ob);
        }
        free(sp);
        free(dp);
        errno = 0;
    }
    if (errno)
        p3m_note_error(spath, "readdir", errno);
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
    char *item;
    while ((item = p3m_stack_pop(&stk)) != NULL) {
        const char *src = item;
        const char *dst = item + strlen(item) + 1;
        char ctx = dst[strlen(dst) + 1];
        scan_dir(src, dst, ctx, &ob);
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
    return atomic_load_explicit(&n_files, memory_order_relaxed) +
           atomic_load_explicit(&n_dirs, memory_order_relaxed);
}

static void prog_draw(double rate, int frame)
{
    uint64_t files = atomic_load_explicit(&n_files, memory_order_relaxed);
    uint64_t dirs  = atomic_load_explicit(&n_dirs, memory_order_relaxed);
    uint64_t errs  = atomic_load_explicit(&p3m_nerrors, memory_order_relaxed);
    uint64_t bytes = atomic_load_explicit(&n_bytes, memory_order_relaxed);
    uint64_t ren   = atomic_load_explicit(&n_renames, memory_order_relaxed);

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

    char fv[32], dv[32], ev[32], rv[32], sv[32], tv[32], nv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_size(el_s > 0 ? (uint64_t)((double)bytes / el_s) : 0, tv);
    p3m_fmt_u64(ren, nv);
    p3m_fmt_elapsed(el_s, el);

    char ratestr[48], sizestr[80];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);
    if (g.apply)
        snprintf(sizestr, sizeof sizestr, "%s · %s/s", sv, tv);
    else
        snprintf(sizestr, sizeof sizestr, "%s to copy", sv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-mv%s %s— parallel move%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "none (-q)");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "action", C_RESET, g.apply ? "apply" : "dry-run");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s\n",
        C_DIM, "renames", C_RESET, nv, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %-22s %s%-8s%s %s\n",
        C_DIM, "size", C_RESET, sizestr, C_DIM, "elapsed", C_RESET, el);
#undef ADD
    fwrite(buf, 1, off, stderr);
    (void)ratestr;
}

/* ------------------------------------------------------------------ */
/* summary                                                              */
/* ------------------------------------------------------------------ */

static void print_summary(double elapsed)
{
    uint64_t files = atomic_load(&n_files);
    uint64_t dirs  = atomic_load(&n_dirs);
    uint64_t ren   = atomic_load(&n_renames);
    uint64_t skip  = atomic_load(&n_skipped);
    uint64_t errs  = atomic_load(&p3m_nerrors);
    uint64_t bytes = atomic_load(&n_bytes);
    uint64_t left  = atomic_load(&n_left);

    char fv[32], dv[32], nv[32], kv[32], ev[32], rv[32], sv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(ren, nv);
    p3m_fmt_u64(skip, kv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-mv%s complete — %s · %s files · %s dirs · "
            "%s rename%s · %s copied · %s%s skipped%s · %s%s error%s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET,
            g.apply ? "apply" : "dry-run", fv, dv,
            nv, ren == 1 ? "" : "s", sv,
            skip ? C_BOLD : "", kv, skip ? C_RESET : "",
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s)", el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
    if (!g.apply)
        fprintf(stderr, "  %sdry run — nothing was moved; "
                "add --apply to move%s\n", C_DIM, C_RESET);
    if (skip && !g.overwrite)
        fprintf(stderr, "  %s%s existing destination%s left untouched "
                "(sources kept); add --overwrite to replace%s\n",
                C_DIM, kv, skip == 1 ? "" : "s", C_RESET);
    if (left) {
        char lv[32];
        p3m_fmt_u64(left, lv);
        fprintf(stderr, "  %s%s source director%s kept "
                "(still hold skipped or failed entries)%s\n",
                C_DIM, lv, left == 1 ? "y" : "ies", C_RESET);
    }
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-mv [OPTIONS] SOURCE... DEST\n"
"\n"
"Move files and directory trees in parallel. On the same filesystem a\n"
"subtree moves with a single rename — instant regardless of size; across\n"
"filesystems it is copied in parallel and the source removed. Unlike mv,\n"
"an existing destination directory is merged rather than refused.\n"
"By default this is a dry run listing what would happen; add --apply.\n"
"\n"
"Options:\n"
"      --apply         actually move (the default is a dry run)\n"
"      --overwrite     replace existing destination entries; without it\n"
"                      they are skipped and their sources kept\n"
"  -j, --threads N     worker threads (default: number of online CPUs)\n"
"  -o, --output FILE   write CSV to FILE; a live progress display is shown\n"
"  -q, --quiet         suppress the console listing (progress and the\n"
"                      summary are still shown; a -o file is still written)\n"
"  -h, --help          show this help and exit\n"
"  -V, --version       show version and exit\n"
"\n"
"The filesystem root can never be a source — this guard cannot be\n"
"overridden. Symbolic links are moved as links, never followed.\n",
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

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "apply",     no_argument,       NULL, 1000 },
        { "overwrite", no_argument,       NULL, 1001 },
        { "threads",   required_argument, NULL, 'j' },
        { "output",    required_argument, NULL, 'o' },
        { "quiet",     no_argument,       NULL, 'q' },
        { "help",      no_argument,       NULL, 'h' },
        { "version",   no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "j:o:qhV", lopts, NULL)) != -1) {
        switch (c) {
        case 1000:
            g.apply = true;
            break;
        case 1001:
            g.overwrite = true;
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-mv: invalid thread count '%s' "
                        "(expected 1-512)\n", optarg);
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
            printf("p3m-mv %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_MV_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    int nargs = argc - optind;
    if (nargs < 2) {
        fprintf(stderr, "p3m-mv: expected SOURCE... DEST\n");
        usage(stderr);
        return 2;
    }
    int nsrc = nargs - 1;

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);
    g.suppress = g.quiet && !g.outpath;
    g.progress = (g.outpath || g.quiet) && isatty(STDERR_FILENO);
    umask(0);                 /* moves preserve modes exactly */

    char *dest = strdup(argv[argc - 1]);
    if (!dest) {
        fprintf(stderr, "p3m-mv: out of memory\n");
        return 2;
    }
    size_t destlen = strlen(dest);
    while (destlen > 1 && dest[destlen - 1] == '/')
        dest[--destlen] = '\0';

    struct stat dst;
    bool dest_exists = lstat(dest, &dst) == 0;
    bool dest_isdir  = dest_exists && S_ISDIR(dst.st_mode);

    if (nsrc > 1 && !dest_isdir) {
        fprintf(stderr, "p3m-mv: target '%s' must be an existing directory "
                "when moving multiple sources\n", dest);
        return 2;
    }

    /* ---- guard pass: refused before any work or output starts ------- */
    char rdest[PATH_MAX];
    bool have_rdest = resolve_dest(dest, rdest) == 0;
    for (int i = 0; i < nsrc; i++) {
        const char *arg = argv[optind + i];

        /* the root guard runs first so '/' gets the FATAL message,
         * not the generic basename refusal */
        struct stat st;
        bool have_st = lstat(arg, &st) == 0;
        if (have_st && !S_ISLNK(st.st_mode)) {
            char rsrc[PATH_MAX];
            if (realpath(arg, rsrc) && !strcmp(rsrc, "/")) {
                fprintf(stderr, "p3m-mv: %sFATAL%s: '%s' resolves to '/' — "
                        "refusing to move the filesystem root.\n"
                        "        This guard cannot be overridden.\n",
                        C_RED, C_RESET, arg);
                return 2;
            }
        }

        /* '.', '..' and paths ending in them cannot be moved */
        const char *b = strrchr(arg, '/');
        b = b ? b + 1 : arg;
        if (!strcmp(b, ".") || !strcmp(b, "..") || *b == '\0') {
            fprintf(stderr, "p3m-mv: refusing to move '%s'\n", arg);
            return 2;
        }

        if (!have_st || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode) ||
            !have_rdest)
            continue;

        char rsrc[PATH_MAX];
        if (!realpath(arg, rsrc))
            continue;

        /* the tree would land at rdest/<basename> or rdest itself */
        char landing[PATH_MAX + 256];
        if (dest_isdir)
            snprintf(landing, sizeof landing, "%s/%s", rdest, b);
        else
            snprintf(landing, sizeof landing, "%s", rdest);
        size_t rl = strlen(rsrc);
        if (!strcmp(landing, rsrc) ||
            (!strncmp(landing, rsrc, rl) && landing[rl] == '/')) {
            fprintf(stderr, "p3m-mv: %sFATAL%s: cannot move '%s' into "
                    "itself ('%s')\n", C_RED, C_RESET, arg, landing);
            return 2;
        }
    }

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-mv: cannot open '%s': %s\n",
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
        fputs("source,type,size,destination,result\n", out);

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

    /* dry-run rename prediction needs the destination device */
    dev_t root_ddev = 0;
    {
        struct stat pd;
        if (dest_exists)
            root_ddev = dst.st_dev;
        else if (have_rdest) {
            char tmp[PATH_MAX];
            strcpy(tmp, rdest);
            char *slash = strrchr(tmp, '/');
            if (slash) {
                if (slash == tmp)
                    slash[1] = '\0';
                else
                    *slash = '\0';
                if (stat(tmp, &pd) == 0)
                    root_ddev = pd.st_dev;
            }
        }
    }

    /* ---- seed from the SOURCE arguments ------------------------------ */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-mv: out of memory\n");
        return 2;
    }
    for (int i = 0; i < nsrc; i++) {
        char *src = strdup(argv[optind + i]);
        if (!src)
            continue;
        size_t len = strlen(src);
        while (len > 1 && src[len - 1] == '/')
            src[--len] = '\0';

        struct stat st;
        if (lstat(src, &st) != 0) {
            p3m_note_error(src, "stat", errno);
            free(src);
            continue;
        }

        char *dpath;
        if (dest_isdir) {
            const char *b = strrchr(src, '/');
            b = b ? b + 1 : src;
            dpath = p3m_path_join(dest, destlen, b);
        } else {
            dpath = strdup(dest);
        }
        if (!dpath) {
            p3m_note_error(src, "malloc", ENOMEM);
            free(src);
            continue;
        }

        struct stat dsame;
        if (lstat(dpath, &dsame) == 0 &&
            dsame.st_dev == st.st_dev && dsame.st_ino == st.st_ino) {
            p3m_note_error(src, "source and destination are the same",
                           EEXIST);
        } else if (S_ISDIR(st.st_mode)) {
            bool descend;
            char childctx = CTX_RENAME;
            mv_dir(src, dpath, &st, CTX_RENAME, root_ddev, &rootob,
                   &descend, &childctx);
            if (descend) {
                char *it = pack_item(src, dpath, childctx);
                if (it)
                    p3m_stack_push_batch(&stk, &it, 1);
                else
                    p3m_note_error(src, "queue", ENOMEM);
            }
        } else {
            mv_entry(AT_FDCWD, src, src, dpath, &st, CTX_RENAME,
                     root_ddev, &rootob);
        }
        free(src);
        free(dpath);
    }
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    /* ---- launch ------------------------------------------------------ */
    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-mv: out of memory\n");
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
            fprintf(stderr, "p3m-mv: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    if (g.apply) {
        dirfix_apply();       /* destination metadata, deepest first */
        remove_srcdirs();     /* emptied source directories          */
    }

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    /* finish the output stream */
    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-mv: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);
    free(dest);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
