/* A small, strict JSON parser. */
#include "zr.h"

typedef struct {
	const char *s;
	const char *end;
	int depth;
	bool error;
} Parser;

static void skip_ws(Parser *p)
{
	while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
		p->s++;
}

static Json *new_node(JsonType t)
{
	Json *j = NEW(Json);
	j->type = t;
	return j;
}

static int hex4(const char *s)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		int d = is_digit(c) ? c - '0' : ((c | 32) >= 'a' && (c | 32) <= 'f') ? (c | 32) - 'a' + 10 : -1;
		if (d < 0)
			return -1;
		v = v * 16 + d;
	}
	return v;
}

static char *parse_string(Parser *p)
{
	if (p->s >= p->end || *p->s != '"') {
		p->error = true;
		return NULL;
	}
	p->s++;
	StrBuf sb = { 0 };
	while (p->s < p->end && *p->s != '"') {
		char c = *p->s++;
		if (c != '\\') {
			sb_addc(&sb, c);
			continue;
		}
		if (p->s >= p->end)
			break;
		c = *p->s++;
		switch (c) {
		case 'n': sb_addc(&sb, '\n'); break;
		case 't': sb_addc(&sb, '\t'); break;
		case 'r': sb_addc(&sb, '\r'); break;
		case 'b': sb_addc(&sb, '\b'); break;
		case 'f': sb_addc(&sb, '\f'); break;
		case 'u': {
			if (p->end - p->s < 4)
				goto fail;
			int cp = hex4(p->s);
			if (cp < 0)
				goto fail;
			p->s += 4;
			if (cp >= 0xd800 && cp < 0xdc00 && p->end - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u') {
				int lo = hex4(p->s + 2);
				if (lo >= 0xdc00 && lo < 0xe000) {
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					p->s += 6;
				}
			}
			sb_addcp(&sb, (u32)cp);
			break;
		}
		default: sb_addc(&sb, c); break;
		}
	}
	if (p->s >= p->end)
		goto fail;
	p->s++;
	return sb_take(&sb);
fail:
	p->error = true;
	sb_free(&sb);
	return NULL;
}

static Json *parse_value(Parser *p);

static void add_child(Json *parent, Json *child)
{
	if ((parent->count & (parent->count - 1)) == 0) /* grow at powers of two */
		parent->items = mem_realloc(parent->items, sizeof(Json *) * (parent->count ? parent->count * 2 : 4));
	parent->items[parent->count++] = child;
}

static Json *parse_container(Parser *p, bool object)
{
	if (++p->depth > 256) {
		p->error = true;
		return NULL;
	}
	Json *j = new_node(object ? JSON_OBJECT : JSON_ARRAY);
	p->s++;
	skip_ws(p);
	char close = object ? '}' : ']';
	if (p->s < p->end && *p->s == close) {
		p->s++;
		p->depth--;
		return j;
	}
	for (;;) {
		char *key = NULL;
		if (object) {
			skip_ws(p);
			key = parse_string(p);
			skip_ws(p);
			if (p->error || p->s >= p->end || *p->s != ':') {
				mem_free(key);
				goto fail;
			}
			p->s++;
		}
		Json *v = parse_value(p);
		if (!v) {
			mem_free(key);
			goto fail;
		}
		v->key = key;
		add_child(j, v);
		skip_ws(p);
		if (p->s < p->end && *p->s == ',') {
			p->s++;
			continue;
		}
		if (p->s < p->end && *p->s == close) {
			p->s++;
			break;
		}
		goto fail;
	}
	p->depth--;
	return j;
fail:
	p->error = true;
	json_free(j);
	return NULL;
}

static bool match(Parser *p, const char *word)
{
	usize n = strlen(word);
	if ((usize)(p->end - p->s) >= n && memcmp(p->s, word, n) == 0) {
		p->s += n;
		return true;
	}
	return false;
}

static Json *parse_value(Parser *p)
{
	skip_ws(p);
	if (p->s >= p->end) {
		p->error = true;
		return NULL;
	}
	char c = *p->s;
	if (c == '{' || c == '[')
		return parse_container(p, c == '{');
	if (c == '"') {
		char *s = parse_string(p);
		if (!s)
			return NULL;
		Json *j = new_node(JSON_STRING);
		j->string = s;
		return j;
	}
	if (match(p, "true") || match(p, "false")) {
		Json *j = new_node(JSON_BOOL);
		j->boolean = p->s[-1] == 'e' && p->s[-2] == 'u';
		return j;
	}
	if (match(p, "null"))
		return new_node(JSON_NULL);
	if (c == '-' || is_digit(c)) {
		const char *end;
		f64 v = str_to_float(p->s, &end);
		if (end == p->s) {
			p->error = true;
			return NULL;
		}
		p->s = end;
		Json *j = new_node(JSON_NUMBER);
		j->number = v;
		return j;
	}
	p->error = true;
	return NULL;
}

Json *json_parse(const char *text, usize len)
{
	Parser p = { text, text + len, 0, false };
	Json *j = parse_value(&p);
	if (p.error) {
		json_free(j);
		return NULL;
	}
	return j;
}

void json_free(Json *j)
{
	if (!j)
		return;
	for (usize i = 0; i < j->count; i++)
		json_free(j->items[i]);
	mem_free(j->items);
	mem_free(j->string);
	mem_free(j->key);
	mem_free(j);
}

Json *json_get(const Json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJECT)
		return NULL;
	for (usize i = 0; i < obj->count; i++)
		if (str_eq(obj->items[i]->key, key))
			return obj->items[i];
	return NULL;
}

Json *json_at(const Json *arr, usize i)
{
	if (!arr || (arr->type != JSON_ARRAY && arr->type != JSON_OBJECT) || i >= arr->count)
		return NULL;
	return arr->items[i];
}

Json *json_path(const Json *j, const char *path)
{
	char part[128];
	while (j && *path) {
		usize n = 0;
		while (path[n] && path[n] != '.' && n < sizeof part - 1) {
			part[n] = path[n];
			n++;
		}
		part[n] = 0;
		path += n;
		if (*path == '.')
			path++;
		if (j->type == JSON_ARRAY && is_digit(part[0]))
			j = json_at(j, (usize)str_to_int(part, NULL));
		else
			j = json_get(j, part);
	}
	return (Json *)j;
}

const char *json_str(const Json *j, const char *fallback)
{
	return j && j->type == JSON_STRING ? j->string : fallback;
}

f64 json_num(const Json *j, f64 fallback)
{
	if (!j)
		return fallback;
	if (j->type == JSON_NUMBER)
		return j->number;
	if (j->type == JSON_STRING)
		return str_to_float(j->string, NULL);
	return fallback;
}

bool json_bool(const Json *j, bool fallback)
{
	return j && j->type == JSON_BOOL ? j->boolean : fallback;
}

void sb_json_string(StrBuf *sb, const char *s)
{
	sb_addc(sb, '"');
	for (; *s; s++) {
		u8 c = (u8)*s;
		switch (c) {
		case '"': sb_adds(sb, "\\\""); break;
		case '\\': sb_adds(sb, "\\\\"); break;
		case '\n': sb_adds(sb, "\\n"); break;
		case '\r': sb_adds(sb, "\\r"); break;
		case '\t': sb_adds(sb, "\\t"); break;
		default:
			if (c < 0x20)
				sb_fmt(sb, "\\u%04x", c);
			else
				sb_addc(sb, (char)c);
		}
	}
	sb_addc(sb, '"');
}
