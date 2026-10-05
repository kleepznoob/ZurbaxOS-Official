/*
 * Calculator — basic and scientific modes.
 *
 * The expression is kept as typed and evaluated as you go: the line below
 * shows the result before you press =. Supports precedence, parentheses
 * (closing ones may be left out), implicit multiplication (2π, 3(4+1)),
 * powers, roots, factorials, percentages, trigonometry in degrees or
 * radians, logarithms and the previous answer.
 */
#include "zui.h"

typedef struct {
	char expr[128];
	char result[64];
} HistoryItem;

typedef struct {
	StrBuf expr;
	char preview[64];
	bool preview_error;
	bool just_evaluated;     /* the expression shows a result; typing a digit starts over */
	f64 ans;
	bool extended;
	bool degrees;
	HistoryItem history[50];
	int nhistory;
} Calc;

static Calc calc;

/* ---- Evaluation -------------------------------------------------------------------- */

typedef struct {
	const char *p;
	const char *err;
	bool degrees;
	f64 ans;
} Parser;

static f64 parse_expr(Parser *ps);

static void skip_space(Parser *ps)
{
	while (*ps->p == ' ')
		ps->p++;
}

/* Matches a token (ASCII or UTF-8) and consumes it. */
static bool accept(Parser *ps, const char *tok)
{
	skip_space(ps);
	usize n = strlen(tok);
	if (memcmp(ps->p, tok, n) == 0) {
		ps->p += n;
		return true;
	}
	return false;
}

static bool starts_primary(Parser *ps)
{
	skip_space(ps);
	const char *p = ps->p;
	if (is_digit((u8)*p) || *p == '.' || *p == '(')
		return true;
	static const char *const starts[] = { "π", "e", "√", "sin", "cos", "tan", "asin", "acos", "atan", "ln", "log", "Ans" };
	for (usize i = 0; i < ARRAY_LEN(starts); i++)
		if (str_starts(p, starts[i]))
			return true;
	return false;
}

static f64 to_radians(Parser *ps, f64 v)
{
	return ps->degrees ? v * PI / 180 : v;
}

static f64 from_radians(Parser *ps, f64 v)
{
	return ps->degrees ? v * 180 / PI : v;
}

static f64 clean_trig(f64 v)
{
	/* sin(180°) should be 0, not 1.2e-16. */
	return fabs(v) < 1e-14 ? 0 : v;
}

static f64 parse_unary(Parser *ps);
static f64 parse_postfix_root(Parser *ps);

static f64 parse_group(Parser *ps)
{
	if (!accept(ps, "(")) {
		/* Function without parentheses: sin 30 */
		return parse_unary(ps);
	}
	f64 v = parse_expr(ps);
	if (!accept(ps, ")")) {
		skip_space(ps);
		if (*ps->p && !ps->err)
			ps->err = "Missing parenthesis";
	}
	return v;
}

static f64 parse_primary(Parser *ps)
{
	skip_space(ps);
	const char *p = ps->p;
	if (is_digit((u8)*p) || *p == '.') {
		const char *end;
		f64 v = str_to_float(p, &end);
		if (end == p) {
			ps->err = "Invalid number";
			return 0;
		}
		ps->p = end;
		return v;
	}
	if (accept(ps, "(")) {
		f64 v = parse_expr(ps);
		accept(ps, ")"); /* a missing ')' at the end is fine */
		return v;
	}
	if (accept(ps, "π") || accept(ps, "pi"))
		return PI;
	if (accept(ps, "Ans"))
		return ps->ans;
	if (accept(ps, "√"))
		return sqrt(parse_postfix_root(ps));
	static const struct {
		const char *name;
		int fn;
	} funcs[] = { { "asin", 1 }, { "acos", 2 }, { "atan", 3 }, { "sin", 4 }, { "cos", 5 }, { "tan", 6 }, { "ln", 7 }, { "log", 8 } };
	for (usize i = 0; i < ARRAY_LEN(funcs); i++) {
		if (!accept(ps, funcs[i].name))
			continue;
		f64 x = parse_group(ps);
		switch (funcs[i].fn) {
		case 1: return from_radians(ps, asin(x));
		case 2: return from_radians(ps, acos(x));
		case 3: return from_radians(ps, atan(x));
		case 4: return clean_trig(sin(to_radians(ps, x)));
		case 5: return clean_trig(cos(to_radians(ps, x)));
		case 6: {
			f64 c = cos(to_radians(ps, x));
			if (fabs(c) < 1e-14) {
				ps->err = "Undefined";
				return 0;
			}
			return clean_trig(sin(to_radians(ps, x)) / c);
		}
		case 7:
		case 8:
			if (x <= 0) {
				ps->err = "Undefined";
				return 0;
			}
			return funcs[i].fn == 7 ? log(x) : log10(x);
		}
	}
	if (accept(ps, "e"))
		return 2.718281828459045;
	if (!*ps->p)
		ps->err = "Incomplete";
	else
		ps->err = "Invalid expression";
	return 0;
}

static f64 factorial(Parser *ps, f64 v)
{
	if (v < 0 || v != floor(v) || v > 170) {
		ps->err = "Undefined";
		return 0;
	}
	f64 r = 1;
	for (int i = 2; i <= (int)v; i++)
		r *= i;
	return r;
}

static f64 parse_postfix(Parser *ps)
{
	f64 v = parse_primary(ps);
	for (;;) {
		if (accept(ps, "!"))
			v = factorial(ps, v);
		else if (accept(ps, "%"))
			v /= 100;
		else if (accept(ps, "²"))
			v *= v;
		else
			return v;
	}
}

/* The operand of √: binds tighter than ^ (√4^2 = 4), so read one postfix term. */
static f64 parse_postfix_root(Parser *ps)
{
	f64 v = parse_postfix(ps);
	if (v < 0 && !ps->err)
		ps->err = "Undefined";
	return v;
}

static f64 parse_power(Parser *ps)
{
	f64 base = parse_postfix(ps);
	if (accept(ps, "^")) {
		f64 e = parse_unary(ps); /* right associative: 2^3^2 = 2^9 */
		f64 r = pow(base, e);
		if (is_nan(r) && !ps->err)
			ps->err = "Undefined";
		return r;
	}
	return base;
}

static f64 parse_unary(Parser *ps)
{
	if (accept(ps, "−") || accept(ps, "-"))
		return -parse_unary(ps);
	if (accept(ps, "+"))
		return parse_unary(ps);
	return parse_power(ps);
}

static f64 parse_term(Parser *ps)
{
	f64 v = parse_unary(ps);
	while (!ps->err) {
		if (accept(ps, "×") || accept(ps, "*")) {
			v *= parse_unary(ps);
		} else if (accept(ps, "÷") || accept(ps, "/")) {
			f64 d = parse_unary(ps);
			if (d == 0) {
				ps->err = "Can't divide by zero";
				return 0;
			}
			v /= d;
		} else if (starts_primary(ps)) {
			v *= parse_power(ps); /* implicit: 2π, 3(4) */
		} else {
			break;
		}
	}
	return v;
}

static f64 parse_expr(Parser *ps)
{
	f64 v = parse_term(ps);
	while (!ps->err) {
		if (accept(ps, "+"))
			v += parse_term(ps);
		else if (accept(ps, "−") || accept(ps, "-"))
			v -= parse_term(ps);
		else
			break;
	}
	return v;
}

static bool evaluate(const char *s, f64 *out, const char **err)
{
	Parser ps = { .p = s, .degrees = calc.degrees, .ans = calc.ans };
	skip_space(&ps);
	if (!*ps.p) {
		*err = "";
		return false;
	}
	f64 v = parse_expr(&ps);
	skip_space(&ps);
	while (!ps.err && accept(&ps, ")"))
		skip_space(&ps);
	if (!ps.err && *ps.p)
		ps.err = "Invalid expression";
	if (!ps.err && (is_nan(v) || is_inf(v)))
		ps.err = is_inf(v) ? "Too large" : "Undefined";
	*err = ps.err;
	*out = v;
	return ps.err == NULL;
}

/* Formats with up to 12 significant digits and thousands separators. */
static void format_number(f64 v, char *out, usize cap, bool group)
{
	if (v == 0)
		v = 0; /* no "-0" */
	char raw[48];
	if (fabs(v) >= 1e15 || (fabs(v) < 1e-9 && v != 0))
		fmt(raw, sizeof raw, "%.10g", v);
	else
		fmt(raw, sizeof raw, "%.12g", v);
	if (!group || str_chr(raw, 'e')) {
		str_copy(out, raw, cap);
		return;
	}
	const char *digits = raw;
	StrBuf sb = { 0 };
	if (*digits == '-') {
		sb_adds(&sb, "−");
		digits++;
	}
	const char *dot = str_chr(digits, '.');
	usize int_len = dot ? (usize)(dot - digits) : strlen(digits);
	for (usize i = 0; i < int_len; i++) {
		if (i > 0 && (int_len - i) % 3 == 0)
			sb_addc(&sb, ',');
		sb_addc(&sb, digits[i]);
	}
	if (dot)
		sb_adds(&sb, dot);
	str_copy(out, sb_str(&sb), cap);
	sb_free(&sb);
}

static void update_preview(void)
{
	f64 v;
	const char *err;
	calc.preview[0] = 0;
	calc.preview_error = false;
	if (evaluate(sb_str(&calc.expr), &v, &err)) {
		char n[64];
		format_number(v, n, sizeof n, true);
		/* Showing "= 7" under a plain "7" adds nothing. */
		if (!str_eq(n, sb_str(&calc.expr)))
			fmt(calc.preview, sizeof calc.preview, "= %s", n);
	} else if (err && *err && !str_eq(err, "Incomplete")) {
		str_copy(calc.preview, err, sizeof calc.preview);
		calc.preview_error = true;
	}
}

/* ---- Editing -------------------------------------------------------------------------- */

static bool is_operator_text(const char *t)
{
	return str_eq(t, "+") || str_eq(t, "−") || str_eq(t, "×") || str_eq(t, "÷") || str_eq(t, "^") || str_eq(t, "%") ||
	       str_eq(t, "!") || str_eq(t, "²");
}

static void insert(const char *text)
{
	if (calc.just_evaluated) {
		/* After =, an operator continues from the result; anything else starts fresh. */
		if (!is_operator_text(text))
			sb_clear(&calc.expr);
		calc.just_evaluated = false;
	}
	/* Replace a trailing binary operator rather than stacking them. */
	static const char *const binops[] = { "+", "−", "×", "÷", "^" };
	bool new_is_binop = false;
	for (usize i = 0; i < ARRAY_LEN(binops); i++)
		new_is_binop |= str_eq(text, binops[i]) && !str_eq(text, "−");
	if (new_is_binop) {
		for (usize i = 0; i < ARRAY_LEN(binops); i++) {
			usize n = strlen(binops[i]);
			if (calc.expr.len >= n && memcmp(calc.expr.data + calc.expr.len - n, binops[i], n) == 0) {
				sb_remove(&calc.expr, calc.expr.len - n, n);
				break;
			}
		}
		if (calc.expr.len == 0)
			sb_adds(&calc.expr, "Ans");
	}
	sb_adds(&calc.expr, text);
	update_preview();
}

static void backspace(void)
{
	calc.just_evaluated = false;
	if (calc.expr.len == 0)
		return;
	/* Remove whole function names at once ("sin(" or "Ans"). */
	static const char *const words[] = { "asin(", "acos(", "atan(", "sin(", "cos(", "tan(", "ln(", "log(", "Ans" };
	for (usize i = 0; i < ARRAY_LEN(words); i++) {
		usize n = strlen(words[i]);
		if (calc.expr.len >= n && memcmp(calc.expr.data + calc.expr.len - n, words[i], n) == 0) {
			sb_remove(&calc.expr, calc.expr.len - n, n);
			update_preview();
			return;
		}
	}
	/* One UTF-8 character. */
	usize i = calc.expr.len - 1;
	while (i > 0 && ((u8)calc.expr.data[i] & 0xC0) == 0x80)
		i--;
	sb_remove(&calc.expr, i, calc.expr.len - i);
	update_preview();
}

static void clear_all(void)
{
	sb_clear(&calc.expr);
	calc.just_evaluated = false;
	update_preview();
}

static void equals(void)
{
	f64 v;
	const char *err;
	if (!evaluate(sb_str(&calc.expr), &v, &err)) {
		if (err && *err) {
			str_copy(calc.preview, err, sizeof calc.preview);
			calc.preview_error = true;
		}
		return;
	}
	char plain[64], grouped[64];
	format_number(v, plain, sizeof plain, false);
	format_number(v, grouped, sizeof grouped, true);
	if (calc.nhistory == (int)ARRAY_LEN(calc.history)) {
		memmove(calc.history, calc.history + 1, sizeof calc.history[0] * (ARRAY_LEN(calc.history) - 1));
		calc.nhistory--;
	}
	HistoryItem *h = &calc.history[calc.nhistory++];
	str_copy(h->expr, sb_str(&calc.expr), sizeof h->expr);
	str_copy(h->result, grouped, sizeof h->result);
	calc.ans = v;
	sb_clear(&calc.expr);
	/* Keep the result editable in plain form (no separators). */
	if (plain[0] == '-') {
		sb_adds(&calc.expr, "−");
		sb_adds(&calc.expr, plain + 1);
	} else {
		sb_adds(&calc.expr, plain);
	}
	calc.just_evaluated = true;
	calc.preview[0] = 0;
	calc.preview_error = false;
}

/* "( )": opens a parenthesis, or closes one when that makes sense. */
static void smart_paren(void)
{
	int open = 0;
	for (usize i = 0; i < calc.expr.len; i++)
		open += calc.expr.data[i] == '(' ? 1 : calc.expr.data[i] == ')' ? -1 : 0;
	char last = calc.expr.len ? calc.expr.data[calc.expr.len - 1] : 0;
	bool after_value = is_digit((u8)last) || last == ')' || last == '.' || (u8)last == 0x80 /* π */ || last == '!' ||
			   last == '%' || last == 's' /* Ans */ || last == 'e';
	insert(open > 0 && after_value ? ")" : "(");
}

static void negate(void)
{
	if (calc.expr.len == 0)
		return;
	calc.just_evaluated = false;
	if (str_starts(sb_str(&calc.expr), "−(") && calc.expr.data[calc.expr.len - 1] == ')') {
		sb_remove(&calc.expr, calc.expr.len - 1, 1);
		sb_remove(&calc.expr, 0, strlen("−("));
	} else {
		sb_insert(&calc.expr, 0, "−(", strlen("−("));
		sb_addc(&calc.expr, ')');
	}
	update_preview();
}

/* ---- Interface ------------------------------------------------------------------------- */

typedef enum { K_DIGIT, K_OP, K_FUNC, K_CLEAR, K_EQUALS } KeyKind;

typedef struct {
	const char *label;
	const char *insert;    /* text to insert; NULL for special actions */
	KeyKind kind;
	u32 action;            /* special: 1 paren, 2 backspace, 3 negate, 4 deg/rad */
} Key;

static const Key BASIC[5][4] = {
	{ { "C", NULL, K_CLEAR, 0 }, { "( )", NULL, K_OP, 1 }, { "%", "%", K_OP, 0 }, { "÷", "÷", K_OP, 0 } },
	{ { "7", "7", K_DIGIT, 0 }, { "8", "8", K_DIGIT, 0 }, { "9", "9", K_DIGIT, 0 }, { "×", "×", K_OP, 0 } },
	{ { "4", "4", K_DIGIT, 0 }, { "5", "5", K_DIGIT, 0 }, { "6", "6", K_DIGIT, 0 }, { "−", "−", K_OP, 0 } },
	{ { "1", "1", K_DIGIT, 0 }, { "2", "2", K_DIGIT, 0 }, { "3", "3", K_DIGIT, 0 }, { "+", "+", K_OP, 0 } },
	{ { "0", "0", K_DIGIT, 0 }, { ".", ".", K_DIGIT, 0 }, { "⌫", NULL, K_OP, 2 }, { "=", NULL, K_EQUALS, 0 } },
};

static const Key EXTENDED[5][3] = {
	{ { "sin", "sin(", K_FUNC, 0 }, { "cos", "cos(", K_FUNC, 0 }, { "tan", "tan(", K_FUNC, 0 } },
	{ { "ln", "ln(", K_FUNC, 0 }, { "log", "log(", K_FUNC, 0 }, { "√", "√", K_FUNC, 0 } },
	{ { "x²", "²", K_FUNC, 0 }, { "xʸ", "^", K_FUNC, 0 }, { "n!", "!", K_FUNC, 0 } },
	{ { "π", "π", K_FUNC, 0 }, { "e", "e", K_FUNC, 0 }, { "Ans", "Ans", K_FUNC, 0 } },
	{ { "±", NULL, K_FUNC, 3 }, { "(", "(", K_FUNC, 0 }, { ")", ")", K_FUNC, 0 } },
};

static void press_key(const Key *k)
{
	switch (k->kind) {
	case K_CLEAR: clear_all(); return;
	case K_EQUALS: equals(); return;
	default: break;
	}
	switch (k->action) {
	case 1: smart_paren(); return;
	case 2: backspace(); return;
	case 3: negate(); return;
	}
	if (k->insert)
		insert(k->insert);
}

/* A keypad button: flat number keys, tinted operators, accented equals. */
static bool keypad_button(Ui *ui, Rect r, const Key *k, int index)
{
	Theme *t = &ui->theme;
	ui_push_id(ui, index);
	UiId id = ui_id(ui, "key");
	UiInteract it = ui_interact(ui, id, r);
	ui_pop_id(ui);
	f32 hover = ui_fade(ui, id, it.hovered, 0.1f);
	Color top, bottom, text = t->text, border = t->button_border;
	switch (k->kind) {
	case K_EQUALS:
		top = color_lighten(t->accent, 0.08f);
		bottom = t->accent;
		text = t->on_accent;
		border = color_lighten(t->accent, -0.25f);
		break;
	case K_CLEAR:
		top = color_lighten(t->danger, 0.08f);
		bottom = t->danger;
		text = COLOR_WHITE;
		border = color_lighten(t->danger, -0.25f);
		break;
	case K_OP:
	case K_FUNC:
		top = color_mix(t->button_top, t->accent, t->dark ? 0.10f : 0.07f);
		bottom = color_mix(t->button_bottom, t->accent, t->dark ? 0.12f : 0.09f);
		break;
	default:
		top = t->button_top;
		bottom = t->button_bottom;
		break;
	}
	if (it.held) {
		top = color_lighten(top, -0.08f);
		bottom = color_lighten(bottom, -0.06f);
	} else if (hover > 0) {
		top = color_mix(top, color_lighten(top, 0.06f), hover);
	}
	ui_shadow(ui, r, 6, 3, 1, color_hex("#00000018"));
	ui_gradient(ui, r, 6, top, bottom);
	ui_border(ui, r, 6, 1, border);
	if (!it.held)
		ui_line(ui, r.x + 5, r.y + 1.5f, r.x + r.w - 5, r.y + 1.5f, 1, color_hex(t->dark ? "#ffffff14" : "#ffffffb0"));
	f32 size = k->kind == K_FUNC ? 15 : 20;
	if (str_eq(k->label, "⌫")) {
		ui_icon(ui, center_in(r, 22, 22), "edit-clear", text);
	} else {
		Font f = ui_font(ui, size, k->kind == K_DIGIT ? 400 : 500);
		ui_text_in(ui, f, r, k->label, text, ALIGN_CENTER);
	}
	return it.clicked;
}

static void handle_keys(Ui *ui)
{
	for (int i = 0; i < ui->nkeys; i++) {
		UiKey *k = &ui->keys[i];
		if (!k->pressed)
			continue;
		if (k->mods & MOD_CTRL) {
			if (k->code == KEY_C) {
				f64 v;
				const char *err;
				char out[64];
				if (evaluate(sb_str(&calc.expr), &v, &err)) {
					format_number(v, out, sizeof out, false);
					ui_clipboard_set(ui, out);
				}
			}
			continue;
		}
		switch (k->code) {
		case KEY_BACKSPACE: backspace(); continue;
		case KEY_ESC:
		case KEY_DELETE: clear_all(); continue;
		case KEY_ENTER:
		case KEY_KPENTER: equals(); continue;
		}
		const char *t = k->text;
		if (!*t)
			continue;
		if (is_digit((u8)*t) || str_eq(t, ".") || str_eq(t, "(") || str_eq(t, ")") || str_eq(t, "%") || str_eq(t, "!") ||
		    str_eq(t, "^") || str_eq(t, "+"))
			insert(t);
		else if (str_eq(t, ","))
			insert(".");
		else if (str_eq(t, "-"))
			insert("−");
		else if (str_eq(t, "*") || str_eq(t, "x"))
			insert("×");
		else if (str_eq(t, "/"))
			insert("÷");
		else if (str_eq(t, "="))
			equals();
		else if (str_eq(t, "p"))
			insert("π");
		else if (str_eq(t, "e"))
			insert("e");
		else if (str_eq(t, "s"))
			insert("sin(");
		else if (str_eq(t, "c"))
			insert("cos(");
		else if (str_eq(t, "t"))
			insert("tan(");
		else if (str_eq(t, "l"))
			insert("ln(");
		else if (str_eq(t, "r"))
			insert("√");
	}
}

static void draw_display(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	Rect area = pad2(r, 20, 10);
	/* The expression shrinks to fit rather than scrolling out of view. */
	const char *e = calc.expr.len ? sb_str(&calc.expr) : "0";
	f32 size = 38;
	Font f = ui_font(ui, size, 300);
	while (size > 18 && ui_text_width(ui, f, e) > area.w) {
		size -= 2;
		f = ui_font(ui, size, 300);
	}
	Rect line = cut_top(&area, area.h * 0.62f);
	f32 ew = ui_text_width(ui, f, e);
	ui_text(ui, f, line.x + line.w - ew, line.y + line.h - 4, e, calc.expr.len ? t->text : t->text_faint);
	Font pf = ui_font(ui, 17, 400);
	if (calc.preview[0]) {
		f32 pw = ui_text_width(ui, pf, calc.preview);
		ui_text(ui, pf, area.x + area.w - pw, area.y + font_cap_height(pf) + 8, calc.preview,
			calc.preview_error ? t->danger : t->text_dim);
	}
}

static void history_popover(Ui *ui, Rect anchor)
{
	Rect c;
	f32 h = clampf(60 + (f32)calc.nhistory * 52, 120, 420);
	if (!ui_popover_begin(ui, "history", anchor, 300, h, &c))
		return;
	Theme *t = &ui->theme;
	if (calc.nhistory == 0) {
		ui_placeholder(ui, c, "history", "No History", "Calculations appear here");
		ui_popover_end(ui);
		return;
	}
	Rect top = cut_top(&c, 34);
	ui_heading(ui, pad2(top, 8, 0), "History");
	if (ui_button_flat(ui, cut_right(&top, 70), "Clear"))
		calc.nhistory = 0;
	f32 y = ui_scroll_begin(ui, c, "history-scroll");
	for (int i = calc.nhistory - 1; i >= 0; i--) {
		HistoryItem *hi = &calc.history[i];
		Rect row = rect(c.x, y, c.w, 50);
		y += 50;
		ui_push_id(ui, i);
		UiInteract it = ui_row(ui, row, ui_id(ui, "h"), false);
		ui_pop_id(ui);
		Rect in = pad2(row, 10, 6);
		ui_text_in(ui, ui_font(ui, 12, 400), cut_top(&in, 16), hi->expr, t->text_dim, ALIGN_END);
		ui_text_in(ui, ui_font(ui, 16, 600), in, hi->result, t->text, ALIGN_END);
		if (it.clicked) {
			/* Insert the result (without separators) into the expression. */
			char plain[64];
			usize n = 0;
			for (const char *p = hi->result; *p && n < sizeof plain - 1; p++)
				if (*p != ',')
					plain[n++] = *p;
			plain[n] = 0;
			insert(plain);
			ui_popover_close(ui);
		}
	}
	ui_scroll_end(ui, y - c.y);
	ui_popover_end(ui);
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	handle_keys(ui);
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	Rect header = cut_top(&win, 46);
	Rect tools = ui_headerbar(ui, header, "", HEADER_FLAT | HEADER_NO_TITLE | HEADER_NO_MAXIMIZE);
	Rect hist_btn = cut_right(&tools, 34);
	if (ui_icon_button(ui, hist_btn, "history", "History"))
		ui_popover_toggle(ui, "history");
	cut_right(&tools, 4);
	bool ext = calc.extended;
	if (ui_icon_toggle(ui, cut_right(&tools, 34), "calculator", ext, "Scientific")) {
		calc.extended = !calc.extended;
		/* Grow or shrink the window to fit the extra column of keys. */
		ui_window_resize(ui, calc.extended ? (int)ui->width + 250 : MAX(300, (int)ui->width - 250), (int)ui->height);
	}
	if (calc.extended) {
		static const char *const units[] = { "Deg", "Rad" };
		int sel = calc.degrees ? 0 : 1;
		if (ui_segmented(ui, center_in(cut_left(&tools, 110), 100, 28), units, 2, &sel)) {
			calc.degrees = sel == 0;
			update_preview();
		}
	}

	/* Display: a recessed panel that reads like a calculator screen. */
	Rect disp = cut_top(&win, 118);
	disp = pad4(disp, 0, 14, 4, 14);
	ui_round(ui, disp, 10, t->view);
	ui_border(ui, disp, 10, 1, t->border);
	ui_line(ui, disp.x + 10, disp.y + 1.5f, disp.x + disp.w - 10, disp.y + 1.5f, 1, color_hex("#00000010"));
	draw_display(ui, disp);

	/* Keypad. */
	Rect pad_r = pad(win, 14);
	int cols = calc.extended ? 7 : 4;
	f32 gap = 8;
	f32 kw = (pad_r.w - gap * (f32)(cols - 1)) / (f32)cols;
	f32 kh = (pad_r.h - gap * 4) / 5;
	for (int row = 0; row < 5; row++) {
		for (int col = 0; col < cols; col++) {
			Rect kr = rect(pad_r.x + (f32)col * (kw + gap), pad_r.y + (f32)row * (kh + gap), kw, kh);
			const Key *k;
			if (calc.extended && col < 3)
				k = &EXTENDED[row][col];
			else
				k = &BASIC[row][calc.extended ? col - 3 : col];
			if (keypad_button(ui, kr, k, row * 10 + col))
				press_key(k);
		}
	}
	history_popover(ui, hist_btn);
}

static void init(Ui *ui)
{
	UNUSED(ui);
	calc.degrees = true;
}

int main(void)
{
	UiAppDesc d = { .id = "calculator", .title = "Calculator", .width = 340, .height = 500, .min_width = 300,
			.min_height = 440, .flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
