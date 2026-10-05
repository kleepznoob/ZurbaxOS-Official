/* Files, directories, paths, configuration files and JSON. */
#pragma once

#include "mem.h"
#include "sys.h"

/* ---- Whole files ---------------------------------------------------------------- */

char *file_read(const char *path, usize *len_out);    /* NUL-terminated, NULL on error */
bool file_write(const char *path, const void *data, usize len);
bool file_write_atomic(const char *path, const void *data, usize len); /* tmp + rename */
bool file_append(const char *path, const void *data, usize len);
char *file_read_line(const char *path);                /* first line, trimmed */
i64 file_read_int(const char *path, i64 fallback);     /* e.g. sysfs values */
bool file_write_str(const char *path, const char *s);
bool file_exists(const char *path);
bool dir_exists(const char *path);
i64 file_size(const char *path);
i64 file_mtime(const char *path);
bool file_copy(const char *from, const char *to);
bool dir_make(const char *path, int mode);             /* creates parents as needed */
bool path_remove_all(const char *path);                /* recursive delete */

/* ---- Directories ------------------------------------------------------------------ */

typedef struct {
	char *name;
	u8 type;    /* DT_* */
	i64 size;
	i64 mtime;
	u32 mode;
} DirEntry;

typedef VEC(DirEntry) DirList;

/* Lists a directory (without . and ..), optionally with stat details, sorted
 * folders-first in natural order. Returns false if it cannot be opened. */
bool dir_list(const char *path, DirList *out, bool with_stat);
void dir_list_free(DirList *list);

/* ---- Paths -------------------------------------------------------------------------- */

char *path_join(const char *a, const char *b);
const char *path_base(const char *path);     /* points into path */
char *path_dir(const char *path);            /* allocated */
const char *path_ext(const char *path);      /* "png" or "" */
char *path_normalize(const char *path);      /* resolves . and .., collapses // */
const char *home_dir(void);

/* ---- Buffered line reader ------------------------------------------------------------ */

typedef struct {
	int fd;
	char buf[4096];
	usize start, end;
	StrBuf line;
	bool eof;
} LineReader;

void line_reader_init(LineReader *r, int fd);
const char *line_reader_next(LineReader *r); /* NULL at end of input */
void line_reader_free(LineReader *r);

/* ---- "key = value" configuration files --------------------------------------------- */

typedef struct {
	char *path;
	VEC(char *) keys;
	VEC(char *) values;
} Conf;

Conf *conf_load(const char *path);           /* never NULL: a missing file is empty */
const char *conf_get(const Conf *c, const char *key, const char *fallback);
i64 conf_get_int(const Conf *c, const char *key, i64 fallback);
f64 conf_get_float(const Conf *c, const char *key, f64 fallback);
bool conf_get_bool(const Conf *c, const char *key, bool fallback);
void conf_set(Conf *c, const char *key, const char *value);
void conf_set_int(Conf *c, const char *key, i64 value);
void conf_set_bool(Conf *c, const char *key, bool value);
void conf_unset(Conf *c, const char *key);
bool conf_save(const Conf *c);
void conf_free(Conf *c);

/* ---- JSON ---------------------------------------------------------------------------- */

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct Json Json;
struct Json {
	JsonType type;
	bool boolean;
	f64 number;
	char *string;      /* JSON_STRING; also the key of an object member */
	char *key;         /* set on members of an object */
	Json **items;      /* JSON_ARRAY / JSON_OBJECT children */
	usize count;
};

Json *json_parse(const char *text, usize len);           /* NULL on syntax error */
void json_free(Json *j);
Json *json_get(const Json *obj, const char *key);        /* NULL if absent */
Json *json_at(const Json *arr, usize i);
Json *json_path(const Json *j, const char *path);        /* "current.temperature_2m" or "items.0.name" */
const char *json_str(const Json *j, const char *fallback);
f64 json_num(const Json *j, f64 fallback);
bool json_bool(const Json *j, bool fallback);
void sb_json_string(StrBuf *sb, const char *s);          /* writes a quoted, escaped string */
