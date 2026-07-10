/*
 * p3m-find — parallel find
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Walks directory trees with a pool of worker threads, evaluating a
 * find(1)-style expression against every entry and printing the
 * matches. Supports the classic operators (! ( ) -a -o, implicit
 * and) and the common tests; stat(2) is called lazily, so searches
 * that only look at names run with no stat calls at all.
 *
 * Deliberately no -delete or -exec: matches are printed (use
 * --print0 | xargs -0, or feed the list to p3m-rm / p3m-ch).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "p3mcore.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P3M_FIND_VERSION "1.0.0"

static struct {
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
    bool         print0;
    long         maxdepth;      /* -1 = unlimited                     */
    long         mindepth;
} g = { .maxdepth = -1 };

static _Atomic uint64_t n_files;
static _Atomic uint64_t n_dirs;
static _Atomic uint64_t n_matched;

static p3m_stack stk;
static p3m_sink  sink;
static time_t    now_s;         /* reference time for -mtime & co.    */

/* ------------------------------------------------------------------ */
/* expression tree                                                      */
/* ------------------------------------------------------------------ */

enum {
    N_AND, N_OR, N_NOT,
    T_TRUE, T_FALSE,
    T_NAME, T_PATH, T_REGEX, T_TYPE, T_SIZE,
    T_TIME, T_NEWER, T_USER, T_GROUP, T_PERM, T_EMPTY
};

struct enode {
    int            op;
    struct enode  *l, *r;       /* operands for and/or/not            */
    /* test payload */
    const char    *pat;         /* name/path glob                     */
    bool           icase;
    regex_t        re;          /* regex (compiled once, ro after)    */
    unsigned       typemask;    /* one bit per DT_*                   */
    char           cmp;         /* '+', '-' or '='                    */
    uint64_t       uval;        /* size value / time value            */
    uint64_t       unit;        /* size unit bytes / time unit secs   */
    bool           exact_bytes; /* -size with 'c': no rounding        */
    int            tfield;      /* 0 mtime, 1 atime, 2 ctime          */
    struct timespec ref;        /* -newer reference                   */
    uid_t          uid;
    gid_t          gid;
    mode_t         perm;
    char           permcmp;     /* 'e' exact, '-' all, '/' any        */
};

static struct enode *expr;      /* root; NULL = match everything      */

static struct enode *node_alloc(int op)
{
    struct enode *n = calloc(1, sizeof *n);
    if (!n) {
        fprintf(stderr, "p3m-find: out of memory\n");
        exit(2);
    }
    n->op = op;
    return n;
}

/* ------------------------------------------------------------------ */
/* expression parser: or := and (-o and)* ; and := not (not)* ;        */
/* not := ! not | ( or ) | test                                        */
/* ------------------------------------------------------------------ */

static char **tokv;
static int    tokc, tokpos;

static const char *tok_peek(void) { return tokpos < tokc ? tokv[tokpos] : NULL; }
static const char *tok_next(void) { return tokpos < tokc ? tokv[tokpos++] : NULL; }

static void parse_die(const char *msg, const char *tok)
{
    fprintf(stderr, "p3m-find: %s%s%s%s\n", msg,
            tok ? " '" : "", tok ? tok : "", tok ? "'" : "");
    exit(2);
}

static const char *need_arg(const char *test)
{
    const char *a = tok_next();
    if (!a)
        parse_die("missing argument to", test);
    return a;
}

static struct enode *parse_or(void);

static int parse_typemask(const char *s, unsigned *mask)
{
    unsigned m = 0;
    for (; *s; s++) {
        switch (*s) {
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

/* [+-]N with a validated numeric body; returns cmp char */
static char parse_signed(const char *s, uint64_t *val, const char **rest)
{
    char cmp = '=';
    if (*s == '+' || *s == '-')
        cmp = *s++;
    if (!isdigit((unsigned char)*s))
        return 0;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno)
        return 0;
    *val = v;
    *rest = end;
    return cmp;
}

static struct enode *parse_primary(void)
{
    const char *t = tok_next();
    if (!t)
        parse_die("expected expression", NULL);

    if (!strcmp(t, "(")) {
        struct enode *n = parse_or();
        const char *close = tok_next();
        if (!close || strcmp(close, ")"))
            parse_die("missing ')'", NULL);
        return n;
    }
    if (!strcmp(t, "!") || !strcmp(t, "-not")) {
        struct enode *n = node_alloc(N_NOT);
        n->l = parse_primary();
        return n;
    }
    if (!strcmp(t, "-true"))
        return node_alloc(T_TRUE);
    if (!strcmp(t, "-false"))
        return node_alloc(T_FALSE);

    if (!strcmp(t, "-name") || !strcmp(t, "-iname") ||
        !strcmp(t, "-path") || !strcmp(t, "-ipath")) {
        struct enode *n = node_alloc(t[1] == 'i'
                                     ? (strstr(t, "name") ? T_NAME : T_PATH)
                                     : (t[1] == 'n' ? T_NAME : T_PATH));
        n->icase = t[1] == 'i';
        n->pat = need_arg(t);
        return n;
    }
    if (!strcmp(t, "-regex") || !strcmp(t, "-iregex")) {
        struct enode *n = node_alloc(T_REGEX);
        const char *pat = need_arg(t);
        /* anchored to the whole path, as with find */
        char *anch = malloc(strlen(pat) + 8);
        if (!anch)
            parse_die("out of memory", NULL);
        sprintf(anch, "^(%s)$", pat);
        int rc = regcomp(&n->re, anch,
                         REG_EXTENDED | REG_NOSUB |
                         (t[1] == 'i' ? REG_ICASE : 0));
        free(anch);
        if (rc != 0)
            parse_die("invalid regular expression", pat);
        return n;
    }
    if (!strcmp(t, "-type")) {
        struct enode *n = node_alloc(T_TYPE);
        if (parse_typemask(need_arg(t), &n->typemask) != 0)
            parse_die("invalid -type (valid: f d l b c p s)", NULL);
        return n;
    }
    if (!strcmp(t, "-size")) {
        struct enode *n = node_alloc(T_SIZE);
        const char *a = need_arg(t), *rest = NULL;
        n->cmp = parse_signed(a, &n->uval, &rest);
        if (!n->cmp)
            parse_die("invalid -size", a);
        switch (*rest) {
        case 'b': case '\0': n->unit = 512;                 break;
        case 'c': n->unit = 1;  n->exact_bytes = true;      break;
        case 'k': n->unit = 1024;                            break;
        case 'M': n->unit = (uint64_t)1024 * 1024;           break;
        case 'G': n->unit = (uint64_t)1024 * 1024 * 1024;    break;
        default:  parse_die("invalid -size unit (b c k M G)", a);
        }
        if (*rest && rest[1])
            parse_die("invalid -size", a);
        return n;
    }
    {
        static const struct { const char *kw; int field; long unit; } tt[] = {
            { "-mtime", 0, 86400 }, { "-mmin", 0, 60 },
            { "-atime", 1, 86400 }, { "-amin", 1, 60 },
            { "-ctime", 2, 86400 }, { "-cmin", 2, 60 },
        };
        for (size_t k = 0; k < sizeof tt / sizeof tt[0]; k++) {
            if (strcmp(t, tt[k].kw))
                continue;
            struct enode *n = node_alloc(T_TIME);
            const char *a = need_arg(t), *rest = NULL;
            n->cmp = parse_signed(a, &n->uval, &rest);
            if (!n->cmp || *rest)
                parse_die("invalid numeric argument to", t);
            n->tfield = tt[k].field;
            n->unit = (uint64_t)tt[k].unit;
            return n;
        }
    }
    if (!strcmp(t, "-newer")) {
        struct enode *n = node_alloc(T_NEWER);
        const char *f = need_arg(t);
        struct stat st;
        if (stat(f, &st) != 0) {
            fprintf(stderr, "p3m-find: -newer: cannot stat '%s': %s\n",
                    f, strerror(errno));
            exit(2);
        }
        n->ref = st.st_mtim;
        return n;
    }
    if (!strcmp(t, "-user")) {
        struct enode *n = node_alloc(T_USER);
        const char *u = need_arg(t);
        if (p3m_resolve_user(u, &n->uid) != 0)
            parse_die("unknown user", u);
        return n;
    }
    if (!strcmp(t, "-group")) {
        struct enode *n = node_alloc(T_GROUP);
        const char *gr = need_arg(t);
        if (p3m_resolve_group(gr, &n->gid) != 0)
            parse_die("unknown group", gr);
        return n;
    }
    if (!strcmp(t, "-perm")) {
        struct enode *n = node_alloc(T_PERM);
        const char *a = need_arg(t);
        n->permcmp = 'e';
        if (*a == '-' || *a == '/')
            n->permcmp = *a++;
        char *end;
        long v = strtol(a, &end, 8);
        if (*end || v < 0 || v > 07777 || end == a)
            parse_die("invalid -perm (octal, optionally -/ prefixed)", a);
        n->perm = (mode_t)v;
        return n;
    }
    if (!strcmp(t, "-empty"))
        return node_alloc(T_EMPTY);

    if (!strcmp(t, "-maxdepth") || !strcmp(t, "-mindepth")) {
        const char *a = need_arg(t);
        char *end;
        long v = strtol(a, &end, 10);
        if (*end || v < 0)
            parse_die("invalid depth", a);
        if (t[1] == 'm' && t[2] == 'a')
            g.maxdepth = v;
        else
            g.mindepth = v;
        return node_alloc(T_TRUE);
    }
    if (!strcmp(t, "-print0")) {         /* global output mode, not an
                                            ordered action as in find */
        g.print0 = true;
        return node_alloc(T_TRUE);
    }
    if (!strcmp(t, "-print"))
        return node_alloc(T_TRUE);

    parse_die("unknown test", t);
    return NULL;
}

static bool at_operand_start(const char *t)
{
    return t && strcmp(t, ")") && strcmp(t, "-o") && strcmp(t, "-or") &&
           strcmp(t, "-a") && strcmp(t, "-and");
}

static struct enode *parse_and(void)
{
    struct enode *l = parse_primary();
    for (;;) {
        const char *t = tok_peek();
        if (t && (!strcmp(t, "-a") || !strcmp(t, "-and"))) {
            tok_next();
            t = tok_peek();
            if (!at_operand_start(t))
                parse_die("expected expression after -a", NULL);
        } else if (!at_operand_start(t)) {
            return l;
        }
        struct enode *n = node_alloc(N_AND);
        n->l = l;
        n->r = parse_primary();
        l = n;
    }
}

static struct enode *parse_or(void)
{
    struct enode *l = parse_and();
    for (;;) {
        const char *t = tok_peek();
        if (!t || (strcmp(t, "-o") && strcmp(t, "-or")))
            return l;
        tok_next();
        struct enode *n = node_alloc(N_OR);
        n->l = l;
        n->r = parse_and();
        l = n;
    }
}

/* ------------------------------------------------------------------ */
/* expression evaluation, with lazy stat and lazy path building        */
/* ------------------------------------------------------------------ */

struct ectx {
    int            dirfd;       /* AT_FDCWD for root arguments        */
    const char    *name;        /* entry name (or full path for root) */
    const char    *dirpath;     /* parent path, NULL for roots        */
    size_t         dlen;
    char          *path;        /* built on demand; freed by caller   */
    unsigned char  dt;
    struct stat    st;
    bool           have_st, st_fail;
};

static const char *ctx_path(struct ectx *c)
{
    if (c->path)
        return c->path;
    if (!c->dirpath)
        return c->name;
    c->path = p3m_path_join(c->dirpath, c->dlen, c->name);
    return c->path ? c->path : c->name;
}

static bool ctx_stat(struct ectx *c)
{
    if (c->have_st)
        return true;
    if (c->st_fail)
        return false;
    if (fstatat(c->dirfd, c->name, &c->st, AT_SYMLINK_NOFOLLOW) != 0) {
        p3m_note_error(ctx_path(c), "stat", errno);
        c->st_fail = true;
        return false;
    }
    c->have_st = true;
    if (c->dt == DT_UNKNOWN)
        c->dt = IFTODT(c->st.st_mode);
    return true;
}

static bool cmp_u64(char cmp, uint64_t v, uint64_t n)
{
    return cmp == '+' ? v > n : cmp == '-' ? v < n : v == n;
}

static bool eval(const struct enode *n, struct ectx *c)
{
    switch (n->op) {
    case N_AND:  return eval(n->l, c) && eval(n->r, c);
    case N_OR:   return eval(n->l, c) || eval(n->r, c);
    case N_NOT:  return !eval(n->l, c);
    case T_TRUE: return true;
    case T_FALSE:return false;

    case T_NAME: {
        const char *base = c->dirpath ? c->name : strrchr(c->name, '/');
        base = c->dirpath ? c->name : (base ? base + 1 : c->name);
        return fnmatch(n->pat, base, n->icase ? FNM_CASEFOLD : 0) == 0;
    }
    case T_PATH:
        return fnmatch(n->pat, ctx_path(c),
                       n->icase ? FNM_CASEFOLD : 0) == 0;
    case T_REGEX:
        return regexec(&n->re, ctx_path(c), 0, NULL, 0) == 0;

    case T_TYPE:
        if (c->dt == DT_UNKNOWN && !ctx_stat(c))
            return false;
        return c->dt < 32 && (n->typemask & (1u << c->dt));

    case T_SIZE: {
        if (!ctx_stat(c))
            return false;
        uint64_t v = n->exact_bytes
                         ? (uint64_t)c->st.st_size
                         : ((uint64_t)c->st.st_size + n->unit - 1) / n->unit;
        return cmp_u64(n->cmp, v, n->uval);
    }
    case T_TIME: {
        if (!ctx_stat(c))
            return false;
        const struct timespec *ts =
            n->tfield == 0 ? &c->st.st_mtim :
            n->tfield == 1 ? &c->st.st_atim : &c->st.st_ctim;
        int64_t age = (int64_t)now_s - (int64_t)ts->tv_sec;
        int64_t d = age < 0 ? -1 : age / (int64_t)n->unit;
        if (d < 0)                       /* future timestamp */
            return n->cmp == '-';
        return cmp_u64(n->cmp, (uint64_t)d, n->uval);
    }
    case T_NEWER:
        if (!ctx_stat(c))
            return false;
        return c->st.st_mtim.tv_sec > n->ref.tv_sec ||
               (c->st.st_mtim.tv_sec == n->ref.tv_sec &&
                c->st.st_mtim.tv_nsec > n->ref.tv_nsec);

    case T_USER:
        return ctx_stat(c) && c->st.st_uid == n->uid;
    case T_GROUP:
        return ctx_stat(c) && c->st.st_gid == n->gid;

    case T_PERM:
        if (!ctx_stat(c))
            return false;
        switch (n->permcmp) {
        case '-': return (c->st.st_mode & n->perm) == n->perm;
        case '/': return n->perm == 0 ||
                         (c->st.st_mode & n->perm) != 0;
        default:  return (c->st.st_mode & 07777) == n->perm;
        }

    case T_EMPTY: {
        if (c->dt == DT_UNKNOWN && !ctx_stat(c))
            return false;
        if (c->dt == DT_REG)
            return ctx_stat(c) && c->st.st_size == 0;
        if (c->dt != DT_DIR)
            return false;
        int fd = openat(c->dirfd, c->name,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return false;
        DIR *d = fdopendir(fd);
        if (!d) {
            close(fd);
            return false;
        }
        bool empty = true;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
            if (nm[0] == '.' &&
                (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
            empty = false;
            break;
        }
        closedir(d);
        return empty;
    }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* output                                                               */
/* ------------------------------------------------------------------ */

static void emit_match(p3m_outbuf *ob, const char *path)
{
    atomic_fetch_add_explicit(&n_matched, 1, memory_order_relaxed);
    if (g.suppress)
        return;
    if (!p3m_ob_room(ob, 2 * strlen(path) + 8)) {
        p3m_note_error(path, "emit", ENAMETOOLONG);
        return;
    }
    if (g.print0) {
        p3m_ob_puts(ob, path);
        p3m_ob_putc(ob, '\0');
    } else {
        p3m_ob_csv(ob, path);
        p3m_ob_putc(ob, '\n');
    }
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static char *pack_item(const char *path, long depth)
{
    size_t pl = strlen(path);
    char *it = malloc(sizeof(uint32_t) + pl + 1);
    if (!it)
        return NULL;
    uint32_t d = (uint32_t)depth;
    memcpy(it, &d, sizeof d);
    memcpy(it + sizeof d, path, pl + 1);
    return it;
}

static void scan_dir(const char *dirpath, long depth, p3m_outbuf *ob)
{
    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(dirpath);

    int fd = open(dirpath, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        p3m_note_error(dirpath, "open", errno);
        return;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        int e = errno;
        close(fd);
        p3m_note_error(dirpath, "fdopendir", e);
        return;
    }
    size_t dlen = strlen(dirpath);
    long edepth = depth + 1;          /* depth of the entries in here */

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

        struct ectx c = {
            .dirfd = fd, .name = nm, .dirpath = dirpath, .dlen = dlen,
            .dt = de->d_type,
        };
        if (c.dt == DT_UNKNOWN && !ctx_stat(&c)) {
            free(c.path);
            errno = 0;
            continue;
        }
        bool isdir = c.dt == DT_DIR;
        if (isdir)
            atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);

        if (edepth >= g.mindepth &&
            (!expr || eval(expr, &c)))
            emit_match(ob, ctx_path(&c));

        if (isdir && (g.maxdepth < 0 || edepth < g.maxdepth)) {
            char *fp = c.path ? c.path : p3m_path_join(dirpath, dlen, nm);
            c.path = NULL;            /* ownership moves to the queue */
            if (!fp) {
                p3m_note_error(nm, "malloc", ENOMEM);
            } else {
                char *it = pack_item(fp, edepth);
                free(fp);
                if (it) {
                    if (nsub == csub) {
                        csub = csub ? csub * 2 : 32;
                        char **ns = realloc(subs, csub * sizeof *ns);
                        if (!ns) {
                            p3m_note_error(nm, "queue", ENOMEM);
                            free(it);
                            it = NULL;
                        } else {
                            subs = ns;
                        }
                    }
                    if (it)
                        subs[nsub++] = it;
                } else {
                    p3m_note_error(nm, "queue", ENOMEM);
                }
            }
        }
        free(c.path);
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
    char *item;
    while ((item = p3m_stack_pop(&stk)) != NULL) {
        uint32_t depth;
        memcpy(&depth, item, sizeof depth);
        scan_dir(item + sizeof depth, (long)depth, &ob);
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
    uint64_t match = atomic_load_explicit(&n_matched, memory_order_relaxed);

    char cur[PATH_MAX];
    p3m_get_current(cur, sizeof cur);
    char ptr[512];
    int pmax = p3m_term_width() - 13;
    if (pmax > 500)
        pmax = 500;
    if (pmax < 20)
        pmax = 20;
    p3m_trunc_left(cur, (size_t)pmax, ptr, sizeof ptr);

    char fv[32], dv[32], ev[32], rv[32], mv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_u64(match, mv);
    p3m_fmt_elapsed(p3m_mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-find%s %s— parallel search%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "none (-q)");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s%s%s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "matched", C_RESET, C_BOLD, mv, C_RESET);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s\n",
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
    uint64_t errs  = atomic_load(&p3m_nerrors);
    uint64_t match = atomic_load(&n_matched);

    char fv[32], dv[32], ev[32], rv[32], mv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_u64(match, mv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-find%s complete — %s%s matched%s · "
            "%s files · %s dirs scanned · %s%s error%s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET,
            C_BOLD, mv, C_RESET, fv, dv,
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "");
    fprintf(stderr, "  %s (%s items/s)", el, rv);
    if (g.outpath)
        fprintf(stderr, " → %s", g.outpath);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* argument parsing / main                                              */
/* ------------------------------------------------------------------ */

static void usage(FILE *to)
{
    fputs(
"Usage: p3m-find [P3M-OPTIONS] [PATH...] [EXPRESSION]\n"
"\n"
"Search directory trees in parallel with a find(1)-style expression.\n"
"With no PATH, the current directory is searched; with no EXPRESSION,\n"
"everything matches. Matches print as CSV (a 'path' header + one row\n"
"per match) or NUL-separated with -print0.\n"
"\n"
"p3m options (before any PATH):\n"
"  -j, --threads N     worker threads (default: number of online CPUs)\n"
"  -o, --output FILE   write matches to FILE; live progress is shown\n"
"  -q, --quiet         suppress the listing; progress and the summary\n"
"                      (with the match count) are still shown\n"
"  -h, --help          show this help and exit\n"
"  -V, --version       show version and exit\n"
"\n"
"Tests:\n"
"  -name P    -iname P    glob on the entry name (case-insensitive: i*)\n"
"  -path P    -ipath P    glob on the full path\n"
"  -regex P   -iregex P   POSIX extended regex on the full path (anchored)\n"
"  -type T[,T...]         f d l b c p s (combinable: -type f,l)\n"
"  -size [+-]N[bckMG]     rounded up to the unit, as with find (default b)\n"
"  -mtime/-atime/-ctime [+-]N   age in 24h units\n"
"  -mmin/-amin/-cmin    [+-]N   age in minutes\n"
"  -newer FILE            modified more recently than FILE\n"
"  -user U  -group G      owner / group (name or numeric id)\n"
"  -perm [-|/]OCTAL       exact / all bits / any bit\n"
"  -empty                 empty regular file or directory\n"
"  -mindepth N  -maxdepth N     depth limits (PATH itself is depth 0)\n"
"  -print0                NUL-separated output for xargs -0\n"
"\n"
"Operators (highest precedence first):\n"
"  ( EXPR )   ! EXPR / -not EXPR   EXPR EXPR / -a   EXPR -o EXPR\n"
"\n"
"There is deliberately no -delete or -exec: pipe matches to xargs -0\n"
"or feed them to p3m-rm / p3m-ch. Symbolic links are never followed.\n",
    to);
}

static bool is_expr_start(const char *t)
{
    return (t[0] == '-' && t[1] != '\0') || !strcmp(t, "(") ||
           !strcmp(t, "!");
}

int main(int argc, char **argv)
{
    /* ---- p3m options, then paths, then the expression ---------------- */
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strncmp(a, "-j", 2) || !strncmp(a, "--threads", 9)) {
            const char *v = NULL;
            if (a[1] == 'j' && a[2])
                v = a + 2;
            else if (a[1] == '-' && a[9] == '=')
                v = a + 10;
            else if (i + 1 < argc)
                v = argv[++i];
            char *end;
            long n = v ? strtol(v, &end, 10) : 0;
            if (!v || *end || n < 1 || n > 512) {
                fprintf(stderr, "p3m-find: invalid thread count "
                        "(expected 1-512)\n");
                return 2;
            }
            g.nthreads = (int)n;
        } else if (!strncmp(a, "-o", 2) || !strncmp(a, "--output", 8)) {
            if (a[1] == 'o' && a[2])
                g.outpath = a + 2;
            else if (a[1] == '-' && a[8] == '=')
                g.outpath = a + 9;
            else if (i + 1 < argc)
                g.outpath = argv[++i];
            else {
                fprintf(stderr, "p3m-find: -o requires a file\n");
                return 2;
            }
        } else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
            g.quiet = true;
        } else if (!strcmp(a, "--print0")) {
            g.print0 = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("p3m-find %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_FIND_VERSION);
            return 0;
        } else {
            break;
        }
    }

    /* paths until the expression begins */
    int pathstart = i;
    while (i < argc && !is_expr_start(argv[i]))
        i++;
    int npaths = i - pathstart;

    /* expression */
    tokv = &argv[i];
    tokc = argc - i;
    tokpos = 0;
    if (tokc > 0) {
        expr = parse_or();
        if (tokpos != tokc)
            parse_die("unexpected token", tokv[tokpos]);
    }

    if (g.nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        g.nthreads = (n > 0) ? (int)n : 4;
    }
    p3m_color = isatty(STDERR_FILENO);
    g.suppress = g.quiet && !g.outpath;
    g.progress = (g.outpath || g.quiet) && isatty(STDERR_FILENO);
    now_s = time(NULL);

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-find: cannot open '%s': %s\n",
                    g.outpath, strerror(errno));
            return 2;
        }
    } else {
        out = stdout;
    }
    static char outvbuf[1 << 20];
    setvbuf(out, outvbuf, _IOFBF, sizeof outvbuf);
    p3m_sink_init(&sink, out);

    if (!g.suppress && !g.print0)
        fputs("path\n", out);

    /* ---- seed with the paths ----------------------------------------- */
    p3m_stack_init(&stk, g.nthreads);
    p3m_outbuf rootob;
    if (p3m_ob_init(&rootob, &sink) != 0) {
        fprintf(stderr, "p3m-find: out of memory\n");
        return 2;
    }
    static char *dot = ".";
    char **roots = npaths ? &argv[pathstart] : &dot;
    int nroots = npaths ? npaths : 1;
    for (int r = 0; r < nroots; r++) {
        char *root = strdup(roots[r]);
        if (!root)
            continue;
        size_t len = strlen(root);
        while (len > 1 && root[len - 1] == '/')
            root[--len] = '\0';

        struct ectx c = {
            .dirfd = AT_FDCWD, .name = root, .dt = DT_UNKNOWN,
        };
        if (!ctx_stat(&c)) {
            free(root);
            continue;
        }
        bool isdir = c.dt == DT_DIR;
        if (isdir)
            atomic_fetch_add(&n_dirs, 1);
        else
            atomic_fetch_add(&n_files, 1);

        if (g.mindepth <= 0 && (!expr || eval(expr, &c)))
            emit_match(&rootob, root);

        if (isdir && g.maxdepth != 0) {
            char *it = pack_item(root, 0);
            if (it)
                p3m_stack_push_batch(&stk, &it, 1);
            else
                p3m_note_error(root, "queue", ENOMEM);
        }
        free(root);
    }
    p3m_ob_flush(&rootob);
    p3m_ob_free(&rootob);

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

    pthread_t *tids = calloc((size_t)g.nthreads, sizeof *tids);
    if (!tids) {
        fprintf(stderr, "p3m-find: out of memory\n");
        return 2;
    }
    int started = 0;
    for (int k = 0; k < g.nthreads; k++) {
        if (pthread_create(&tids[k], NULL, worker, NULL) != 0)
            break;
        started++;
    }
    if (started < g.nthreads) {
        if (started == 0) {
            fprintf(stderr, "p3m-find: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int k = 0; k < started; k++)
        pthread_join(tids[k], NULL);
    free(tids);

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();

    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-find: %swrite error%s on %s — output is "
                "incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    if (g.outpath || g.quiet)
        print_summary(elapsed);
    p3m_print_errors();
    p3m_stack_destroy(&stk);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
