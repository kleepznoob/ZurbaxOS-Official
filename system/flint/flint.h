/*
 * Flint — the Zurbax shell.
 *
 * An interactive command language with line editing, history and
 * completion, pipes, redirection, && / ||, variables, globbing and job
 * control, plus a set of built-in commands (ls, cat, grep, find, ...)
 * so a fresh system is useful without any other programs.
 */
#pragma once

#include "zr.h"

/* Colours used by commands when writing to a terminal. */
#define C_RESET "\x1b[0m"
#define C_BOLD "\x1b[1m"
#define C_DIM "\x1b[2m"
#define C_RED "\x1b[31m"
#define C_GREEN "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_BLUE "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN "\x1b[36m"
#define C_GRAY "\x1b[90m"
#define C_BBLUE "\x1b[1;34m"
#define C_BGREEN "\x1b[1;32m"
#define C_BCYAN "\x1b[1;36m"

typedef int (*CommandFn)(int argc, char **argv);

typedef struct {
	const char *name;
	CommandFn fn;
	const char *usage;
	const char *help;
	bool special;          /* changes the shell itself: runs in-process, never forked */
} Command;

extern const Command COMMANDS[];
extern const int COMMAND_COUNT;
const Command *find_command(const char *name);

/* Output helpers that write through a buffer to stdout. */
void out(const char *s);
void outn(const char *s, usize n);
void outf(const char *f, ...) PRINTF_LIKE(1, 2);
void errf(const char *f, ...) PRINTF_LIKE(1, 2);
void out_flush(void);
bool out_is_tty(void);

/* Shell state shared with commands. */
typedef struct {
	char *name, *value;
} Alias;

typedef VEC(Alias) AliasVec;
typedef VEC(char *) StrVec;

extern int last_status;
extern bool interactive;
extern AliasVec aliases;
extern StrVec history;
const char *alias_get(const char *name);
void alias_set(const char *name, const char *value);
void alias_unset(const char *name);
int run_line(const char *line);           /* parses and runs a command line */
int source_file(const char *path);
const char *find_in_path(const char *name); /* static buffer, or NULL */
bool exit_requested(void);
void request_exit(int status);

/* Talking to the desktop (open, notify, theme). */
bool desktop_launch(const char *app, const char *arg);
bool desktop_notify(const char *title, const char *body);
bool desktop_settings_changed(void);
