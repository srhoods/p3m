/*
 * p3m-ch — parallel chmod + chown + chgrp in a single scan
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Walks one or more directory trees with a pool of worker threads and
 * changes mode, owner and group in one pass. Directories and files can
 * be given different modes. Defaults to a dry run that lists the
 * changes which would be applied; --apply makes them. Only entries
 * whose metadata actually differs generate syscalls, so re-running on a
 * compliant tree is nearly free.
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

#define P3M_CH_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* mode specifications: octal or symbolic clause list                   */
/* ------------------------------------------------------------------ */

#define MAX_CLAUSES 16

struct clause {
    unsigned who;    /* bit0 u, bit1 g, bit2 o; 0 => omitted (a, umasked) */
    char     op;     /* + - =                                             */
    unsigned rwx;    /* requested rwx bits, 0-7                           */
    bool     X;      /* conditional execute                               */
    bool     s;      /* setuid/setgid                                     */
    bool     t;      /* sticky                                            */
    char     copy;   /* 'u','g','o': copy that class's bits, else 0       */
};

struct modespec {
    bool          set;
    bool          octal;
    mode_t        omode;
    int           n;
    struct clause cl[MAX_CLAUSES];
};

static int parse_symbolic(const char *s, struct modespec *ms)
{
    ms->n = 0;
    while (*s) {
        unsigned who = 0;
        bool who_given = false;
        for (;; s++) {
            if      (*s == 'u') who |= 1;
            else if (*s == 'g') who |= 2;
            else if (*s == 'o') who |= 4;
            else if (*s == 'a') who |= 7;
            else break;
            who_given = true;
        }
        if (*s != '+' && *s != '-' && *s != '=')
            return -1;
        while (*s == '+' || *s == '-' || *s == '=') {
            if (ms->n == MAX_CLAUSES)
                return -1;
            struct clause *c = &ms->cl[ms->n];
            memset(c, 0, sizeof *c);
            c->who = who_given ? who : 0;
            c->op = *s++;
            if ((*s == 'u' || *s == 'g' || *s == 'o') &&
                !strchr("rwxXst", s[1] ? s[1] : ' ')) {
                c->copy = *s++;
            } else {
                for (;; s++) {
                    if      (*s == 'r') c->rwx |= 4;
                    else if (*s == 'w') c->rwx |= 2;
                    else if (*s == 'x') c->rwx |= 1;
                    else if (*s == 'X') c->X = true;
                    else if (*s == 's') c->s = true;
                    else if (*s == 't') c->t = true;
                    else break;
                }
            }
            ms->n++;
        }
        if (*s == ',')
            s++;
        else if (*s)
            return -1;
    }
    return ms->n ? 0 : -1;
}

static int parse_mode(const char *s, struct modespec *ms)
{
    memset(ms, 0, sizeof *ms);
    size_t len = strlen(s);
    if (len >= 1 && len <= 4 && strspn(s, "01234567") == len) {
        ms->octal = true;
        ms->omode = (mode_t)strtoul(s, NULL, 8);
        ms->set = true;
        return 0;
    }
    if (parse_symbolic(s, ms) != 0)
        return -1;
    ms->set = true;
    return 0;
}

/* Compute the new permission bits for one entry. */
static mode_t apply_spec(const struct modespec *ms, mode_t cur, bool isdir,
                         mode_t um)
{
    if (ms->octal)
        return ms->omode;

    mode_t m = cur & 07777;
    for (int i = 0; i < ms->n; i++) {
        const struct clause *c = &ms->cl[i];
        unsigned who = c->who ? c->who : 7;

        unsigned rwx = c->rwx;
        if (c->copy) {
            if      (c->copy == 'u') rwx = (m >> 6) & 7;
            else if (c->copy == 'g') rwx = (m >> 3) & 7;
            else                     rwx = m & 7;
        }
        if (c->X && (isdir || (m & 0111)))
            rwx |= 1;

        mode_t bits = 0;
        if (who & 1) bits |= (mode_t)rwx << 6;
        if (who & 2) bits |= (mode_t)rwx << 3;
        if (who & 4) bits |= (mode_t)rwx;
        if (c->s) {
            if ((who & 1)) bits |= S_ISUID;
            if ((who & 2)) bits |= S_ISGID;
        }
        if (c->t)
            bits |= S_ISVTX;

        /* with no explicit who, the umask limits the rwx bits (chmod(1)) */
        if (!c->who)
            bits = (bits & ~(mode_t)0777) | (bits & 0777 & ~um);

        switch (c->op) {
        case '+':
            m |= bits;
            break;
        case '-':
            m &= ~bits;
            break;
        case '=': {
            mode_t clear = 0;
            if (who & 1) clear |= S_IRWXU | S_ISUID;
            if (who & 2) clear |= S_IRWXG | S_ISGID;
            if (who & 4) clear |= S_IRWXO | S_ISVTX;
            /* like chmod(1), '=' preserves a directory's set-id bits
             * unless the clause mentions them explicitly */
            if (isdir && !c->s)
                clear &= ~(mode_t)(S_ISUID | S_ISGID);
            m = (m & ~clear) | bits;
            break;
        }
        }
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* configuration                                                        */
/* ------------------------------------------------------------------ */

static struct {
    struct modespec fspec;      /* mode for non-directories */
    struct modespec dspec;      /* mode for directories     */
    bool            set_uid, set_gid;
    uid_t           uid;
    gid_t           gid;
    char            uname[64], gname[64];
    bool            apply;
    int             nthreads;
    const char     *outpath;    /* NULL => stdout */
    bool            progress;
    mode_t          umask_val;
} g;

static _Atomic uint64_t n_files;      /* non-directory entries scanned  */
static _Atomic uint64_t n_dirs;       /* directories scanned            */
static _Atomic uint64_t n_changed;    /* changes pending or applied     */
static _Atomic uint64_t n_unchanged;  /* already compliant              */

static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* change calculation, application and CSV emission                     */
/* ------------------------------------------------------------------ */

#define CSV_HEADER \
    "path,type,old_mode,new_mode,old_owner,new_owner," \
    "old_group,new_group,result\n"

struct chg {
    bool   mode, own, grp;
    mode_t newm;
};

static struct chg calc_changes(const struct stat *st)
{
    struct chg c = { 0 };
    bool isdir = S_ISDIR(st->st_mode);
    const struct modespec *spec = isdir ? &g.dspec : &g.fspec;

    /* Linux cannot chmod a symlink itself; never dereference, so skip */
    if (spec->set && !S_ISLNK(st->st_mode)) {
        c.newm = apply_spec(spec, st->st_mode & 07777, isdir, g.umask_val);
        c.mode = c.newm != (st->st_mode & 07777);
    }
    c.own = g.set_uid && st->st_uid != g.uid;
    c.grp = g.set_gid && st->st_gid != g.gid;
    return c;
}

static void emit_row(p3m_outbuf *ob, const char *path, const struct stat *st,
                     const struct chg *c, const char *result)
{
    if (!p3m_ob_room(ob, 2 * strlen(path) + 512)) {
        p3m_note_error(path, "emit", ENAMETOOLONG);
        return;
    }
    char ufb[32], gfb[32];
    const char *oldu = p3m_uid_name(st->st_uid, ufb);
    const char *oldg = p3m_gid_name(st->st_gid, gfb);

    p3m_ob_csv(ob, path);
    p3m_ob_fmt(ob, ",%c,%04o,", p3m_dt_char(IFTODT(st->st_mode)),
               (unsigned)(st->st_mode & 07777));
    if (c->mode)
        p3m_ob_fmt(ob, "%04o", (unsigned)c->newm);
    p3m_ob_putc(ob, ',');
    p3m_ob_csv(ob, oldu);
    p3m_ob_putc(ob, ',');
    if (c->own)
        p3m_ob_csv(ob, g.uname);
    p3m_ob_putc(ob, ',');
    p3m_ob_csv(ob, oldg);
    p3m_ob_putc(ob, ',');
    if (c->grp)
        p3m_ob_csv(ob, g.gname);
    p3m_ob_fmt(ob, ",%s\n", result);
}

/*
 * Apply (or just report) the pending changes for one entry.
 * `dfd`+`name` address the entry for the syscalls; `path` is for output.
 */
static void do_change(int dfd, const char *name, const char *path,
                      const struct stat *st, const struct chg *c,
                      p3m_outbuf *ob)
{
    if (!g.apply) {
        atomic_fetch_add_explicit(&n_changed, 1, memory_order_relaxed);
        emit_row(ob, path, st, c, "pending");
        return;
    }

    char result[192];
    bool ok = true;

    /* chown first: it clears setuid/setgid, which a following chmod
     * then reinstates if the mode spec asks for them */
    if (c->own || c->grp) {
        if (fchownat(dfd, name, c->own ? g.uid : (uid_t)-1,
                     c->grp ? g.gid : (gid_t)-1, AT_SYMLINK_NOFOLLOW) != 0) {
            snprintf(result, sizeof result, "failed: chown: %s",
                     strerror(errno));
            p3m_note_error(path, "chown", errno);
            ok = false;
        }
    }
    if (ok && c->mode) {
        if (fchmodat(dfd, name, c->newm, 0) != 0) {
            snprintf(result, sizeof result, "failed: chmod: %s",
                     strerror(errno));
            p3m_note_error(path, "chmod", errno);
            ok = false;
        }
    }
    if (ok) {
        atomic_fetch_add_explicit(&n_changed, 1, memory_order_relaxed);
        emit_row(ob, path, st, c, "applied");
    } else {
        emit_row(ob, path, st, c, result);
    }
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

        struct stat st;
        if (fstatat(dfd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            char *fp = p3m_path_join(dirpath, dlen, nm);
            p3m_note_error(fp ? fp : nm, "stat", errno);
            free(fp);
            errno = 0;
            continue;
        }

        bool isdir = S_ISDIR(st.st_mode);
        if (isdir)
            atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);

        struct chg c = calc_changes(&st);
        bool any = c.mode || c.own || c.grp;
        if (!any)
            atomic_fetch_add_explicit(&n_unchanged, 1, memory_order_relaxed);

        if (any || isdir) {
            char *fp = p3m_path_join(dirpath, dlen, nm);
            if (!fp) {
                p3m_note_error(nm, "malloc", ENOMEM);
            } else {
                if (any)
                    do_change(dfd, nm, fp, &st, &c, ob);
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

#define PROG_LINES 8

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
    uint64_t chg   = atomic_load_explicit(&n_changed, memory_order_relaxed);
    uint64_t same  = atomic_load_explicit(&n_unchanged, memory_order_relaxed);
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

    char fv[32], dv[32], cv[32], uv[32], ev[32], rv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(chg, cv);
    p3m_fmt_u64(same, uv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_elapsed(p3m_mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-ch%s %s— parallel mode/ownership (%s)%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, g.apply ? "apply" : "dry-run", C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "stdout");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-10s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "action", C_RESET, g.apply ? "apply" : "dry-run");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-10s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-10s%s %s\n",
        C_DIM, "changes", C_RESET, cv, C_DIM, "unchanged", C_RESET, uv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-10s%s %s%s%s\n",
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "elapsed", C_RESET, el);
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
    uint64_t chg   = atomic_load(&n_changed);
    uint64_t same  = atomic_load(&n_unchanged);
    uint64_t errs  = atomic_load(&p3m_nerrors);

    char sv[32], cv[32], uv[32], ev[32], rv[32], el[32];
    p3m_fmt_u64(files + dirs, sv);
    p3m_fmt_u64(chg, cv);
    p3m_fmt_u64(same, uv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-ch%s complete — %s%s%s · %s scanned · "
            "%s change%s %s · %s unchanged · %s%s error%s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET,
            C_CYAN, g.apply ? "apply" : "dry-run", C_RESET, sv,
            cv, chg == 1 ? "" : "s", g.apply ? "applied" : "pending", uv,
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s)", el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
    if (!g.apply && chg)
        fprintf(stderr, "  %sre-run with --apply to make these changes%s\n",
                C_DIM, C_RESET);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-ch [OPTIONS] PATH...\n"
"\n"
"Parallel chmod + chown + chgrp: change mode, owner and group across\n"
"directory trees in a single recursive scan. The named PATHs themselves\n"
"are processed too. Without --apply this is a DRY RUN that only lists\n"
"the changes which would be made, as CSV.\n"
"\n"
"At least one of -m, -f, -d, -u, -g is required.\n"
"\n"
"Mode and ownership:\n"
"  -m, --mode MODE       mode for both files and directories\n"
"  -f, --file-mode MODE  mode for files only (overrides -m for files)\n"
"  -d, --dir-mode MODE   mode for directories only (overrides -m for dirs)\n"
"  -u, --owner USER      new owner: name or uid; USER:GROUP also accepted\n"
"  -g, --group GROUP     new group: name or gid\n"
"\n"
"MODE is octal (775, 0775, 2755) or symbolic like chmod(1):\n"
"[ugoa]([+-=][rwxXst]|[+-=][ugo])+, clauses separated by commas,\n"
"e.g. u+rwx,g-w  o=rx  a+X  g=u  +t\n"
"\n"
"Execution:\n"
"      --apply           make the changes (default is a dry run)\n"
"  -n, --dry-run         list changes without applying them (default)\n"
"  -j, --threads N       worker threads (default: number of online CPUs)\n"
"  -o, --output FILE     write CSV to FILE; a live progress display is shown\n"
"  -h, --help            show this help and exit\n"
"  -V, --version         show version and exit\n"
"\n"
"Symbolic links are never followed; modes are never changed on links\n"
"themselves (Linux does not support it) but ownership is (lchown).\n"
"Exit status: 0 success, 1 errors were encountered, 2 usage error.\n",
    to);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "mode",      required_argument, NULL, 'm' },
        { "file-mode", required_argument, NULL, 'f' },
        { "dir-mode",  required_argument, NULL, 'd' },
        { "owner",     required_argument, NULL, 'u' },
        { "group",     required_argument, NULL, 'g' },
        { "apply",     no_argument,       NULL, 1000 },
        { "dry-run",   no_argument,       NULL, 'n' },
        { "threads",   required_argument, NULL, 'j' },
        { "output",    required_argument, NULL, 'o' },
        { "help",      no_argument,       NULL, 'h' },
        { "version",   no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    struct modespec both = { 0 };
    const char *ownarg = NULL, *grparg = NULL;

    int c;
    while ((c = getopt_long(argc, argv, "m:f:d:u:g:nj:o:hV", lopts, NULL))
           != -1) {
        switch (c) {
        case 'm':
            if (parse_mode(optarg, &both) != 0) {
                fprintf(stderr, "p3m-ch: invalid mode '%s'\n", optarg);
                return 2;
            }
            break;
        case 'f':
            if (parse_mode(optarg, &g.fspec) != 0) {
                fprintf(stderr, "p3m-ch: invalid file mode '%s'\n", optarg);
                return 2;
            }
            break;
        case 'd':
            if (parse_mode(optarg, &g.dspec) != 0) {
                fprintf(stderr, "p3m-ch: invalid dir mode '%s'\n", optarg);
                return 2;
            }
            break;
        case 'u':
            ownarg = optarg;
            break;
        case 'g':
            grparg = optarg;
            break;
        case 1000:
            g.apply = true;
            break;
        case 'n':
            g.apply = false;
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-ch: invalid thread count '%s' "
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
            printf("p3m-ch %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_CH_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (both.set) {
        if (!g.fspec.set)
            g.fspec = both;
        if (!g.dspec.set)
            g.dspec = both;
    }

    /* -u user:group convenience form */
    char ownbuf[128];
    if (ownarg) {
        const char *colon = strchr(ownarg, ':');
        if (colon && !grparg) {
            size_t ul = (size_t)(colon - ownarg);
            if (ul >= sizeof ownbuf) {
                fprintf(stderr, "p3m-ch: owner name too long\n");
                return 2;
            }
            memcpy(ownbuf, ownarg, ul);
            ownbuf[ul] = '\0';
            ownarg = ownbuf;
            grparg = colon + 1;
        }
        if (p3m_resolve_user(ownarg, &g.uid) != 0) {
            fprintf(stderr, "p3m-ch: unknown user '%s'\n", ownarg);
            return 2;
        }
        g.set_uid = true;
        char fb[32];
        snprintf(g.uname, sizeof g.uname, "%s", p3m_uid_name(g.uid, fb));
    }
    if (grparg) {
        if (p3m_resolve_group(grparg, &g.gid) != 0) {
            fprintf(stderr, "p3m-ch: unknown group '%s'\n", grparg);
            return 2;
        }
        g.set_gid = true;
        char fb[32];
        snprintf(g.gname, sizeof g.gname, "%s", p3m_gid_name(g.gid, fb));
    }

    if (!g.fspec.set && !g.dspec.set && !g.set_uid && !g.set_gid) {
        fprintf(stderr, "p3m-ch: nothing to do "
                "(give at least one of -m, -f, -d, -u, -g)\n\n");
        usage(stderr);
        return 2;
    }
    if (optind >= argc) {
        fprintf(stderr, "p3m-ch: no paths given\n\n");
        usage(stderr);
        return 2;
    }

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    g.umask_val = umask(0);
    umask(g.umask_val);
    p3m_color = isatty(STDERR_FILENO);
    g.progress = g.outpath && isatty(STDERR_FILENO);

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-ch: cannot open '%s': %s\n",
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
        fprintf(stderr, "%sp3m-ch: dry run — no changes will be made "
                "(use --apply)%s\n", C_DIM, C_RESET);

    fputs(CSV_HEADER, out);

    /* seed the queue; the named paths themselves are processed too */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-ch: out of memory\n");
        return 2;
    }
    for (int i = optind; i < argc; i++) {
        char *root = strdup(argv[i]);
        if (!root)
            continue;
        size_t len = strlen(root);
        while (len > 1 && root[len - 1] == '/')   /* normalise trailing / */
            root[--len] = '\0';

        struct stat st;
        if (lstat(root, &st) != 0) {
            p3m_note_error(root, "stat", errno);
            free(root);
            continue;
        }
        bool isdir = S_ISDIR(st.st_mode);
        if (isdir)
            atomic_fetch_add(&n_dirs, 1);
        else
            atomic_fetch_add(&n_files, 1);

        struct chg ch = calc_changes(&st);
        if (ch.mode || ch.own || ch.grp)
            do_change(AT_FDCWD, root, root, &st, &ch, &rootob);
        else
            atomic_fetch_add(&n_unchanged, 1);

        if (isdir)
            p3m_stack_push_batch(&stk, &root, 1);
        else
            free(root);
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
        fprintf(stderr, "p3m-ch: out of memory\n");
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
            fprintf(stderr, "p3m-ch: could not create any worker threads\n");
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
        fprintf(stderr, "p3m-ch: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
