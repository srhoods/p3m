/*
 * p3m-cp — parallel copy
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Copies files and directory trees with a pool of worker threads, like
 * cp -R but parallel and safety-first: dry run by default (--apply to
 * copy), existing destinations are never overwritten unless --overwrite
 * is given, and symbolic links are never followed — they are recreated
 * as links.
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

#define P3M_CP_VERSION "1.0.0"

static struct {
    bool         apply;
    bool         overwrite;
    bool         preserve;    /* -p: mode, ownership, timestamps        */
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
    mode_t       umask;       /* saved process umask (umask(0) is set)  */
} g;

static _Atomic uint64_t n_files;    /* non-directory entries seen        */
static _Atomic uint64_t n_dirs;     /* directories seen                  */
static _Atomic uint64_t n_bytes;    /* bytes copied (apply) / to copy    */
static _Atomic uint64_t n_skipped;  /* existing destinations not touched */

static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* work items: a source directory, its destination, and whether the    */
/* destination may already exist (children need existence checks only  */
/* when merging into a pre-existing directory)                          */
/* ------------------------------------------------------------------ */

static char *pack_item(const char *src, const char *dst, bool may_exist)
{
    size_t sl = strlen(src), dl = strlen(dst);
    char *it = malloc(sl + 1 + dl + 1 + 1);
    if (!it)
        return NULL;
    memcpy(it, src, sl + 1);
    memcpy(it + sl + 1, dst, dl + 1);
    it[sl + 1 + dl + 1] = may_exist ? 'E' : 'A';
    return it;
}

/* ------------------------------------------------------------------ */
/* directory fix-up list                                                */
/*                                                                      */
/* Destination directories are created owner-writable so workers can    */
/* populate them; their final mode (and, with -p, ownership and times   */
/* — writes inside bump a directory's mtime) is applied afterwards,     */
/* deepest first so a restrictive parent mode cannot cut off children.  */
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
    f->mode  = g.preserve ? (st->st_mode & 07777)
                          : (st->st_mode & 0777 & ~g.umask);
    f->uid   = st->st_uid;
    f->gid   = st->st_gid;
    f->ts[0] = st->st_atim;
    f->ts[1] = st->st_mtim;
    f->depth = depth;
    pthread_mutex_unlock(&dfx_mu);
}

static int dfx_cmp(const void *a, const void *b)
{
    const struct dirfix *x = a, *y = b;
    return y->depth - x->depth;               /* deepest first */
}

static void dirfix_apply(void)
{
    qsort(dfx, n_dfx, sizeof *dfx, dfx_cmp);
    for (size_t i = 0; i < n_dfx; i++) {
        struct dirfix *f = &dfx[i];
        if (g.preserve) {
            if (chown(f->path, f->uid, f->gid) != 0 &&
                errno != EPERM && errno != EINVAL)
                p3m_note_error(f->path, "chown", errno);
        }
        if (chmod(f->path, f->mode) != 0)
            p3m_note_error(f->path, "chmod", errno);
        if (g.preserve && utimensat(AT_FDCWD, f->path, f->ts, 0) != 0)
            p3m_note_error(f->path, "utimens", errno);
        free(f->path);
    }
    free(dfx);
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
/* file content copy                                                    */
/* ------------------------------------------------------------------ */

#define CP_CHUNK ((size_t)4 << 20)

static __thread char *cpbuf;                  /* fallback read/write buf */

/* copies open fd in -> out; returns 0 ok, -1 with errno set */
static int copy_content(int in, int out)
{
    for (;;) {
        ssize_t r = copy_file_range(in, NULL, out, NULL, CP_CHUNK, 0);
        if (r > 0) {
            atomic_fetch_add_explicit(&n_bytes, (uint64_t)r,
                                      memory_order_relaxed);
            continue;
        }
        if (r == 0)
            return 0;
        if (errno != EINVAL && errno != EXDEV &&
            errno != ENOSYS && errno != EOPNOTSUPP)
            return -1;
        break;                                /* fall back to read/write */
    }
    if (!cpbuf) {
        cpbuf = malloc(1 << 20);
        if (!cpbuf) {
            errno = ENOMEM;
            return -1;
        }
    }
    for (;;) {
        ssize_t r = read(in, cpbuf, 1 << 20);
        if (r == 0)
            return 0;
        if (r < 0)
            return -1;
        char *p = cpbuf;
        while (r > 0) {
            ssize_t w = write(out, p, (size_t)r);
            if (w < 0)
                return -1;
            p += w;
            r -= w;
        }
        atomic_fetch_add_explicit(&n_bytes, (uint64_t)(p - cpbuf),
                                  memory_order_relaxed);
    }
}

/* apply -p metadata to an fd; chown first (chown clears set-id bits) */
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

/*
 * Copy one non-directory entry. srcdirfd/name locate the source
 * (AT_FDCWD + full path for command-line arguments); spath is the
 * source path as printed. Writes the CSV row itself.
 */
static void copy_entry(int srcdirfd, const char *name, const char *spath,
                       const char *dpath, const struct stat *st,
                       bool may_exist, p3m_outbuf *ob)
{
    char type = p3m_dt_char(IFTODT(st->st_mode));
    uint64_t size = S_ISREG(st->st_mode) ? (uint64_t)st->st_size : 0;

    atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);

    /* ---- dry run: report what would happen -------------------------- */
    if (!g.apply) {
        const char *res = "pending";
        struct stat dst;
        if (may_exist &&
            fstatat(AT_FDCWD, dpath, &dst, AT_SYMLINK_NOFOLLOW) == 0) {
            if (g.overwrite) {
                res = "overwrite";
            } else {
                res = "exists";
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
            }
        }
        atomic_fetch_add_explicit(&n_bytes, size, memory_order_relaxed);
        emit_row(ob, spath, type, size, dpath, res);
        return;
    }

    /* ---- regular file ------------------------------------------------ */
    if (S_ISREG(st->st_mode)) {
        int in = openat(srcdirfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (in < 0) {
            int e = errno;
            p3m_note_error(spath, "open", e);
            emit_row(ob, spath, type, size, dpath, "failed: open source");
            return;
        }
        mode_t createmode = g.preserve ? (st->st_mode & 07777)
                                       : (st->st_mode & 0777 & ~g.umask);
        /* O_EXCL never follows a symlink planted at the destination */
        int out = open(dpath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       createmode | 0200);
        if (out < 0 && errno == EEXIST) {
            if (!g.overwrite) {
                close(in);
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, spath, type, size, dpath, "skipped: exists");
                return;
            }
            if (unlink(dpath) != 0 && errno != ENOENT) {
                int e = errno;
                close(in);
                p3m_note_error(dpath, "unlink", e);
                emit_row(ob, spath, type, size, dpath, "failed: unlink");
                return;
            }
            out = open(dpath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       createmode | 0200);
        }
        if (out < 0) {
            int e = errno;
            close(in);
            p3m_note_error(dpath, "create", e);
            emit_row(ob, spath, type, size, dpath, "failed: create");
            return;
        }
        int rc = copy_content(in, out);
        int e = errno;
        close(in);
        if (rc != 0) {
            close(out);
            unlink(dpath);                    /* don't leave a torso */
            p3m_note_error(dpath, "copy", e);
            emit_row(ob, spath, type, size, dpath, "failed: copy");
            return;
        }
        if (g.preserve)
            preserve_fd(out, dpath, st);
        else if (createmode & ~(mode_t)0777)
            fchmod(out, createmode);
        if (close(out) != 0) {
            e = errno;
            p3m_note_error(dpath, "close", e);
            emit_row(ob, spath, type, size, dpath, "failed: write");
            return;
        }
        emit_row(ob, spath, type, size, dpath, "copied");
        return;
    }

    /* ---- symbolic link: recreated, never followed --------------------- */
    if (S_ISLNK(st->st_mode)) {
        char target[PATH_MAX + 1];
        ssize_t tl = readlinkat(srcdirfd, name, target, sizeof target - 1);
        if (tl < 0) {
            int e = errno;
            p3m_note_error(spath, "readlink", e);
            emit_row(ob, spath, type, 0, dpath, "failed: readlink");
            return;
        }
        target[tl] = '\0';
        int rc = symlink(target, dpath);
        if (rc != 0 && errno == EEXIST) {
            if (!g.overwrite) {
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, spath, type, 0, dpath, "skipped: exists");
                return;
            }
            if (unlink(dpath) == 0 || errno == ENOENT)
                rc = symlink(target, dpath);
        }
        if (rc != 0) {
            int e = errno;
            p3m_note_error(dpath, "symlink", e);
            emit_row(ob, spath, type, 0, dpath, "failed: symlink");
            return;
        }
        if (g.preserve) {
            if (lchown(dpath, st->st_uid, st->st_gid) != 0 &&
                errno != EPERM && errno != EINVAL)
                p3m_note_error(dpath, "chown", errno);
            struct timespec ts[2] = { st->st_atim, st->st_mtim };
            utimensat(AT_FDCWD, dpath, ts, AT_SYMLINK_NOFOLLOW);
        }
        emit_row(ob, spath, type, 0, dpath, "copied");
        return;
    }

    /* ---- FIFOs and device nodes --------------------------------------- */
    if (S_ISFIFO(st->st_mode) || S_ISCHR(st->st_mode) ||
        S_ISBLK(st->st_mode)) {
        mode_t m = (st->st_mode & (S_IFMT | 07777));
        if (!g.preserve)
            m = (m & S_IFMT) | (m & 0777 & ~g.umask);
        int rc = S_ISFIFO(st->st_mode) ? mkfifo(dpath, m & 07777)
                                       : mknod(dpath, m, st->st_rdev);
        if (rc != 0 && errno == EEXIST) {
            if (!g.overwrite) {
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                emit_row(ob, spath, type, 0, dpath, "skipped: exists");
                return;
            }
            if (unlink(dpath) == 0 || errno == ENOENT)
                rc = S_ISFIFO(st->st_mode) ? mkfifo(dpath, m & 07777)
                                           : mknod(dpath, m, st->st_rdev);
        }
        if (rc != 0) {
            int e = errno;
            p3m_note_error(dpath, "mknod", e);
            emit_row(ob, spath, type, 0, dpath, "failed: mknod");
            return;
        }
        if (g.preserve) {
            if (chown(dpath, st->st_uid, st->st_gid) != 0 &&
                errno != EPERM && errno != EINVAL)
                p3m_note_error(dpath, "chown", errno);
            chmod(dpath, st->st_mode & 07777);
            struct timespec ts[2] = { st->st_atim, st->st_mtim };
            utimensat(AT_FDCWD, dpath, ts, 0);
        }
        emit_row(ob, spath, type, 0, dpath, "copied");
        return;
    }

    /* sockets have no meaning when copied */
    p3m_note_error(spath, "socket: cannot copy", EOPNOTSUPP);
    emit_row(ob, spath, type, 0, dpath, "failed: socket unsupported");
}

/*
 * Handle a directory encountered during the walk (or a root argument):
 * create/merge the destination, emit the row, and report through
 * *descend / *child_may_exist whether and how to walk into it.
 */
static void copy_dir(const char *spath, const char *dpath,
                     const struct stat *st, bool may_exist,
                     p3m_outbuf *ob, bool *descend, bool *child_may_exist)
{
    *descend = true;
    *child_may_exist = false;

    atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);

    if (!g.apply) {
        const char *res = "pending";
        struct stat dst;
        if (may_exist &&
            fstatat(AT_FDCWD, dpath, &dst, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISDIR(dst.st_mode)) {
                res = "merge";
                *child_may_exist = true;
            } else if (g.overwrite) {
                res = "overwrite";
            } else {
                res = "exists";
                atomic_fetch_add_explicit(&n_skipped, 1,
                                          memory_order_relaxed);
                *descend = false;
            }
        }
        emit_row(ob, spath, 'd', 0, dpath, res);
        return;
    }

    /* owner rwx during population; the final mode lands in the fix-up */
    mode_t createmode = (st->st_mode & 0777 & ~g.umask) | S_IRWXU;
    int rc = mkdir(dpath, createmode);
    if (rc != 0 && errno == EEXIST) {
        struct stat dst;
        if (fstatat(AT_FDCWD, dpath, &dst, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(dst.st_mode)) {
            *child_may_exist = true;
            dirfix_add(dpath, st);
            emit_row(ob, spath, 'd', 0, dpath, "merged");
            return;
        }
        if (!g.overwrite) {
            atomic_fetch_add_explicit(&n_skipped, 1, memory_order_relaxed);
            emit_row(ob, spath, 'd', 0, dpath, "skipped: exists");
            *descend = false;
            return;
        }
        if (unlink(dpath) == 0 || errno == ENOENT)
            rc = mkdir(dpath, createmode);
    }
    if (rc != 0) {
        int e = errno;
        p3m_note_error(dpath, "mkdir", e);
        emit_row(ob, spath, 'd', 0, dpath, "failed: mkdir");
        *descend = false;
        return;
    }
    dirfix_add(dpath, st);
    emit_row(ob, spath, 'd', 0, dpath, "created");
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static void scan_dir(const char *spath, const char *dpath, bool may_exist,
                     p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(spath);

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
            bool descend, child_may;
            copy_dir(sp, dp, &st, may_exist, ob, &descend, &child_may);
            if (descend) {
                char *it = pack_item(sp, dp, child_may);
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
            copy_entry(fd, nm, sp, dp, &st, may_exist, ob);
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
        bool may_exist = dst[strlen(dst) + 1] == 'E';
        scan_dir(src, dst, may_exist, &ob);
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

    char fv[32], dv[32], ev[32], rv[32], sv[32], tv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_size(el_s > 0 ? (uint64_t)((double)bytes / el_s) : 0, tv);
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
    ADD("\x1b[K%s%s%s %sp3m-cp%s %s— parallel copy%s\n",
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
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %-22s %s%-8s%s %s\n",
        C_DIM, "size", C_RESET, sizestr, C_DIM, "elapsed", C_RESET, el);
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
    uint64_t skip  = atomic_load(&n_skipped);
    uint64_t errs  = atomic_load(&p3m_nerrors);
    uint64_t bytes = atomic_load(&n_bytes);

    char fv[32], dv[32], kv[32], ev[32], rv[32], sv[32], tv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(skip, kv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_size(elapsed > 0 ? (uint64_t)((double)bytes / elapsed) : 0, tv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-cp%s complete — %s · %s files · %s dirs · "
            "%s %s · %s%s skipped%s · %s%s error%s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET,
            g.apply ? "apply" : "dry-run", fv, dv,
            sv, g.apply ? "copied" : "to copy",
            skip ? C_BOLD : "", kv, skip ? C_RESET : "",
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s", el, rv);
    if (g.apply)
        fprintf(stderr, " · %s/s", tv);
    fputc(')', stderr);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
    if (!g.apply)
        fprintf(stderr, "  %sdry run — nothing was copied; "
                "add --apply to copy%s\n", C_DIM, C_RESET);
    if (skip && !g.overwrite)
        fprintf(stderr, "  %s%s existing destination%s left untouched; "
                "add --overwrite to replace%s\n",
                C_DIM, kv, skip == 1 ? "" : "s", C_RESET);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-cp [OPTIONS] SOURCE... DEST\n"
"\n"
"Copy files and directory trees in parallel (recursive, like cp -R).\n"
"By default this is a dry run listing everything that would be copied;\n"
"add --apply to copy. If DEST is an existing directory, sources are\n"
"copied into it; otherwise a single SOURCE is copied to the path DEST.\n"
"\n"
"Options:\n"
"      --apply         actually copy (the default is a dry run)\n"
"      --overwrite     replace existing destination entries; without it\n"
"                      they are skipped and reported\n"
"  -p, --preserve      preserve mode, ownership (when permitted) and\n"
"                      timestamps; default is mode masked by umask, as cp\n"
"  -j, --threads N     worker threads (default: number of online CPUs)\n"
"  -o, --output FILE   write CSV to FILE; a live progress display is shown\n"
"  -q, --quiet         suppress the console listing (progress and the\n"
"                      summary are still shown; a -o file is still written)\n"
"  -h, --help          show this help and exit\n"
"  -V, --version       show version and exit\n"
"\n"
"Symbolic links are never followed: they are recreated as links.\n"
"Copying a directory into itself is refused.\n",
    to);
}

/* resolved destination path for guard checks (DEST itself may not
 * exist yet: resolve its parent and re-append the last component) */
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
        { "preserve",  no_argument,       NULL, 'p' },
        { "threads",   required_argument, NULL, 'j' },
        { "output",    required_argument, NULL, 'o' },
        { "quiet",     no_argument,       NULL, 'q' },
        { "help",      no_argument,       NULL, 'h' },
        { "version",   no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "pj:o:qhV", lopts, NULL)) != -1) {
        switch (c) {
        case 1000:
            g.apply = true;
            break;
        case 1001:
            g.overwrite = true;
            break;
        case 'p':
            g.preserve = true;
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-cp: invalid thread count '%s' "
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
            printf("p3m-cp %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_CP_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    int nargs = argc - optind;
    if (nargs < 2) {
        fprintf(stderr, "p3m-cp: expected SOURCE... DEST\n");
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
    g.umask = umask(0);           /* modes are applied exactly from here */

    char *dest = strdup(argv[argc - 1]);
    if (!dest) {
        fprintf(stderr, "p3m-cp: out of memory\n");
        return 2;
    }
    size_t destlen = strlen(dest);
    while (destlen > 1 && dest[destlen - 1] == '/')
        dest[--destlen] = '\0';

    struct stat dst;
    bool dest_exists = lstat(dest, &dst) == 0;
    bool dest_isdir  = dest_exists && S_ISDIR(dst.st_mode);

    if (nsrc > 1 && !dest_isdir) {
        fprintf(stderr, "p3m-cp: target '%s' must be an existing directory "
                "when copying multiple sources\n", dest);
        return 2;
    }

    /* guard pass: refuse copying any directory into itself before any
     * work (or output) starts */
    char rdest[PATH_MAX];
    bool have_rdest = resolve_dest(dest, rdest) == 0;
    for (int i = 0; i < nsrc; i++) {
        struct stat st;
        if (lstat(argv[optind + i], &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        char rsrc[PATH_MAX];
        if (!realpath(argv[optind + i], rsrc) || !have_rdest)
            continue;
        /* the tree lands at rdest/<basename> (into a dir) or rdest */
        char landing[PATH_MAX + 256];
        if (dest_isdir) {
            char *tmp = strdup(argv[optind + i]);
            if (!tmp)
                continue;
            size_t tl = strlen(tmp);
            while (tl > 1 && tmp[tl - 1] == '/')
                tmp[--tl] = '\0';
            char *b = strrchr(tmp, '/');
            snprintf(landing, sizeof landing, "%s/%s", rdest,
                     b ? b + 1 : tmp);
            free(tmp);
        } else {
            snprintf(landing, sizeof landing, "%s", rdest);
        }
        size_t rl = strlen(rsrc);
        if (!strcmp(landing, rsrc) ||
            (!strncmp(landing, rsrc, rl) && landing[rl] == '/')) {
            fprintf(stderr, "p3m-cp: %sFATAL%s: cannot copy '%s' into "
                    "itself ('%s')\n", C_RED, C_RESET,
                    argv[optind + i], landing);
            return 2;
        }
    }

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-cp: cannot open '%s': %s\n",
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

    /* seed the queue from the SOURCE arguments */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-cp: out of memory\n");
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

        if (S_ISDIR(st.st_mode)) {
            if (dest_exists && !dest_isdir) {
                p3m_note_error(src, "cannot overwrite non-directory "
                               "destination with a directory", EEXIST);
            } else {
                bool descend, child_may;
                copy_dir(src, dpath, &st, true, &rootob,
                         &descend, &child_may);
                if (descend) {
                    char *it = pack_item(src, dpath, child_may);
                    if (it)
                        p3m_stack_push_batch(&stk, &it, 1);
                    else
                        p3m_note_error(src, "queue", ENOMEM);
                }
            }
        } else {
            struct stat dsame;
            if (lstat(dpath, &dsame) == 0 &&
                dsame.st_dev == st.st_dev && dsame.st_ino == st.st_ino) {
                p3m_note_error(src, "source and destination are the "
                               "same file", EEXIST);
            } else {
                copy_entry(AT_FDCWD, src, src, dpath, &st, true, &rootob);
            }
        }
        free(src);
        free(dpath);
    }
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

    /* launch */
    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-cp: out of memory\n");
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
            fprintf(stderr, "p3m-cp: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);
    free(tids);

    /* directory modes, ownership and times, deepest first */
    if (g.apply)
        dirfix_apply();

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    /* finish the output stream */
    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-cp: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);
    free(dest);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
