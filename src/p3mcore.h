/*
 * p3mcore — shared engine for the p3m tool suite
 *
 * Parallel directory-walk work queue, buffered thread-safe CSV output,
 * uid/gid name caching, error accounting, live progress scaffolding and
 * small formatting helpers.
 */
#ifndef P3MCORE_H
#define P3MCORE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ---- colours (tools set p3m_color once at startup) ------------------ */

extern bool p3m_color;
#define C_RESET (p3m_color ? "\x1b[0m"  : "")
#define C_BOLD  (p3m_color ? "\x1b[1m"  : "")
#define C_DIM   (p3m_color ? "\x1b[2m"  : "")
#define C_RED   (p3m_color ? "\x1b[31m" : "")
#define C_GREEN (p3m_color ? "\x1b[32m" : "")
#define C_CYAN  (p3m_color ? "\x1b[36m" : "")

/* ---- formatting helpers --------------------------------------------- */

double p3m_mono_now(void);
char  *p3m_fmt_u64(uint64_t v, char out[32]);      /* 1234567 -> 1,234,567 */
char  *p3m_fmt_size(uint64_t b, char out[32]);     /* 1234567 -> 1.18 MiB  */
char  *p3m_fmt_elapsed(double s, char out[32]);
void   p3m_fmt_ts(const struct timespec *ts, char out[64], bool with_ns);
void   p3m_fmt_perms(mode_t m, char out[11]);      /* -rw-r--r--           */
char   p3m_dt_char(unsigned char dt);              /* DT_REG -> 'f', ...   */
int    p3m_term_width(void);
void   p3m_trunc_left(const char *s, size_t max, char *out, size_t outsz);
char  *p3m_path_join(const char *dir, size_t dlen, const char *name);

/* ---- error accounting ----------------------------------------------- */

extern _Atomic uint64_t p3m_nerrors;
void p3m_note_error(const char *path, const char *what, int err);
void p3m_print_errors(void);                       /* report on stderr */

/* ---- "current path" shown by the progress display ------------------- */

void p3m_set_current(const char *p);
void p3m_get_current(char *dst, size_t n);

/* ---- parallel work stack of directory paths ------------------------- */

typedef struct {
    char           **items;
    size_t           len, cap;
    int              idle, nthreads;
    bool             done;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
} p3m_stack;

void  p3m_stack_init(p3m_stack *s, int nthreads);
void  p3m_stack_set_threads(p3m_stack *s, int nthreads);
/* takes ownership of the paths (malloc'd); frees them on queue failure */
void  p3m_stack_push_batch(p3m_stack *s, char **paths, size_t n);
/* returns a path to scan (caller frees), or NULL when the walk is done */
char *p3m_stack_pop(p3m_stack *s);
void  p3m_stack_destroy(p3m_stack *s);

/* ---- buffered, thread-safe output ----------------------------------- */

#define P3M_OB_CAP ((size_t)1 << 20)

typedef struct {
    FILE           *f;
    pthread_mutex_t mu;
    atomic_bool     failed;
} p3m_sink;

typedef struct {
    char     *buf;
    size_t    len;
    p3m_sink *sink;
} p3m_outbuf;

void p3m_sink_init(p3m_sink *k, FILE *f);
int  p3m_ob_init(p3m_outbuf *ob, p3m_sink *k);     /* 0 ok, -1 ENOMEM */
void p3m_ob_free(p3m_outbuf *ob);
void p3m_ob_flush(p3m_outbuf *ob);
/* ensure `need` bytes are free (flushing if necessary); false = too big */
bool p3m_ob_room(p3m_outbuf *ob, size_t need);
void p3m_ob_puts(p3m_outbuf *ob, const char *s);
void p3m_ob_putc(p3m_outbuf *ob, char c);
void p3m_ob_fmt(p3m_outbuf *ob, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void p3m_ob_csv(p3m_outbuf *ob, const char *s);    /* RFC 4180 quoting */

/* ---- file content copy ----------------------------------------------- */

/* copy open fd in -> out (copy_file_range, falling back to read/write);
 * adds bytes copied to *bytes if non-NULL; 0 ok, -1 with errno set */
int p3m_copy_fd(int in, int out, _Atomic uint64_t *bytes);

/* ---- uid/gid name caches and resolvers ------------------------------ */

const char *p3m_uid_name(uid_t id, char fallback[32]);
const char *p3m_gid_name(gid_t id, char fallback[32]);
/* name or numeric string -> id; 0 ok, -1 unknown */
int p3m_resolve_user(const char *s, uid_t *out);
int p3m_resolve_group(const char *s, gid_t *out);

/* ---- live progress display scaffolding ------------------------------ */
/*
 * The core owns the refresh thread, cursor handling, redraw positioning
 * and rate calculation; the tool provides a draw callback that renders
 * exactly `lines` lines (each starting with \x1b[K, ending with \n).
 */
typedef struct {
    void     (*draw)(double rate, int frame);
    uint64_t (*items)(void);       /* monotonic counter used for the rate */
    int        lines;
} p3m_progress_cfg;

int  p3m_progress_start(const p3m_progress_cfg *cfg);
void p3m_progress_stop(void);      /* joins, clears block, restores cursor */

extern const char *p3m_spinner[10];

#endif /* P3MCORE_H */
