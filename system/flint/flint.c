/* Flint: line editing, parsing and running commands. */
#include "flint.h"

int last_status;
bool interactive;
AliasVec aliases;
StrVec history;

static int tty = -1;
static int shell_pgid;
static bool want_exit;
static int exit_status;
static char history_path[512];

bool exit_requested(void)
{
	return want_exit;
}

void request_exit(int status)
{
	want_exit = true;
	exit_status = status;
}

/* ---- Output buffer ------------------------------------------------------------------- */

static char obuf[8192];
static usize olen;

void out_flush(void)
{
	if (olen)
		write_all(1, obuf, olen);
	olen = 0;
}

void outn(const char *s, usize n)
{
	if (olen + n > sizeof obuf)
		out_flush();
	if (n > sizeof obuf) {
		write_all(1, s, n);
		return;
	}
	memcpy(obuf + olen, s, n);
	olen += n;
}

void out(const char *s)
{
	outn(s, strlen(s));
}

void outf(const char *f, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, f);
	int n = fmt_v(buf, sizeof buf, f, ap);
	va_end(ap);
	outn(buf, (usize)MIN(n, (int)sizeof buf - 1));
}

void errf(const char *f, ...)
{
	out_flush();
	char buf[2048];
	va_list ap;
	va_start(ap, f);
	int n = fmt_v(buf, sizeof buf, f, ap);
	va_end(ap);
	write_all(2, buf, (usize)MIN(n, (int)sizeof buf - 1));
}

bool out_is_tty(void)
{
	struct termios t;
	return sys_ioctl(1, TCGETS, &t) == 0;
}

/* ---- Aliases ----------------------------------------------------------------------------- */

const char *alias_get(const char *name)
{
	vec_each(&aliases, a) if (str_eq(a->name, name)) return a->value;
	return NULL;
}

void alias_set(const char *name, const char *value)
{
	vec_each(&aliases, a) {
		if (str_eq(a->name, name)) {
			mem_free(a->value);
			a->value = str_dup(value);
			return;
		}
	}
	vec_push(&aliases, ((Alias){ str_dup(name), str_dup(value) }));
}

void alias_unset(const char *name)
{
	for (usize i = 0; i < aliases.len; i++)
		if (str_eq(aliases.data[i].name, name)) {
			mem_free(aliases.data[i].name);
			mem_free(aliases.data[i].value);
			aliases.data[i] = aliases.data[--aliases.len];
			return;
		}
}

/* ---- Paths --------------------------------------------------------------------------------- */

const char *find_in_path(const char *name)
{
	static char buf[1024];
	if (str_chr(name, '/')) {
		str_copy(buf, name, sizeof buf);
		return file_exists(buf) ? buf : NULL;
	}
	const char *path = env_get("PATH");
	if (!path)
		path = "/system/bin:/data/apps/bin";
	while (*path) {
		const char *e = str_chr(path, ':');
		usize n = e ? (usize)(e - path) : strlen(path);
		fmt(buf, sizeof buf, "%.*s/%s", (int)n, path, name);
		struct stat st;
		if (n && sys_stat(buf, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
			return buf;
		path += n;
		if (*path == ':')
			path++;
	}
	return NULL;
}

/* ---- Tokenizing ------------------------------------------------------------------------------ */

typedef enum { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_AMP, T_OUT, T_APPEND, T_IN, T_ERR, T_ERR_APPEND, T_ERR_OUT, T_BOTH, T_END } TokType;

typedef struct {
	TokType type;
	char *text;            /* T_WORD */
	bool glob;             /* contains unquoted * ? */
	bool quoted;           /* any part quoted (an empty "" still counts as a word) */
} Token;

typedef VEC(Token) Tokens;

static char *capture_output(const char *line);

static void expand_var(const char **pp, StrBuf *w)
{
	const char *p = *pp;
	char name[128];
	usize n = 0;
	if (*p == '?') {
		sb_fmt(w, "%d", last_status);
		*pp = p + 1;
		return;
	}
	if (*p == '$') {
		sb_fmt(w, "%d", (int)sys_getpid());
		*pp = p + 1;
		return;
	}
	if (*p == '{') {
		p++;
		while (*p && *p != '}' && n < sizeof name - 1)
			name[n++] = *p++;
		if (*p == '}')
			p++;
	} else {
		while ((is_alnum((u8)*p) || *p == '_') && n < sizeof name - 1)
			name[n++] = *p++;
	}
	name[n] = 0;
	if (n == 0) {
		sb_addc(w, '$');
	} else {
		const char *v = env_get(name);
		if (v)
			sb_adds(w, v);
	}
	*pp = p;
}

/* "$(...)": the output of a command, trailing newlines removed. */
static bool expand_subst(const char **pp, StrBuf *w)
{
	const char *p = *pp + 2;
	int depth = 1;
	const char *start = p;
	while (*p && depth) {
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		if (depth)
			p++;
	}
	if (!*p)
		return false;
	char *inner = str_ndup(start, (usize)(p - start));
	char *res = capture_output(inner);
	mem_free(inner);
	usize n = strlen(res);
	while (n && res[n - 1] == '\n')
		n--;
	sb_add(w, res, n);
	mem_free(res);
	*pp = p + 1;
	return true;
}

static bool tokenize(const char *s, Tokens *toks)
{
	const char *p = s;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || *p == '#' || *p == '\n')
			break;
		Token t = { 0 };
		if (p[0] == '|' && p[1] == '|') t.type = T_OR, p += 2;
		else if (*p == '|') t.type = T_PIPE, p++;
		else if (p[0] == '&' && p[1] == '&') t.type = T_AND, p += 2;
		else if (p[0] == '&' && p[1] == '>') t.type = T_BOTH, p += 2;
		else if (*p == '&') t.type = T_AMP, p++;
		else if (*p == ';') t.type = T_SEMI, p++;
		else if (p[0] == '>' && p[1] == '>') t.type = T_APPEND, p += 2;
		else if (*p == '>') t.type = T_OUT, p++;
		else if (*p == '<') t.type = T_IN, p++;
		else if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') t.type = T_ERR_OUT, p += 4;
		else if (p[0] == '2' && p[1] == '>' && p[2] == '>') t.type = T_ERR_APPEND, p += 3;
		else if (p[0] == '2' && p[1] == '>') t.type = T_ERR, p += 2;
		else {
			t.type = T_WORD;
			StrBuf w = { 0 };
			bool first = true;
			while (*p && !str_chr(" \t\n|&;<>", *p)) {
				if (*p == '\'') {
					t.quoted = true;
					p++;
					while (*p && *p != '\'')
						sb_addc(&w, *p++);
					if (!*p) {
						sb_free(&w);
						return false;
					}
					p++;
				} else if (*p == '"') {
					t.quoted = true;
					p++;
					while (*p && *p != '"') {
						if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$'))
							sb_addc(&w, p[1]), p += 2;
						else if (*p == '$' && p[1] == '(') {
							if (!expand_subst(&p, &w)) {
								sb_free(&w);
								return false;
							}
						} else if (*p == '$')
							p++, expand_var(&p, &w);
						else
							sb_addc(&w, *p++);
					}
					if (!*p) {
						sb_free(&w);
						return false;
					}
					p++;
				} else if (*p == '\\' && p[1]) {
					sb_addc(&w, p[1]);
					p += 2;
				} else if (*p == '$' && p[1] == '(') {
					if (!expand_subst(&p, &w)) {
						sb_free(&w);
						return false;
					}
				} else if (*p == '$') {
					p++;
					expand_var(&p, &w);
				} else if (*p == '~' && first && (p[1] == '/' || p[1] == 0 || str_chr(" \t|&;<>", p[1]))) {
					sb_adds(&w, home_dir());
					p++;
				} else {
					if (*p == '*' || *p == '?')
						t.glob = true;
					sb_addc(&w, *p++);
				}
				first = false;
			}
			t.text = sb_take(&w);
		}
		vec_push(toks, t);
	}
	vec_push(toks, ((Token){ .type = T_END }));
	return true;
}

static void free_tokens(Tokens *toks)
{
	vec_each(toks, t) mem_free(t->text);
	vec_free(toks);
}

/* ---- Globbing ------------------------------------------------------------------------------------ */

static bool match(const char *pat, const char *s)
{
	while (*pat) {
		if (*pat == '*') {
			while (*pat == '*')
				pat++;
			if (!*pat)
				return true;
			for (; *s; s++)
				if (match(pat, s))
					return true;
			return false;
		}
		if (!*s)
			return false;
		if (*pat != '?' && *pat != *s)
			return false;
		pat++, s++;
	}
	return !*s;
}

static int cmp_str(const void *a, const void *b, void *ctx)
{
	UNUSED(ctx);
	return str_cmp_natural(*(char *const *)a, *(char *const *)b);
}

/* Expands a pattern in its last path component; no match keeps the word. */
static void glob_into(const char *word, StrVec *out_args)
{
	const char *slash = str_rchr(word, '/');
	char *dir = slash ? str_ndup(word, (usize)(slash - word) + 1) : str_dup("");
	const char *pat = slash ? slash + 1 : word;
	DirList list = { 0 };
	usize before = out_args->len;
	if (dir_list(*dir ? dir : ".", &list, false)) {
		vec_each(&list, e) {
			if (e->name[0] == '.' && pat[0] != '.')
				continue;
			if (match(pat, e->name))
				vec_push(out_args, str_fmt("%s%s", dir, e->name));
		}
		dir_list_free(&list);
	}
	mem_free(dir);
	if (out_args->len == before)
		vec_push(out_args, str_dup(word));
	else
		sort(out_args->data + before, out_args->len - before, sizeof(char *), cmp_str, NULL);
}

/* ---- Running --------------------------------------------------------------------------------------- */

typedef struct {
	StrVec argv;
	char *in, *out, *err;
	bool append, err_append, err_to_out;
} Cmd;

#define MAX_PIPE 16

typedef struct {
	int pgid;
	int pids[MAX_PIPE];
	int npids;
	char text[256];
	bool stopped;
} Job;

static Job jobs[32];
static int njobs;

static void free_cmd(Cmd *c)
{
	vec_each(&c->argv, a) mem_free(*a);
	vec_free(&c->argv);
	mem_free(c->in);
	mem_free(c->out);
	mem_free(c->err);
}

static bool apply_redirects(const Cmd *c)
{
	if (c->in) {
		int fd = (int)sys_open(c->in, O_RDONLY | O_CLOEXEC, 0);
		if (fd < 0) {
			errf("flint: %s: %s\n", c->in, err_str(fd));
			return false;
		}
		sys_dup3(fd, 0, 0);
		sys_close(fd);
	}
	if (c->out) {
		int fd = (int)sys_open(c->out, O_WRONLY | O_CREAT | O_CLOEXEC | (c->append ? O_APPEND : O_TRUNC), 0644);
		if (fd < 0) {
			errf("flint: %s: %s\n", c->out, err_str(fd));
			return false;
		}
		sys_dup3(fd, 1, 0);
		sys_close(fd);
	}
	if (c->err) {
		int fd = (int)sys_open(c->err, O_WRONLY | O_CREAT | O_CLOEXEC | (c->err_append ? O_APPEND : O_TRUNC), 0644);
		if (fd < 0) {
			errf("flint: %s: %s\n", c->err, err_str(fd));
			return false;
		}
		sys_dup3(fd, 2, 0);
		sys_close(fd);
	}
	if (c->err_to_out)
		sys_dup3(1, 2, 0);
	return true;
}

static void reset_child_signals(void)
{
	static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE };
	for (usize i = 0; i < ARRAY_LEN(sigs); i++)
		sig_default(sigs[i]);
}

static int status_of(int st)
{
	if ((st & 0x7f) == 0)
		return (st >> 8) & 0xff;            /* exited */
	if ((st & 0xff) == 0x7f)
		return 128 + ((st >> 8) & 0xff);    /* stopped */
	return 128 + (st & 0x7f);               /* killed by a signal */
}

static void give_terminal(int pgid)
{
	if (interactive && tty >= 0)
		sys_ioctl(tty, TIOCSPGRP, &pgid);
}

/* Waits for a foreground job; a stopped job (Ctrl+Z) is kept in the jobs list. */
static int wait_job(Job *j)
{
	int status = 0;
	int remaining = j->npids;
	if (!interactive) {
		/* No job control: the children share our process group, so wait for each. */
		for (int i = 0; i < j->npids; i++) {
			int st = 0;
			long r;
			do
				r = sys_wait4(j->pids[i], &st, 0, NULL);
			while (r == -EINTR);
			if (i == j->npids - 1)
				status = status_of(st);
		}
		return status;
	}
	while (remaining > 0) {
		int st = 0;
		long pid = sys_wait4(-j->pgid, &st, WUNTRACED, NULL);
		if (pid < 0) {
			if (pid == -EINTR)
				continue;
			break;
		}
		if ((st & 0xff) == 0x7f) {
			j->stopped = true;
			if (njobs < (int)ARRAY_LEN(jobs))
				jobs[njobs++] = *j;
			errf("\n[%d]+  Stopped    %s\n", njobs, j->text);
			status = 148;
			break;
		}
		remaining--;
		if (pid == j->pids[j->npids - 1])
			status = status_of(st);
	}
	give_terminal(shell_pgid);
	return status;
}

static int run_pipeline(Cmd *cmds, int n, bool background, const char *text)
{
	/* A special command alone changes the shell itself, so it can't run in a child. */
	if (n == 1 && cmds[0].argv.len) {
		const Command *c = find_command(cmds[0].argv.data[0]);
		if (c && c->special) {
			int saved[3] = { (int)sys_dup3(0, 100, O_CLOEXEC) >= 0 ? 100 : -1, -1, -1 };
			saved[1] = sys_dup3(1, 101, O_CLOEXEC) >= 0 ? 101 : -1;
			saved[2] = sys_dup3(2, 102, O_CLOEXEC) >= 0 ? 102 : -1;
			int st = 1;
			if (apply_redirects(&cmds[0])) {
				vec_push(&cmds[0].argv, NULL);
				st = c->fn((int)cmds[0].argv.len - 1, cmds[0].argv.data);
				cmds[0].argv.len--;
			}
			out_flush();
			for (int i = 0; i < 3; i++)
				if (saved[i] >= 0) {
					sys_dup3(saved[i], i, 0);
					sys_close(saved[i]);
				}
			return st;
		}
	}
	Job job = { 0 };
	str_copy(job.text, text, sizeof job.text);
	int prev_read = -1;
	for (int i = 0; i < n; i++) {
		int fds[2] = { -1, -1 };
		if (i + 1 < n && sys_pipe2(fds, O_CLOEXEC) < 0) {
			errf("flint: can't create a pipe\n");
			break;
		}
		out_flush();
		long pid = syscall5(SYS_clone, SIGCHLD, 0, 0, 0, 0);
		if (pid == 0) {
			int me = (int)sys_getpid();
			if (interactive)
				sys_setpgid(0, job.pgid ? job.pgid : me);
			if (interactive && !background && i == 0)
				give_terminal(me);
			reset_child_signals();
			if (prev_read >= 0)
				sys_dup3(prev_read, 0, 0);
			if (fds[1] >= 0)
				sys_dup3(fds[1], 1, 0);
			if (!apply_redirects(&cmds[i]))
				syscall1(SYS_exit, 1);
			Cmd *c = &cmds[i];
			if (!c->argv.len)
				syscall1(SYS_exit, 0);
			vec_push(&c->argv, NULL);
			const Command *bc = find_command(c->argv.data[0]);
			if (bc) {
				int st = bc->fn((int)c->argv.len - 1, c->argv.data);
				out_flush();
				syscall1(SYS_exit, st);
			}
			const char *path = find_in_path(c->argv.data[0]);
			if (!path) {
				errf("flint: command not found: %s\n", c->argv.data[0]);
				syscall1(SYS_exit, 127);
			}
			sys_execve(path, c->argv.data, environ);
			errf("flint: can't run %s\n", c->argv.data[0]);
			syscall1(SYS_exit, 126);
		}
		if (pid < 0) {
			errf("flint: can't start a process\n");
			break;
		}
		if (!job.pgid)
			job.pgid = (int)pid;
		if (interactive)
			sys_setpgid((int)pid, job.pgid);
		job.pids[job.npids++] = (int)pid;
		if (prev_read >= 0)
			sys_close(prev_read);
		if (fds[1] >= 0)
			sys_close(fds[1]);
		prev_read = fds[0];
	}
	if (prev_read >= 0)
		sys_close(prev_read);
	if (!job.npids)
		return 1;
	if (background) {
		if (njobs < (int)ARRAY_LEN(jobs))
			jobs[njobs++] = job;
		errf("[%d] %d\n", njobs, job.pgid);
		return 0;
	}
	give_terminal(job.pgid);
	return wait_job(&job);
}

/* Reports background jobs that finished. */
static void reap_jobs(void)
{
	for (int i = 0; i < njobs; i++) {
		if (jobs[i].stopped)
			continue;
		int st;
		bool done = true;
		for (int k = 0; k < jobs[i].npids; k++) {
			if (jobs[i].pids[k] == 0)
				continue;
			long r = sys_wait4(jobs[i].pids[k], &st, WNOHANG, NULL);
			if (r == 0)
				done = false;
			else
				jobs[i].pids[k] = 0;
		}
		if (done) {
			errf("[%d]   Done       %s\n", i + 1, jobs[i].text);
			jobs[i] = jobs[--njobs];
			i--;
		}
	}
}

int job_foreground(int index)
{
	if (njobs == 0) {
		errf("fg: no jobs\n");
		return 1;
	}
	int i = index > 0 && index <= njobs ? index - 1 : njobs - 1;
	Job j = jobs[i];
	jobs[i] = jobs[--njobs];
	j.stopped = false;
	outf("%s\n", j.text);
	out_flush();
	give_terminal(j.pgid);
	sys_kill(-j.pgid, SIGCONT);
	return wait_job(&j);
}

int job_background(int index)
{
	for (int i = njobs - 1; i >= 0; i--)
		if (jobs[i].stopped && (index <= 0 || index == i + 1)) {
			jobs[i].stopped = false;
			sys_kill(-jobs[i].pgid, SIGCONT);
			outf("[%d]+ %s &\n", i + 1, jobs[i].text);
			return 0;
		}
	errf("bg: no stopped jobs\n");
	return 1;
}

void job_list(void)
{
	for (int i = 0; i < njobs; i++)
		outf("[%d]  %-10s %s\n", i + 1, jobs[i].stopped ? "Stopped" : "Running", jobs[i].text);
}

/* Builds commands from tokens and runs them, honouring && || ; &. */
static int run_tokens(Token *t, int depth)
{
	int status = last_status;
	bool skip = false;
	while (t->type != T_END) {
		Cmd cmds[MAX_PIPE];
		memset(cmds, 0, sizeof cmds);
		int n = 1;
		Token *start = t;
		bool ok = true;
		while (t->type != T_END && t->type != T_AND && t->type != T_OR && t->type != T_SEMI && t->type != T_AMP) {
			Cmd *c = &cmds[n - 1];
			if (t->type == T_PIPE) {
				if (n == MAX_PIPE || !c->argv.len) {
					errf("flint: syntax error near |\n");
					ok = false;
					break;
				}
				n++;
				t++;
				continue;
			}
			if (t->type == T_WORD) {
				/* Aliases expand for the first word of a command. */
				const char *al = !c->argv.len && depth < 4 ? alias_get(t->text) : NULL;
				if (al) {
					Tokens sub = { 0 };
					if (tokenize(al, &sub))
						for (usize k = 0; k + 1 < sub.len; k++)
							if (sub.data[k].type == T_WORD)
								vec_push(&c->argv, str_dup(sub.data[k].text));
					free_tokens(&sub);
				} else if (t->glob) {
					glob_into(t->text, &c->argv);
				} else {
					vec_push(&c->argv, str_dup(t->text));
				}
				t++;
				continue;
			}
			/* A redirection needs a file name after it. */
			TokType r = t->type;
			if (r == T_ERR_OUT) {
				c->err_to_out = true;
				t++;
				continue;
			}
			if (t[1].type != T_WORD) {
				errf("flint: syntax error: missing file name\n");
				ok = false;
				break;
			}
			char *f = str_dup(t[1].text);
			switch (r) {
			case T_OUT: mem_free(c->out), c->out = f, c->append = false; break;
			case T_APPEND: mem_free(c->out), c->out = f, c->append = true; break;
			case T_IN: mem_free(c->in), c->in = f; break;
			case T_ERR: mem_free(c->err), c->err = f, c->err_append = false; break;
			case T_ERR_APPEND: mem_free(c->err), c->err = f, c->err_append = true; break;
			case T_BOTH: mem_free(c->out), c->out = f, c->append = false, c->err_to_out = true; break;
			default: mem_free(f);
			}
			t += 2;
		}
		bool background = t->type == T_AMP;
		/* The command's text, for job messages. */
		StrBuf text = { 0 };
		for (Token *k = start; k < t; k++)
			if (k->type == T_WORD) {
				if (text.len)
					sb_addc(&text, ' ');
				sb_adds(&text, k->text);
			}
		if (ok && !skip && (cmds[0].argv.len || cmds[0].in || cmds[0].out))
			status = run_pipeline(cmds, n, background, sb_str(&text));
		else if (!ok)
			status = 2;
		sb_free(&text);
		for (int i = 0; i < n; i++)
			free_cmd(&cmds[i]);
		last_status = status;
		if (want_exit || !ok)
			break;
		if (t->type == T_END)
			break;
		TokType sep = t->type;
		t++;
		skip = (sep == T_AND && status != 0) || (sep == T_OR && status == 0);
		if (sep == T_SEMI || sep == T_AMP)
			skip = false;
	}
	return status;
}

/* Finds the end of the next command at the top level: a ; && || or & outside
 * quotes and $( ). Returns the separator found (T_END at the end of the line). */
static TokType next_separator(const char *p, const char **end)
{
	const char *start = p;
	int depth = 0;
	char quote = 0;
	for (; *p; p++) {
		if (quote) {
			if (*p == '\\' && quote == '"' && p[1])
				p++;
			else if (*p == quote)
				quote = 0;
			continue;
		}
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '\'' || *p == '"') {
			quote = *p;
			continue;
		}
		if (*p == '$' && p[1] == '(') {
			depth++;
			p++;
			continue;
		}
		if (depth) {
			if (*p == '(')
				depth++;
			else if (*p == ')')
				depth--;
			continue;
		}
		if (*p == '#' && (p == start || p[-1] == ' ' || p[-1] == '\t'))
			break;
		if (*p == ';') {
			*end = p;
			return T_SEMI;
		}
		if (p[0] == '&' && p[1] == '&') {
			*end = p;
			return T_AND;
		}
		if (p[0] == '|' && p[1] == '|') {
			*end = p;
			return T_OR;
		}
		if (p[0] == '&' && p[1] != '>' && (p == start || p[-1] != '>')) {
			*end = p;
			return T_AMP;
		}
	}
	*end = p;
	return T_END;
}

/* Runs a line command by command, so each sees the effects of the one before
 * ($? and $(...) are expanded just before a command runs). */
int run_line(const char *line)
{
	const char *p = line;
	bool skip = false;
	int status = last_status;
	while (*p && !want_exit) {
		const char *end;
		TokType sep = next_separator(p, &end);
		char *seg = str_ndup(p, (usize)(end - p));
		if (sep == T_AMP) {
			/* Keep the & so the command runs in the background. */
			char *bg = str_fmt("%s &", seg);
			mem_free(seg);
			seg = bg;
		}
		if (!skip) {
			Tokens toks = { 0 };
			if (!tokenize(seg, &toks)) {
				errf("flint: unfinished quote\n");
				status = 2;
			} else if (toks.len > 1) {
				status = run_tokens(toks.data, 0);
			}
			free_tokens(&toks);
			last_status = status;
		}
		mem_free(seg);
		if (sep == T_END)
			break;
		p = end + (sep == T_AND || sep == T_OR ? 2 : 1);
		skip = (sep == T_AND && status != 0) || (sep == T_OR && status == 0);
	}
	return status;
}

static char *capture_output(const char *line)
{
	int fds[2];
	if (sys_pipe2(fds, O_CLOEXEC) < 0)
		return str_dup("");
	out_flush();
	long pid = syscall5(SYS_clone, SIGCHLD, 0, 0, 0, 0);
	if (pid == 0) {
		sys_dup3(fds[1], 1, 0);
		interactive = false;
		int st = run_line(line);
		out_flush();
		syscall1(SYS_exit, st);
	}
	sys_close(fds[1]);
	StrBuf sb = { 0 };
	char buf[4096];
	for (;;) {
		long n = sys_read(fds[0], buf, sizeof buf);
		if (n == -EINTR)
			continue;
		if (n <= 0)
			break;
		sb_add(&sb, buf, (usize)n);
	}
	sys_close(fds[0]);
	int st;
	if (pid > 0)
		sys_wait4((int)pid, &st, 0, NULL);
	return sb_take(&sb);
}

int source_file(const char *path)
{
	char *text = file_read(path, NULL);
	if (!text)
		return 1;
	usize n;
	char **lines = str_split(text, '\n', &n);
	int st = 0;
	for (usize i = 0; i < n && !want_exit; i++)
		st = run_line(lines[i]);
	str_split_free(lines, n);
	mem_free(text);
	return st;
}

/* ---- Line editor -------------------------------------------------------------------------------- */

static struct termios saved_termios;

static void raw_mode(bool on)
{
	if (on) {
		sys_ioctl(tty, TCGETS, &saved_termios);
		struct termios t = saved_termios;
		t.c_iflag &= ~(u32)(ICRNL | IXON);
		t.c_lflag &= ~(u32)(ICANON | ECHO | ISIG | IEXTEN);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		sys_ioctl(tty, TCSETS, &t);
	} else {
		sys_ioctl(tty, TCSETS, &saved_termios);
	}
}

static int term_cols(void)
{
	struct winsize ws;
	if (sys_ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

static usize width_of(const char *s, usize n)
{
	/* Visible columns: skips escape sequences, one column per character. */
	usize w = 0;
	for (usize i = 0; i < n; i++) {
		if (s[i] == '\x1b') {
			i++;
			if (i < n && s[i] == '[')
				while (i < n && !is_alpha((u8)s[i]))
					i++;
			else if (i < n && s[i] == ']')
				while (i < n && s[i] != '\a')
					i++;
			continue;
		}
		if (((u8)s[i] & 0xC0) != 0x80)
			w++;
	}
	return w;
}

typedef struct {
	StrBuf buf;
	usize pos;            /* byte offset of the cursor */
	char prompt[512];
	usize prompt_w;
	int rows_drawn, cursor_row;
	int hist_index;       /* history.len = editing a new line */
	StrBuf saved;         /* the line being edited before browsing history */
	bool last_tab;
} Editor;

/* The newest history entry starting with the current text, for the grey hint. */
static const char *suggestion(Editor *e)
{
	if (!e->buf.len || e->pos != e->buf.len)
		return NULL;
	for (isize i = (isize)history.len - 1; i >= 0; i--) {
		const char *h = history.data[i];
		if (strlen(h) > e->buf.len && memcmp(h, e->buf.data, e->buf.len) == 0)
			return h + e->buf.len;
	}
	return NULL;
}

static bool is_command_word(const char *w, usize n)
{
	char name[128];
	if (n == 0 || n >= sizeof name)
		return false;
	memcpy(name, w, n);
	name[n] = 0;
	return find_command(name) || alias_get(name) || find_in_path(name);
}

static void refresh(Editor *e)
{
	int cols = term_cols();
	StrBuf o = { 0 };
	/* Back to where the prompt starts. */
	if (e->cursor_row > 0)
		sb_fmt(&o, "\x1b[%dA", e->cursor_row);
	sb_adds(&o, "\r");
	sb_adds(&o, e->prompt);
	/* The command name is green when it exists and red when it doesn't. */
	const char *s = sb_str(&e->buf);
	usize i = 0;
	while (i < e->buf.len && s[i] == ' ')
		i++;
	usize ws = i;
	while (i < e->buf.len && s[i] != ' ')
		i++;
	sb_add(&o, s, ws);
	if (i > ws) {
		sb_adds(&o, is_command_word(s + ws, i - ws) ? C_GREEN : C_RED);
		sb_add(&o, s + ws, i - ws);
		sb_adds(&o, C_RESET);
	}
	sb_add(&o, s + i, e->buf.len - i);
	const char *hint = suggestion(e);
	if (hint) {
		sb_adds(&o, C_GRAY);
		sb_adds(&o, hint);
		sb_adds(&o, C_RESET);
	}
	sb_adds(&o, "\x1b[J");
	usize total = e->prompt_w + width_of(s, e->buf.len) + (hint ? width_of(hint, strlen(hint)) : 0);
	usize cur = e->prompt_w + width_of(s, e->pos);
	int end_row = (int)(total / (usize)cols), cur_row = (int)(cur / (usize)cols), cur_col = (int)(cur % (usize)cols);
	/* At an exact multiple of the width the terminal hasn't wrapped yet. */
	if (total && total % (usize)cols == 0) {
		sb_adds(&o, "\r\n");
	}
	if (end_row > cur_row)
		sb_fmt(&o, "\x1b[%dA", end_row - cur_row);
	sb_adds(&o, "\r");
	if (cur_col)
		sb_fmt(&o, "\x1b[%dC", cur_col);
	e->cursor_row = cur_row;
	e->rows_drawn = end_row;
	write_all(1, o.data, o.len);
	sb_free(&o);
}

static void make_prompt(Editor *e)
{
	char cwd[512] = "?";
	sys_getcwd(cwd, sizeof cwd);
	const char *home = home_dir();
	char shown[512];
	if (str_starts(cwd, home) && (cwd[strlen(home)] == '/' || cwd[strlen(home)] == 0))
		fmt(shown, sizeof shown, "~%s", cwd + strlen(home));
	else
		str_copy(shown, cwd, sizeof shown);
	const char *user = env_get("USER");
	bool root = sys_geteuid() == 0;
	if (root)
		user = "root";
	/* The window title follows the folder. */
	char title[600];
	fmt(title, sizeof title, "\x1b]0;%s\a", shown);
	write_all(1, title, strlen(title));
	fmt(e->prompt, sizeof e->prompt, "%s%s" C_RESET C_GRAY "@zurbax " C_RESET C_BBLUE "%s" C_RESET " %s%s " C_RESET,
	    root ? "\x1b[1;31m" : C_BGREEN, user ? user : "user", shown, last_status ? C_RED : "\x1b[1;35m", root ? "#" : "❯");
	e->prompt_w = width_of(e->prompt, strlen(e->prompt));
}

static void insert_text(Editor *e, const char *s, usize n)
{
	sb_insert(&e->buf, e->pos, s, n);
	e->pos += n;
}

static usize prev_char(const Editor *e, usize p)
{
	if (p == 0)
		return 0;
	p--;
	while (p > 0 && ((u8)e->buf.data[p] & 0xC0) == 0x80)
		p--;
	return p;
}

static usize next_char(const Editor *e, usize p)
{
	if (p >= e->buf.len)
		return e->buf.len;
	p++;
	while (p < e->buf.len && ((u8)e->buf.data[p] & 0xC0) == 0x80)
		p++;
	return p;
}

static void set_line(Editor *e, const char *s)
{
	sb_clear(&e->buf);
	sb_adds(&e->buf, s);
	e->pos = e->buf.len;
}

/* Tab completion: commands for the first word, files and folders after it. */
static void complete(Editor *e)
{
	const char *s = sb_str(&e->buf);
	usize start = e->pos;
	while (start > 0 && s[start - 1] != ' ' && s[start - 1] != '|' && s[start - 1] != ';' && s[start - 1] != '&')
		start--;
	usize before = start;
	while (before > 0 && s[before - 1] == ' ')
		before--;
	bool first = before == 0 || str_chr("|;&", s[before - 1]);
	char word[512];
	usize wn = MIN(e->pos - start, sizeof word - 1);
	memcpy(word, s + start, wn);
	word[wn] = 0;
	StrVec cands = { 0 };
	char dir[512] = "", prefix[256];
	bool tilde = word[0] == '~';
	if (first && !str_chr(word, '/')) {
		for (int i = 0; i < COMMAND_COUNT; i++)
			if (str_starts(COMMANDS[i].name, word))
				vec_push(&cands, str_dup(COMMANDS[i].name));
		vec_each(&aliases, a) if (str_starts(a->name, word)) vec_push(&cands, str_dup(a->name));
		const char *path = env_get("PATH");
		if (!path)
			path = "/system/bin";
		while (*path) {
			const char *pe = str_chr(path, ':');
			usize n = pe ? (usize)(pe - path) : strlen(path);
			char d[512];
			fmt(d, sizeof d, "%.*s", (int)n, path);
			DirList l = { 0 };
			if (dir_list(d, &l, false)) {
				vec_each(&l, ent) if (str_starts(ent->name, word)) vec_push(&cands, str_dup(ent->name));
				dir_list_free(&l);
			}
			path += n;
			if (*path == ':')
				path++;
		}
		str_copy(prefix, word, sizeof prefix);
	} else {
		const char *w = word;
		char expanded[512];
		if (tilde) {
			fmt(expanded, sizeof expanded, "%s%s", home_dir(), word + 1);
			w = expanded;
		}
		const char *slash = str_rchr(w, '/');
		if (slash)
			fmt(dir, sizeof dir, "%.*s", (int)(slash - w + 1), w);
		str_copy(prefix, slash ? slash + 1 : w, sizeof prefix);
		DirList l = { 0 };
		if (dir_list(dir[0] ? dir : ".", &l, false)) {
			vec_each(&l, ent) {
				if (ent->name[0] == '.' && prefix[0] != '.')
					continue;
				if (str_starts(ent->name, prefix))
					vec_push(&cands, str_fmt("%s%s", ent->name, ent->type == DT_DIR ? "/" : ""));
			}
			dir_list_free(&l);
		}
	}
	if (cands.len == 0) {
		write_all(1, "\a", 1);
	} else {
		/* Longest common prefix of the candidates. */
		usize common = strlen(cands.data[0]);
		for (usize i = 1; i < cands.len; i++) {
			usize k = 0;
			while (k < common && cands.data[i][k] == cands.data[0][k])
				k++;
			common = k;
		}
		usize have = strlen(prefix);
		if (common > have) {
			insert_text(e, cands.data[0] + have, common - have);
			if (cands.len == 1 && cands.data[0][common - 1] != '/')
				insert_text(e, " ", 1);
		} else if (cands.len == 1) {
			if (cands.data[0][common - 1] != '/')
				insert_text(e, " ", 1);
		} else if (e->last_tab) {
			/* Second Tab: list the choices under the line. */
			sort(cands.data, cands.len, sizeof(char *), cmp_str, NULL);
			usize maxw = 0;
			for (usize i = 0; i < cands.len; i++)
				maxw = MAX(maxw, strlen(cands.data[i]));
			int cols = MAX(1, term_cols() / (int)(maxw + 2));
			StrBuf o = { 0 };
			sb_adds(&o, "\r\n");
			for (usize i = 0; i < cands.len && i < 200; i++) {
				sb_fmt(&o, "%-*s", (int)maxw + 2, cands.data[i]);
				if ((int)(i + 1) % cols == 0)
					sb_adds(&o, "\r\n");
			}
			if ((int)cands.len % cols)
				sb_adds(&o, "\r\n");
			write_all(1, o.data, o.len);
			sb_free(&o);
			e->cursor_row = 0;
		} else {
			write_all(1, "\a", 1);
		}
	}
	vec_each(&cands, c) mem_free(*c);
	vec_free(&cands);
}

static int read_byte(void)
{
	u8 c;
	for (;;) {
		long n = sys_read(tty, &c, 1);
		if (n == 1)
			return c;
		if (n == -EINTR)
			continue;
		return -1;
	}
}

/* Reads a line with editing. Returns NULL at end of input (Ctrl+D). */
static char *read_line(void)
{
	Editor e = { 0 };
	make_prompt(&e);
	e.hist_index = (int)history.len;
	raw_mode(true);
	refresh(&e);
	char *result = NULL;
	for (;;) {
		int c = read_byte();
		bool tab = false;
		if (c < 0) {
			break;
		} else if (c == '\r' || c == '\n') {
			/* Redraw the line plainly (without the grey hint) and move on. */
			result = sb_take(&e.buf);
			StrBuf o = { 0 };
			if (e.cursor_row > 0)
				sb_fmt(&o, "\x1b[%dA", e.cursor_row);
			sb_adds(&o, "\r");
			sb_adds(&o, e.prompt);
			sb_adds(&o, result);
			sb_adds(&o, "\x1b[J\r\n");
			write_all(1, o.data, o.len);
			sb_free(&o);
			break;
		} else if (c == 3) { /* Ctrl+C: cancel the line */
			write_all(1, "^C\r\n", 4);
			sb_clear(&e.buf);
			e.pos = 0;
			e.cursor_row = 0;
			last_status = 130;
			make_prompt(&e);
		} else if (c == 4) { /* Ctrl+D */
			if (e.buf.len == 0) {
				write_all(1, "\r\n", 2);
				break;
			}
			usize n = next_char(&e, e.pos);
			sb_remove(&e.buf, e.pos, n - e.pos);
		} else if (c == 127 || c == 8) {
			usize p = prev_char(&e, e.pos);
			sb_remove(&e.buf, p, e.pos - p);
			e.pos = p;
		} else if (c == 1) {
			e.pos = 0;
		} else if (c == 5) {
			e.pos = e.buf.len;
		} else if (c == 11) {
			sb_remove(&e.buf, e.pos, e.buf.len - e.pos);
		} else if (c == 21) {
			sb_remove(&e.buf, 0, e.pos);
			e.pos = 0;
		} else if (c == 23) { /* Ctrl+W: delete the previous word */
			usize p = e.pos;
			while (p > 0 && e.buf.data[p - 1] == ' ')
				p--;
			while (p > 0 && e.buf.data[p - 1] != ' ')
				p--;
			sb_remove(&e.buf, p, e.pos - p);
			e.pos = p;
		} else if (c == 12) { /* Ctrl+L */
			write_all(1, "\x1b[H\x1b[2J", 7);
			e.cursor_row = 0;
		} else if (c == '\t') {
			complete(&e);
			tab = true;
		} else if (c == 27) {
			int c1 = read_byte();
			if (c1 == '[' || c1 == 'O') {
				int c2 = read_byte();
				int num = 0;
				while (c2 >= '0' && c2 <= '9') {
					num = num * 10 + (c2 - '0');
					c2 = read_byte();
				}
				if (c2 == ';') { /* modifiers, e.g. Ctrl+arrow: 1;5C */
					read_byte();
					c2 = read_byte();
				}
				switch (c2) {
				case 'A':
				case 'B': {
					int dir = c2 == 'A' ? -1 : 1;
					if (e.hist_index == (int)history.len) {
						sb_clear(&e.saved);
						sb_add(&e.saved, e.buf.data ? e.buf.data : "", e.buf.len);
					}
					/* Browse entries that start with what was typed. */
					int i = e.hist_index;
					for (;;) {
						i += dir;
						if (i < 0) {
							i = e.hist_index;
							break;
						}
						if (i >= (int)history.len) {
							i = (int)history.len;
							break;
						}
						if (str_starts(history.data[i], sb_str(&e.saved)))
							break;
					}
					e.hist_index = i;
					set_line(&e, i < (int)history.len ? history.data[i] : sb_str(&e.saved));
					break;
				}
				case 'C': {
					const char *h = suggestion(&e);
					if (h)
						insert_text(&e, h, strlen(h));
					else
						e.pos = next_char(&e, e.pos);
					break;
				}
				case 'D': e.pos = prev_char(&e, e.pos); break;
				case 'H': e.pos = 0; break;
				case 'F': e.pos = e.buf.len; break;
				case '~':
					if (num == 1 || num == 7)
						e.pos = 0;
					else if (num == 4 || num == 8)
						e.pos = e.buf.len;
					else if (num == 3) {
						usize n = next_char(&e, e.pos);
						sb_remove(&e.buf, e.pos, n - e.pos);
					}
					break;
				}
			} else if (c1 == 'b' || c1 == 'f') { /* Alt+B / Alt+F: word left/right */
				if (c1 == 'b') {
					while (e.pos > 0 && e.buf.data[e.pos - 1] == ' ')
						e.pos--;
					while (e.pos > 0 && e.buf.data[e.pos - 1] != ' ')
						e.pos--;
				} else {
					while (e.pos < e.buf.len && e.buf.data[e.pos] == ' ')
						e.pos++;
					while (e.pos < e.buf.len && e.buf.data[e.pos] != ' ')
						e.pos++;
				}
			}
		} else if (c >= 32) {
			/* A UTF-8 character may span several bytes. */
			char ch[4] = { (char)c };
			int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
			for (int k = 1; k <= extra; k++)
				ch[k] = (char)read_byte();
			insert_text(&e, ch, (usize)(1 + extra));
		}
		e.last_tab = tab;
		refresh(&e);
	}
	raw_mode(false);
	sb_free(&e.buf);
	sb_free(&e.saved);
	return result;
}

/* ---- History ----------------------------------------------------------------------------------- */

static void history_add(const char *line)
{
	const char *p = line;
	while (*p == ' ')
		p++;
	if (!*p)
		return;
	if (history.len && str_eq(history.data[history.len - 1], line))
		return;
	vec_push(&history, str_dup(line));
	if (history_path[0])
		file_append(history_path, str_fmt("%s\n", line), strlen(line) + 1);
}

static void history_load(void)
{
	fmt(history_path, sizeof history_path, "%s/.local/share/zurbax/flint_history", home_dir());
	char *dir = path_dir(history_path);
	dir_make(dir, 0755);
	mem_free(dir);
	char *text = file_read(history_path, NULL);
	if (!text)
		return;
	usize n;
	char **lines = str_split(text, '\n', &n);
	usize first = n > 1000 ? n - 1000 : 0;
	for (usize i = first; i < n; i++)
		if (lines[i][0])
			vec_push(&history, str_dup(lines[i]));
	str_split_free(lines, n);
	mem_free(text);
}

/* ---- Main -------------------------------------------------------------------------------------- */

static void greet(void)
{
	char *path = str_fmt("%s/.local/share/zurbax/flint_greeted", home_dir());
	bool seen = file_exists(path);
	if (!seen)
		file_write_str(path, "1\n");
	mem_free(path);
	if (seen)
		return;
	out("\n  " C_BOLD "Welcome to Forge" C_RESET " — the Zurbax command line, running the " C_BOLD "Flint" C_RESET " shell.\n");
	out("  Type " C_BGREEN "help" C_RESET " to see what you can do, or " C_BGREEN "sysinfo" C_RESET " for a look at this computer.\n\n");
	out_flush();
}

int main(int argc, char **argv)
{
	/* flint -c "command" runs one line; flint script.fl runs a file. */
	if (argc > 2 && str_eq(argv[1], "-c"))
		return run_line(argv[2]);
	if (argc > 1 && argv[1][0] != '-') {
		int st = source_file(argv[1]);
		out_flush();
		return st;
	}
	struct termios t;
	interactive = sys_ioctl(0, TCGETS, &t) == 0;
	if (!env_get("PATH"))
		env_set("PATH", "/system/bin:/data/apps/bin");
	env_set("SHELL", "/system/bin/flint");
	if (interactive) {
		tty = 0;
		/* Own the terminal: run in our own group and ignore job-control signals. */
		shell_pgid = (int)sys_getpid();
		sys_setpgid(0, shell_pgid);
		give_terminal(shell_pgid);
		sig_ignore(SIGINT);
		sig_ignore(SIGQUIT);
		sig_ignore(SIGTSTP);
		sig_ignore(SIGTTIN);
		sig_ignore(SIGTTOU);
		history_load();
		alias_set("ll", "ls -l");
		alias_set("la", "ls -la");
		alias_set("..", "cd ..");
		char *rc = str_fmt("%s/.flintrc", home_dir());
		source_file(rc);
		mem_free(rc);
		const char *start = home_dir();
		char cwd[512];
		if (sys_getcwd(cwd, sizeof cwd) < 0 || str_eq(cwd, "/"))
			sys_chdir(start);
		greet();
		while (!want_exit) {
			reap_jobs();
			out_flush();
			char *line = read_line();
			if (!line)
				break;
			history_add(line);
			run_line(line);
			out_flush();
			mem_free(line);
		}
		return exit_status;
	}
	/* Not a terminal: read commands from standard input. */
	LineReader r;
	line_reader_init(&r, 0);
	const char *line;
	while (!want_exit && (line = line_reader_next(&r)))
		run_line(line);
	out_flush();
	return want_exit ? exit_status : last_status;
}
