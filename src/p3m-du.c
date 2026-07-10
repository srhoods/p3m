/*
 * p3m-du — parallel disk usage
 *
 * Part of p3m: Parallel POSIX Permission Manager
 *
 * Estimates file space usage like du(1), but walks the tree with a pool
 * of worker threads. Supports the common du options (-s -c -d -h --si
 * -X --exclude -B -b) and the usual p3m output, progress and quiet
 * options.
 *
 * NOTE: unlike the other p3m tools, -h means --human-readable (matching
 * du); use --help for usage.
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
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P3M_DU_VERSION "1.0.0"

enum sizefmt { FMT_BLOCKS, FMT_HUMAN, FMT_SI };

static struct {
    enum sizefmt fmt;
    uint64_t     blocksize;   /* FMT_BLOCKS: bytes per printed unit      */
    bool         apparent;    /* sum st_size instead of st_blocks*512    */
    bool         total;       /* -c: append a grand-total row            */
    int          max_depth;   /* print rows down to this depth only      */
    const char  *units_str;   /* what the progress display shows         */
    int          nthreads;
    const char  *outpath;
    bool         quiet;
    bool         suppress;
    bool         progress;
} g = { .fmt = FMT_BLOCKS, .blocksize = 1024, .max_depth = INT_MAX,
        .units_str = "1K" };

static _Atomic uint64_t n_files;   /* non-directory entries counted      */
static _Atomic uint64_t n_dirs;    /* directories counted                */
static _Atomic uint64_t n_bytes;   /* bytes accumulated so far           */

static p3m_stack stk;
static p3m_sink  sink;

/* ------------------------------------------------------------------ */
/* directory node store                                                 */
/*                                                                      */
/* One node per directory (plus one per non-directory argument). Sizes  */
/* accumulate into the owning node during the walk and are rolled up    */
/* into parents afterwards. Nodes live in fixed-size chunks so a node,  */
/* once created, never moves; a child's index is always greater than    */
/* its parent's, so a single reverse sweep aggregates the whole forest. */
/* ------------------------------------------------------------------ */

struct dnode {
    char    *path;
    size_t   parent;          /* index of parent, SIZE_MAX for roots    */
    int      depth;           /* 0 = a command-line argument            */
    uint64_t bytes;           /* own contribution; children added later */
    uint64_t total;           /* filled by the aggregation sweep        */
};

#define NODE_CHUNK_SHIFT 13
#define NODE_CHUNK_SIZE  ((size_t)1 << NODE_CHUNK_SHIFT)
#define NODE_MAX_CHUNKS  (1 << 18)

static struct dnode   *node_chunks[NODE_MAX_CHUNKS];
static size_t          n_nodes;
static pthread_mutex_t node_mu = PTHREAD_MUTEX_INITIALIZER;

static inline struct dnode *node_get(size_t i)
{
    return &node_chunks[i >> NODE_CHUNK_SHIFT][i & (NODE_CHUNK_SIZE - 1)];
}

/* takes ownership of path; returns SIZE_MAX on allocation failure */
static size_t node_new(char *path, size_t parent, int depth, uint64_t own)
{
    pthread_mutex_lock(&node_mu);
    size_t i = n_nodes;
    size_t c = i >> NODE_CHUNK_SHIFT;
    if (c >= NODE_MAX_CHUNKS) {
        pthread_mutex_unlock(&node_mu);
        free(path);
        return SIZE_MAX;
    }
    if (!node_chunks[c]) {
        node_chunks[c] = malloc(NODE_CHUNK_SIZE * sizeof(struct dnode));
        if (!node_chunks[c]) {
            pthread_mutex_unlock(&node_mu);
            free(path);
            return SIZE_MAX;
        }
    }
    n_nodes = i + 1;
    pthread_mutex_unlock(&node_mu);

    struct dnode *n = node_get(i);
    n->path   = path;
    n->parent = parent;
    n->depth  = depth;
    n->bytes  = own;
    n->total  = 0;
    return i;
}

/* ------------------------------------------------------------------ */
/* hard-link tracking                                                   */
/*                                                                      */
/* Like du, a file with several hard links is counted once per run. A   */
/* (dev, ino) pair is looked up only for files with st_nlink > 1, so    */
/* the shared table sees little traffic on typical trees.               */
/* ------------------------------------------------------------------ */

struct hlkey { dev_t dev; ino_t ino; };   /* ino 0 marks an empty slot */

static struct hlkey   *hl_tab;
static size_t          hl_cap, hl_n;
static pthread_mutex_t hl_mu = PTHREAD_MUTEX_INITIALIZER;

static inline size_t hl_hash(dev_t dev, ino_t ino, size_t cap)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ull
               ^ (uint64_t)dev * 0xC2B2AE3D27D4EB4Full;
    return (size_t)(h & (cap - 1));
}

/* returns true if (dev, ino) was already recorded; records it if not */
static bool hl_seen(dev_t dev, ino_t ino)
{
    pthread_mutex_lock(&hl_mu);
    if (hl_n * 10 >= hl_cap * 7) {              /* grow at 70% load */
        size_t ncap = hl_cap ? hl_cap * 2 : 4096;
        struct hlkey *nt = calloc(ncap, sizeof *nt);
        if (nt) {
            for (size_t i = 0; i < hl_cap; i++) {
                if (!hl_tab[i].ino)
                    continue;
                size_t j = hl_hash(hl_tab[i].dev, hl_tab[i].ino, ncap);
                while (nt[j].ino)
                    j = (j + 1) & (ncap - 1);
                nt[j] = hl_tab[i];
            }
            free(hl_tab);
            hl_tab = nt;
            hl_cap = ncap;
        } else if (!hl_cap) {
            pthread_mutex_unlock(&hl_mu);
            p3m_note_error("hard-link table", "malloc", ENOMEM);
            return false;                       /* fail open: count it */
        }
    }
    size_t j = hl_hash(dev, ino, hl_cap);
    while (hl_tab[j].ino) {
        if (hl_tab[j].ino == ino && hl_tab[j].dev == dev) {
            pthread_mutex_unlock(&hl_mu);
            return true;
        }
        j = (j + 1) & (hl_cap - 1);
    }
    hl_tab[j].dev = dev;
    hl_tab[j].ino = ino;
    hl_n++;
    pthread_mutex_unlock(&hl_mu);
    return false;
}

/* ------------------------------------------------------------------ */
/* exclusion patterns (--exclude, -X)                                   */
/* ------------------------------------------------------------------ */

static char **excl;
static size_t n_excl, c_excl;

static int add_exclude(const char *pat)
{
    if (n_excl == c_excl) {
        size_t nc = c_excl ? c_excl * 2 : 16;
        char **ne = realloc(excl, nc * sizeof *ne);
        if (!ne)
            return -1;
        excl = ne;
        c_excl = nc;
    }
    excl[n_excl] = strdup(pat);
    if (!excl[n_excl])
        return -1;
    n_excl++;
    return 0;
}

static int load_exclude_file(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        if (add_exclude(line) != 0) {
            free(line);
            fclose(f);
            errno = ENOMEM;
            return -1;
        }
    }
    free(line);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* a pattern excludes an entry if it matches its name or its full path */
static bool excluded(const char *name, const char *path)
{
    for (size_t i = 0; i < n_excl; i++)
        if (fnmatch(excl[i], name, 0) == 0 ||
            (path && fnmatch(excl[i], path, 0) == 0))
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* size formatting                                                      */
/* ------------------------------------------------------------------ */

/*
 * du-style human formatting with ceiling rounding: one decimal below
 * 10, integers above, promoting to the next unit when rounding reaches
 * the base (1023.1K -> 1.0M).
 */
static void fmt_human(uint64_t bytes, uint64_t base, const char *units,
                      char out[32])
{
    if (bytes < base) {
        snprintf(out, 32, "%" PRIu64, bytes);
        return;
    }
    uint64_t pow = base;
    int u = 0;
    while (u < 5 && bytes / base >= pow)
        pow *= base, u++;

    unsigned __int128 tenths =
        ((unsigned __int128)bytes * 10 + pow - 1) / pow;
    if (tenths < 100) {
        snprintf(out, 32, "%u.%u%c",
                 (unsigned)(tenths / 10), (unsigned)(tenths % 10), units[u]);
        return;
    }
    uint64_t whole = (bytes + pow - 1) / pow;
    if (whole >= base && u < 5) {
        u++;
        snprintf(out, 32, "1.0%c", units[u]);
        return;
    }
    snprintf(out, 32, "%" PRIu64 "%c", whole, units[u]);
}

static void fmt_du_size(uint64_t bytes, char out[32])
{
    switch (g.fmt) {
    case FMT_HUMAN:
        fmt_human(bytes, 1024, "KMGTPE", out);
        break;
    case FMT_SI:
        fmt_human(bytes, 1000, "kMGTPE", out);
        break;
    default:
        snprintf(out, 32, "%" PRIu64,
                 (bytes + g.blocksize - 1) / g.blocksize);
        break;
    }
}

/* SIZE argument for -B: [N]UNIT where UNIT is K,M,G,T,P,E (1024-based),
 * KB,MB,... (1000-based) or KiB,MiB,... (1024-based) */
static int parse_blocksize(const char *s, uint64_t *out)
{
    const char *p = s;
    uint64_t n = 1;
    if (isdigit((unsigned char)*p)) {
        char *end;
        errno = 0;
        unsigned long long v = strtoull(p, &end, 10);
        if (errno || v == 0)
            return -1;
        n = v;
        p = end;
    }
    uint64_t mult = 1;
    if (*p) {
        const char *letters = "KMGTPE";
        const char *hit = strchr(letters, toupper((unsigned char)*p));
        if (!hit)
            return -1;
        int exp = (int)(hit - letters) + 1;
        p++;
        uint64_t base = 1024;
        if (p[0] == 'B' && p[1] == '\0')
            base = 1000, p++;
        else if (p[0] == 'i' && p[1] == 'B' && p[2] == '\0')
            p += 2;
        for (int i = 0; i < exp; i++) {
            if (mult > UINT64_MAX / base)
                return -1;
            mult *= base;
        }
    }
    if (*p || n > UINT64_MAX / mult)
        return -1;
    *out = n * mult;
    return 0;
}

/* ------------------------------------------------------------------ */
/* directory scanning                                                   */
/* ------------------------------------------------------------------ */

static inline uint64_t entry_size(const struct stat *st)
{
    return g.apparent ? (uint64_t)st->st_size
                      : (uint64_t)st->st_blocks * 512;
}

static char *idx_str(size_t i)
{
    char b[24];
    snprintf(b, sizeof b, "%zu", i);
    return strdup(b);
}

static void scan_dir(size_t idx, p3m_outbuf *ob)
{
    (void)ob;
    struct dnode *node = node_get(idx);
    const char *dirpath = node->path;

    if (atomic_load_explicit(&sink.failed, memory_order_relaxed))
        return;                       /* abort: let the queue drain */

    p3m_set_current(dirpath);

    /* O_NOFOLLOW: a directory swapped for a symlink mid-run is refused */
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

    uint64_t local = 0;
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
            char *fp = p3m_path_join(dirpath, dlen, nm);
            p3m_note_error(fp ? fp : nm, "stat", errno);
            free(fp);
            errno = 0;
            continue;
        }
        bool isdir = S_ISDIR(st.st_mode);

        char *fp = NULL;
        if (isdir || n_excl) {
            fp = p3m_path_join(dirpath, dlen, nm);
            if (!fp) {
                p3m_note_error(nm, "malloc", ENOMEM);
                errno = 0;
                continue;
            }
        }
        if (n_excl && excluded(nm, fp)) {   /* not counted, not entered */
            free(fp);
            errno = 0;
            continue;
        }

        uint64_t sz = entry_size(&st);
        if (isdir) {
            size_t child = node_new(fp, idx, node->depth + 1, sz);
            if (child == SIZE_MAX) {
                p3m_note_error(nm, "malloc", ENOMEM);
                errno = 0;
                continue;
            }
            char *is = idx_str(child);
            if (is) {
                if (nsub == csub) {
                    csub = csub ? csub * 2 : 32;
                    char **ns = realloc(subs, csub * sizeof *ns);
                    if (!ns) {
                        p3m_note_error(node_get(child)->path, "queue", ENOMEM);
                        free(is);
                        errno = 0;
                        continue;
                    }
                    subs = ns;
                }
                subs[nsub++] = is;
            } else {
                p3m_note_error(node_get(child)->path, "queue", ENOMEM);
            }
            atomic_fetch_add_explicit(&n_dirs, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&n_bytes, sz, memory_order_relaxed);
        } else {
            if (st.st_nlink > 1 && hl_seen(st.st_dev, st.st_ino))
                sz = 0;                 /* hard link seen before: du rule */
            local += sz;
            free(fp);
            atomic_fetch_add_explicit(&n_files, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&n_bytes, sz, memory_order_relaxed);
        }
        errno = 0;
    }
    if (errno)
        p3m_note_error(dirpath, "readdir", errno);
    closedir(d);

    node->bytes += local;   /* only this thread scans this directory */

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
        scan_dir((size_t)strtoull(item, NULL, 10), &ob);
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

    char fv[32], dv[32], ev[32], rv[32], sv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64((uint64_t)(rate + 0.5), rv);
    p3m_fmt_size(bytes, sv);
    p3m_fmt_elapsed(p3m_mono_now() - t_start, el);

    char ratestr[48];
    snprintf(ratestr, sizeof ratestr, "%s items/s", rv);

    char buf[4096];
    size_t off = 0;
#define ADD(...) off += (size_t)snprintf(buf + off, sizeof buf - off, __VA_ARGS__)
    ADD("\x1b[K%s%s%s %sp3m-du%s %s— parallel disk usage%s\n",
        C_CYAN, p3m_spinner[frame % 10], C_RESET, C_BOLD, C_RESET,
        C_DIM, C_RESET);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "path", C_RESET, ptr);
    ADD("\x1b[K  %s%-9s%s %s\n", C_DIM, "output", C_RESET,
        g.outpath ? g.outpath : "none (-q)");
    ADD("\x1b[K  %s%-9s%s %-14d %s%-8s%s %s\n",
        C_DIM, "threads", C_RESET, g.nthreads,
        C_DIM, "units", C_RESET, g.units_str);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "files", C_RESET, fv, C_DIM, "dirs", C_RESET, dv);
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s%s%s\n",
        C_DIM, "rate", C_RESET, ratestr, C_DIM, "errors", C_RESET,
        errs ? C_RED : "", ev, errs ? C_RESET : "");
    ADD("\x1b[K  %s%-9s%s %-14s %s%-8s%s %s\n",
        C_DIM, "size", C_RESET, sv, C_DIM, "elapsed", C_RESET, el);
#undef ADD
    fwrite(buf, 1, off, stderr);
}

/* ------------------------------------------------------------------ */
/* summary                                                              */
/* ------------------------------------------------------------------ */

static void print_summary(double elapsed, uint64_t grand, uint64_t rows)
{
    uint64_t files = atomic_load(&n_files);
    uint64_t dirs  = atomic_load(&n_dirs);
    uint64_t errs  = atomic_load(&p3m_nerrors);

    char fv[32], dv[32], ev[32], rv[32], sv[32], nv[32], el[32];
    p3m_fmt_u64(files, fv);
    p3m_fmt_u64(dirs, dv);
    p3m_fmt_u64(errs, ev);
    p3m_fmt_u64(elapsed > 0 ? (uint64_t)((double)(files + dirs) / elapsed) : 0,
                rv);
    p3m_fmt_size(grand, sv);
    p3m_fmt_u64(rows, nv);
    p3m_fmt_elapsed(elapsed, el);

    fprintf(stderr,
            "%s✓%s %sp3m-du%s complete — %s files · %s dirs · %s%s error%s%s"
            " · %s%s\n",
            C_GREEN, C_RESET, C_BOLD, C_RESET, fv, dv,
            errs ? C_RED : "", ev, errs == 1 ? "" : "s",
            errs ? C_RESET : "",
            sv, g.apparent ? " (apparent)" : "");
    fprintf(stderr, "  %s rows in %s (%s items/s)", nv, el, rv);
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
"Usage: p3m-du [OPTIONS] [PATH...]\n"
"\n"
"Summarise disk usage of directory trees in parallel, emitting CSV\n"
"(size,path — one row per directory, deepest first). With no PATH, the\n"
"current directory is scanned. Sizes are the recursive disk usage in\n"
"1K blocks by default, as with du(1).\n"
"\n"
"du options:\n"
"  -s, --summarize        only print each PATH argument (same as -d 0)\n"
"  -c, --total            append a grand-total row\n"
"  -d, --max-depth N      print directories at most N levels below each\n"
"                         PATH (the walk and totals always cover everything)\n"
"  -h, --human-readable   sizes like 1.0K, 23M, 2.3G (powers of 1024)\n"
"      --si               like -h, but powers of 1000 (1.0k, 23M, 2.3G)\n"
"  -B, --block-size SIZE  print sizes in SIZE-byte blocks, rounded up;\n"
"                         SIZE accepts suffixes: K,M,G,... (1024-based),\n"
"                         KB,MB,... (1000-based), KiB,MiB,... (1024-based)\n"
"  -b, --bytes            apparent size (st_size) in bytes\n"
"      --exclude PATTERN  skip entries matching the glob PATTERN\n"
"  -X, --exclude-from F   read exclude patterns from F, one per line\n"
"\n"
"p3m options:\n"
"  -j, --threads N        worker threads (default: number of online CPUs)\n"
"  -o, --output FILE      write CSV to FILE; a live progress display is shown\n"
"  -q, --quiet            suppress the console listing (progress and the\n"
"                         summary are still shown; a -o file is still written)\n"
"      --help             show this help and exit (NB: -h is human-readable,\n"
"                         as with du)\n"
"  -V, --version          show version and exit\n"
"\n"
"Hard-linked files are counted once. Symbolic links are never followed.\n",
    to);
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "summarize",      no_argument,       NULL, 's' },
        { "total",          no_argument,       NULL, 'c' },
        { "max-depth",      required_argument, NULL, 'd' },
        { "human-readable", no_argument,       NULL, 'h' },
        { "si",             no_argument,       NULL, 1000 },
        { "block-size",     required_argument, NULL, 'B' },
        { "bytes",          no_argument,       NULL, 'b' },
        { "exclude",        required_argument, NULL, 1001 },
        { "exclude-from",   required_argument, NULL, 'X' },
        { "threads",        required_argument, NULL, 'j' },
        { "output",         required_argument, NULL, 'o' },
        { "quiet",          no_argument,       NULL, 'q' },
        { "help",           no_argument,       NULL, 1002 },
        { "version",        no_argument,       NULL, 'V' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "scd:hB:bX:j:o:qV", lopts, NULL))
           != -1) {
        switch (c) {
        case 's':
            g.max_depth = 0;
            break;
        case 'c':
            g.total = true;
            break;
        case 'd': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 0 || v > INT_MAX) {
                fprintf(stderr, "p3m-du: invalid max depth '%s'\n", optarg);
                return 2;
            }
            g.max_depth = (int)v;
            break;
        }
        case 'h':
            g.fmt = FMT_HUMAN;
            g.units_str = "human";
            break;
        case 1000:
            g.fmt = FMT_SI;
            g.units_str = "si";
            break;
        case 'B':
            if (parse_blocksize(optarg, &g.blocksize) != 0) {
                fprintf(stderr, "p3m-du: invalid block size '%s'\n", optarg);
                return 2;
            }
            g.fmt = FMT_BLOCKS;
            g.units_str = optarg;
            break;
        case 'b':
            g.fmt = FMT_BLOCKS;
            g.blocksize = 1;
            g.apparent = true;
            g.units_str = "bytes";
            break;
        case 1001:
            if (add_exclude(optarg) != 0) {
                fprintf(stderr, "p3m-du: out of memory\n");
                return 2;
            }
            break;
        case 'X':
            if (load_exclude_file(optarg) != 0) {
                fprintf(stderr, "p3m-du: cannot read exclude file '%s': %s\n",
                        optarg, strerror(errno));
                return 2;
            }
            break;
        case 'j': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > 512) {
                fprintf(stderr, "p3m-du: invalid thread count '%s' "
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
        case 1002:
            usage(stdout);
            return 0;
        case 'V':
            printf("p3m-du %s (p3m: Parallel POSIX Permission Manager)\n",
                   P3M_DU_VERSION);
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
    p3m_color = isatty(STDERR_FILENO);
    g.suppress = g.quiet && !g.outpath;
    g.progress = (g.outpath || g.quiet) && isatty(STDERR_FILENO);

    FILE *out;
    if (g.outpath) {
        out = fopen(g.outpath, "w");
        if (!out) {
            fprintf(stderr, "p3m-du: cannot open '%s': %s\n",
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
        fputs("size,path\n", out);

    /* seed: one node per argument */
    p3m_stack_init(&stk, g.nthreads);
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
            p3m_note_error(root, "stat", errno);
            free(root);
            continue;
        }
        const char *base = strrchr(root, '/');
        base = base ? base + 1 : root;
        if (n_excl && excluded(base, root)) {
            free(root);
            continue;
        }

        uint64_t sz = entry_size(&st);
        if (S_ISDIR(st.st_mode)) {
            size_t idx = node_new(root, SIZE_MAX, 0, sz);
            char *is = idx == SIZE_MAX ? NULL : idx_str(idx);
            if (!is) {
                p3m_note_error(roots[i], "malloc", ENOMEM);
                continue;
            }
            atomic_fetch_add(&n_dirs, 1);
            atomic_fetch_add(&n_bytes, sz);
            p3m_stack_push_batch(&stk, &is, 1);
        } else {
            if (st.st_nlink > 1 && hl_seen(st.st_dev, st.st_ino))
                sz = 0;
            if (node_new(root, SIZE_MAX, 0, sz) == SIZE_MAX) {
                p3m_note_error(roots[i], "malloc", ENOMEM);
                continue;
            }
            atomic_fetch_add(&n_files, 1);
            atomic_fetch_add(&n_bytes, sz);
        }
    }

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
        fprintf(stderr, "p3m-du: out of memory\n");
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
            fprintf(stderr, "p3m-du: could not create any worker threads\n");
            return 2;
        }
        p3m_stack_set_threads(&stk, started);
    }
    for (int i = 0; i < started; i++)
        pthread_join(tids[i], NULL);

    double elapsed = p3m_mono_now() - t_start;

    if (g.progress)
        p3m_progress_stop();
    free(tids);

    /* roll child totals up into parents: a child's index always exceeds
     * its parent's, so one reverse sweep completes the whole forest */
    uint64_t grand = 0;
    for (size_t i = n_nodes; i-- > 0; ) {
        struct dnode *n = node_get(i);
        n->total = n->bytes;
        if (n->parent != SIZE_MAX)
            node_get(n->parent)->bytes += n->total;
        else
            grand += n->total;
    }

    /* emit, children before parents (deepest first, like du) */
    uint64_t rows = 0;
    p3m_outbuf ob;
    if (p3m_ob_init(&ob, &sink) != 0) {
        fprintf(stderr, "p3m-du: out of memory\n");
        return 2;
    }
    if (!g.suppress) {
        char sv[32];
        for (size_t i = n_nodes; i-- > 0; ) {
            struct dnode *n = node_get(i);
            if (n->depth > g.max_depth)
                continue;
            if (!p3m_ob_room(&ob, 2 * strlen(n->path) + 64)) {
                p3m_note_error(n->path, "emit", ENAMETOOLONG);
                continue;
            }
            fmt_du_size(n->total, sv);
            p3m_ob_puts(&ob, sv);
            p3m_ob_putc(&ob, ',');
            p3m_ob_csv(&ob, n->path);
            p3m_ob_putc(&ob, '\n');
            rows++;
        }
        if (g.total) {
            fmt_du_size(grand, sv);
            p3m_ob_fmt(&ob, "%s,total\n", sv);
            rows++;
        }
    }
    p3m_ob_flush(&ob);
    p3m_ob_free(&ob);

    /* finish the output stream */
    if (fflush(out) != 0 || ferror(out))
        atomic_store(&sink.failed, true);
    if (g.outpath && fclose(out) != 0)
        atomic_store(&sink.failed, true);

    if (atomic_load(&sink.failed)) {
        fprintf(stderr, "p3m-du: %swrite error%s on %s — output is incomplete\n",
                C_RED, C_RESET, g.outpath ? g.outpath : "stdout");
        return 1;
    }

    if (g.outpath || g.quiet)
        print_summary(elapsed, grand, rows);
    p3m_print_errors();
    p3m_stack_destroy(&stk);

    return atomic_load(&p3m_nerrors) ? 1 : 0;
}
