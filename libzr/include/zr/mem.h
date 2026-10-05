/* Memory, strings, text formatting and containers. */
#pragma once

#include "base.h"

/* ---- Raw memory (standard names: the compiler emits calls to these) --------- */

void *memcpy(void *restrict dst, const void *restrict src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);
void *memchr(const void *s, int c, usize n);
usize strlen(const char *s);

/* ---- Heap -------------------------------------------------------------------- */

void *mem_alloc(usize n);              /* uninitialised */
void *mem_zalloc(usize n);             /* zero-filled */
void *mem_realloc(void *p, usize n);
void mem_free(void *p);
void *mem_dup(const void *p, usize n);
usize mem_usable(const void *p);

#define NEW(T) ((T *)mem_zalloc(sizeof(T)))
#define NEW_ARRAY(T, n) ((T *)mem_zalloc(sizeof(T) * (usize)(n)))

/* ---- C strings ----------------------------------------------------------------- */

bool str_eq(const char *a, const char *b);
bool str_eq_nocase(const char *a, const char *b);
int str_cmp(const char *a, const char *b);
int str_cmp_nocase(const char *a, const char *b);
int str_cmp_natural(const char *a, const char *b); /* "file2" < "file10" */
bool str_starts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
char *str_dup(const char *s);
char *str_ndup(const char *s, usize n);
const char *str_chr(const char *s, int c);
const char *str_rchr(const char *s, int c);
const char *str_find(const char *hay, const char *needle);
const char *str_find_nocase(const char *hay, const char *needle);
usize str_copy(char *dst, const char *src, usize cap); /* always terminates; returns strlen(src) */
char *str_trim(char *s);                               /* in place, returns s advanced */
char *str_lower(char *s);
i64 str_to_int(const char *s, const char **end);
f64 str_to_float(const char *s, const char **end);
char **str_split(const char *s, char sep, usize *count); /* free with str_split_free */
void str_split_free(char **parts, usize count);
char *str_replace(const char *s, const char *from, const char *to);

static inline bool is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }
static inline bool is_alpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static inline bool is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static inline bool is_xdigit(int c) { return is_digit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static inline int to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int to_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* ---- UTF-8 ---------------------------------------------------------------------- */

/* Decodes one code point; returns bytes consumed (>= 1). Invalid input yields U+FFFD. */
usize utf8_decode(const char *s, usize len, u32 *cp);
usize utf8_encode(u32 cp, char out[4]);
usize utf8_next(const char *s, usize len, usize pos);
usize utf8_prev(const char *s, usize pos);
usize utf8_count(const char *s, usize len);

/* ---- Formatting ------------------------------------------------------------------ */
/* printf-style: %d %i %u %x %X %o %c %s %p %f %e %g %% with flags - + 0 space #,
 * width, .precision (including * forms such as %.*s), and length modifiers l ll z h hh. */

int fmt_v(char *buf, usize cap, const char *f, va_list ap);
int fmt(char *buf, usize cap, const char *f, ...) PRINTF_LIKE(3, 4);
char *str_fmt(const char *f, ...) PRINTF_LIKE(1, 2);
char *str_fmt_v(const char *f, va_list ap);
void print(const char *f, ...) PRINTF_LIKE(1, 2);
void eprint(const char *f, ...) PRINTF_LIKE(1, 2);
void fd_print(int fd, const char *f, ...) PRINTF_LIKE(2, 3);
void fmt_bytes(char *buf, usize cap, u64 bytes); /* "1.4 GB" */

/* ---- Growable string ---------------------------------------------------------------- */

typedef struct {
	char *data; /* always NUL-terminated when non-NULL */
	usize len;
	usize cap;
} StrBuf;

void sb_reserve(StrBuf *sb, usize extra);
void sb_add(StrBuf *sb, const void *data, usize n);
void sb_adds(StrBuf *sb, const char *s);
void sb_addc(StrBuf *sb, char c);
void sb_addcp(StrBuf *sb, u32 codepoint);
void sb_fmt(StrBuf *sb, const char *f, ...) PRINTF_LIKE(2, 3);
void sb_insert(StrBuf *sb, usize pos, const char *s, usize n);
void sb_remove(StrBuf *sb, usize pos, usize n);
void sb_clear(StrBuf *sb);
char *sb_take(StrBuf *sb); /* hands ownership of the buffer to the caller */
void sb_free(StrBuf *sb);
static inline const char *sb_str(const StrBuf *sb) { return sb->data ? sb->data : ""; }

/* ---- Dynamic arrays ------------------------------------------------------------------ */

#define VEC(T)            \
	struct {          \
		T *data;  \
		usize len; \
		usize cap; \
	}

void vec_grow_(void **data, usize *cap, usize need, usize elem);

#define vec_reserve(v, n) vec_grow_((void **)&(v)->data, &(v)->cap, (n), sizeof(*(v)->data))
#define vec_push(v, ...) (vec_reserve((v), (v)->len + 1), (v)->data[(v)->len++] = (__VA_ARGS__))
#define vec_pop(v) ((v)->data[--(v)->len])
#define vec_last(v) ((v)->data[(v)->len - 1])
#define vec_clear(v) ((v)->len = 0)
#define vec_free(v) (mem_free((v)->data), (v)->data = NULL, (v)->len = (v)->cap = 0)
#define vec_remove(v, i)                                                                       \
	(memmove(&(v)->data[(i)], &(v)->data[(i) + 1], ((v)->len - (i) - 1) * sizeof(*(v)->data)), \
	 (v)->len--)
#define vec_insert(v, i, ...)                                                                  \
	(vec_reserve((v), (v)->len + 1),                                                       \
	 memmove(&(v)->data[(i) + 1], &(v)->data[(i)], ((v)->len - (i)) * sizeof(*(v)->data)), \
	 (v)->data[(i)] = (__VA_ARGS__), (v)->len++)
#define vec_each(v, it) for (__auto_type it = (v)->data; it && it < (v)->data + (v)->len; it++)

/* ---- Sorting --------------------------------------------------------------------------- */

typedef int (*CmpFn)(const void *a, const void *b, void *ctx);
void sort(void *base, usize n, usize size, CmpFn cmp, void *ctx); /* stable merge sort */

/* ---- Hash map: string keys -> pointers ---------------------------------------------------- */

typedef struct {
	char *key;
	void *value;
	u64 hash;
} MapSlot;

typedef struct {
	MapSlot *slots;
	usize cap;
	usize len;
} Map;

u64 hash_bytes(const void *data, usize n);
void *map_get(const Map *m, const char *key);
bool map_has(const Map *m, const char *key);
void map_set(Map *m, const char *key, void *value); /* key is copied */
void *map_del(Map *m, const char *key);             /* returns the old value */
void map_free(Map *m, void (*free_value)(void *));
/* Iterate: for (usize i = 0; map_next(&m, &i, &k, &v);) */
bool map_next(const Map *m, usize *iter, const char **key, void **value);
