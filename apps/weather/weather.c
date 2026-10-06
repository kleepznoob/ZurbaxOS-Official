/*
 * Weather — the forecast for where you are, from Open-Meteo.
 *
 * The window is the sky: its colours follow the weather and the time of day.
 * Now, the next 24 hours, the week and the details, with icons painted
 * here rather than drawn from files. Finds places by name; on first start
 * it guesses the place from the time zone.
 */
#include "zui.h"
#include "zn.h"

#define FORECAST_URL "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
	"&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m,pressure_msl" \
	"&hourly=temperature_2m,weather_code,precipitation_probability,is_day" \
	"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,uv_index_max" \
	"&timezone=auto&timeformat=unixtime&forecast_days=7"
#define SEARCH_URL "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=6&language=en&format=json"

typedef struct {
	char name[96], region[128];
	f64 lat, lon;
} Place;

typedef struct {
	i64 fetched;
	int utc_offset;
	f32 temp, feels, humidity, wind, wind_dir, pressure;
	int code;
	bool day;
	int nhours;
	i64 hour_time[24];
	f32 hour_temp[24];
	int hour_code[24], hour_pop[24];
	bool hour_day[24];
	int ndays;
	i64 day_time[7], sunrise[7], sunset[7];
	f32 day_max[7], day_min[7], uv[7];
	int day_code[7], day_pop[7];
} Forecast;

enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };

typedef struct {
	Ui *ui;
	Conf *conf;
	char conf_path[512], cache_path[512];
	Place place;
	bool have_place;
	bool fahrenheit;
	Forecast *fc;
	char error[160];
	/* background work */
	int job;
	int job_kind;            /* 1 forecast, 2 search */
	char job_url[1024];
	char *job_body;
	char job_error[160];
	int pending_kind;        /* asked for while busy: runs next, newest wins */
	char pending_url[1024];
	/* searching */
	bool searching;
	u64 search_opened;       /* frame it opened in: that click isn't an outside click */
	StrBuf query;
	Place results[6];
	int nresults;
	bool search_done;
	f64 next_refresh;
} Weather;

static Weather wx;

/* ---- Conditions ------------------------------------------------------------------------------ */

enum { W_CLEAR, W_PARTLY, W_CLOUDY, W_FOG, W_DRIZZLE, W_RAIN, W_SNOW, W_STORM };

static int condition(int code)
{
	if (code <= 1)
		return W_CLEAR;
	if (code == 2)
		return W_PARTLY;
	if (code == 3)
		return W_CLOUDY;
	if (code == 45 || code == 48)
		return W_FOG;
	if (code >= 51 && code <= 57)
		return W_DRIZZLE;
	if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82))
		return W_RAIN;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return W_SNOW;
	if (code >= 95)
		return W_STORM;
	return W_CLOUDY;
}

static const char *condition_text(int code)
{
	switch (code) {
	case 0: return "Clear";
	case 1: return "Mostly Clear";
	case 2: return "Partly Cloudy";
	case 3: return "Overcast";
	case 45: return "Fog";
	case 48: return "Freezing Fog";
	case 51: case 53: return "Drizzle";
	case 55: return "Heavy Drizzle";
	case 56: case 57: return "Freezing Drizzle";
	case 61: return "Light Rain";
	case 63: return "Rain";
	case 65: return "Heavy Rain";
	case 66: case 67: return "Freezing Rain";
	case 71: return "Light Snow";
	case 73: return "Snow";
	case 75: return "Heavy Snow";
	case 77: return "Snow Grains";
	case 80: return "Showers";
	case 81: return "Rain Showers";
	case 82: return "Heavy Showers";
	case 85: case 86: return "Snow Showers";
	case 95: return "Thunderstorm";
	case 96: case 99: return "Thunderstorm with Hail";
	}
	return "Cloudy";
}

/* ---- Painted icons ----------------------------------------------------------------------------- */

static void paint_sun(Ui *ui, f32 cx, f32 cy, f32 r)
{
	for (int i = 3; i >= 1; i--)
		ui_circle(ui, cx, cy, r * (1 + 0.28f * (f32)i), color_hex(i == 1 ? "#ffd75a2a" : i == 2 ? "#ffd75a16" : "#ffd75a0a"));
	for (int k = 0; k < 8; k++) {
		f32 a = (f32)k * PI / 4;
		f32 c = cosf(a), s = sinf(a);
		ui_line(ui, cx + c * r * 1.28f, cy + s * r * 1.28f, cx + c * r * 1.55f, cy + s * r * 1.55f, maxf(1.5f, r * 0.13f), color_hex("#ffc53d"));
	}
	ui_gradient(ui, rect(cx - r, cy - r, 2 * r, 2 * r), r, color_hex("#ffe27a"), color_hex("#ffa81f"));
	ui_circle(ui, cx - r * 0.3f, cy - r * 0.32f, r * 0.34f, color_hex("#ffffff30"));
}

static void paint_moon(Ui *ui, f32 cx, f32 cy, f32 r)
{
	ui_circle(ui, cx, cy, r * 1.45f, color_hex("#dfe7ff12"));
	/* A crescent: the outer arc one way, an inner arc back. */
	Path p = { 0 };
	path_arc(&p, cx, cy, r, r, PI * 0.3f, PI * 1.7f);
	path_arc(&p, cx + r * 0.42f, cy, r * 0.82f, r * 0.82f, PI * 1.55f, PI * 0.45f);
	path_close(&p);
	ui_path_fill(ui, &p, color_hex("#f3f0dc"));
	path_free(&p);
	ui_circle(ui, cx - r * 0.62f, cy + r * 0.18f, r * 0.11f, color_hex("#d8d3b870"));
	ui_circle(ui, cx - r * 0.42f, cy - r * 0.5f, r * 0.08f, color_hex("#d8d3b870"));
}

static void cloud_shape(Ui *ui, f32 x, f32 y, f32 w, Color c)
{
	f32 h = w * 0.36f;
	ui_round(ui, rect(x, y + h * 0.35f, w, h * 0.65f), h * 0.32f, c);
	ui_circle(ui, x + w * 0.30f, y + h * 0.45f, w * 0.20f, c);
	ui_circle(ui, x + w * 0.56f, y + h * 0.28f, w * 0.27f, c);
	ui_circle(ui, x + w * 0.80f, y + h * 0.55f, w * 0.16f, c);
}

static void paint_cloud(Ui *ui, f32 x, f32 y, f32 w, bool dark)
{
	/* Underside shade, body, then a highlight on top for some volume. */
	cloud_shape(ui, x, y + w * 0.05f, w, color_hex(dark ? "#4a5468" : "#b9c6d8"));
	cloud_shape(ui, x, y, w, color_hex(dark ? "#6b7689" : "#f4f7fb"));
	ui_circle(ui, x + w * 0.52f, y + w * 0.02f, w * 0.13f, color_hex(dark ? "#ffffff10" : "#ffffffb0"));
}

static void paint_icon(Ui *ui, Rect r, int code, bool day)
{
	int c = condition(code);
	f32 s = minf(r.w, r.h), x = r.x + (r.w - s) / 2, y = r.y + (r.h - s) / 2;
	f32 cx = x + s / 2, cy = y + s / 2;
	if (c == W_CLEAR) {
		if (day)
			paint_sun(ui, cx, cy, s * 0.26f);
		else
			paint_moon(ui, cx, cy, s * 0.3f);
		return;
	}
	if (c == W_PARTLY) {
		if (day)
			paint_sun(ui, x + s * 0.62f, y + s * 0.36f, s * 0.2f);
		else
			paint_moon(ui, x + s * 0.64f, y + s * 0.34f, s * 0.21f);
		paint_cloud(ui, x + s * 0.08f, y + s * 0.42f, s * 0.72f, false);
		return;
	}
	if (c == W_FOG) {
		paint_cloud(ui, x + s * 0.12f, y + s * 0.18f, s * 0.76f, false);
		for (int i = 0; i < 3; i++)
			ui_round(ui, rect(x + s * (0.14f + 0.06f * (f32)(i % 2)), y + s * (0.62f + 0.11f * (f32)i), s * 0.66f, s * 0.055f), s * 0.03f,
				 color_hex("#e6ebf2c8"));
		return;
	}
	bool heavy = c == W_STORM || c == W_RAIN;
	paint_cloud(ui, x + s * 0.1f, y + s * 0.16f, s * 0.8f, heavy);
	if (c == W_CLOUDY) {
		paint_cloud(ui, x + s * 0.02f, y + s * 0.34f, s * 0.6f, false);
		return;
	}
	f32 top = y + s * 0.6f;
	if (c == W_RAIN || c == W_DRIZZLE || c == W_STORM) {
		int drops = c == W_DRIZZLE ? 3 : 4;
		for (int i = 0; i < drops; i++) {
			f32 dx = x + s * (0.28f + 0.15f * (f32)i);
			f32 len = s * (c == W_DRIZZLE ? 0.08f : 0.14f);
			ui_line(ui, dx, top + s * 0.04f * (f32)(i % 2), dx - len * 0.35f, top + len + s * 0.04f * (f32)(i % 2), maxf(1.4f, s * 0.04f),
				color_hex("#5fb2ff"));
		}
	}
	if (c == W_SNOW) {
		for (int i = 0; i < 4; i++) {
			f32 fx = x + s * (0.28f + 0.15f * (f32)i), fy = top + s * (0.06f + 0.08f * (f32)(i % 2));
			f32 fr = s * 0.045f;
			for (int k = 0; k < 3; k++) {
				f32 a = (f32)k * PI / 3;
				ui_line(ui, fx - cosf(a) * fr, fy - sinf(a) * fr, fx + cosf(a) * fr, fy + sinf(a) * fr, maxf(1, s * 0.018f), COLOR_WHITE);
			}
		}
	}
	if (c == W_STORM) {
		Path p = { 0 };
		f32 bx = x + s * 0.5f, by = y + s * 0.5f;
		path_move(&p, bx + s * 0.04f, by);
		path_line(&p, bx - s * 0.1f, by + s * 0.22f);
		path_line(&p, bx + s * 0.0f, by + s * 0.22f);
		path_line(&p, bx - s * 0.06f, by + s * 0.42f);
		path_line(&p, bx + s * 0.14f, by + s * 0.14f);
		path_line(&p, bx + s * 0.03f, by + s * 0.14f);
		path_line(&p, bx + s * 0.1f, by);
		path_close(&p);
		ui_path_fill(ui, &p, color_hex("#ffd23f"));
		path_free(&p);
	}
}

/* The sky behind everything. */
static void sky_colors(int code, bool day, Color *top, Color *bottom)
{
	int c = condition(code);
	if (!day) {
		*top = color_hex(c <= W_PARTLY ? "#0b1530" : "#151b28");
		*bottom = color_hex(c <= W_PARTLY ? "#2a3b6b" : "#2e3747");
		return;
	}
	switch (c) {
	case W_CLEAR: *top = color_hex("#2f7fd6"), *bottom = color_hex("#79bff0"); break;
	case W_PARTLY: *top = color_hex("#3d79bd"), *bottom = color_hex("#8db7de"); break;
	case W_CLOUDY: case W_FOG: *top = color_hex("#5d6f86"), *bottom = color_hex("#9aa9bb"); break;
	case W_SNOW: *top = color_hex("#7d93ad"), *bottom = color_hex("#c9d6e3"); break;
	default: *top = color_hex("#33435a"), *bottom = color_hex("#5f7189"); break;
	}
}

/* ---- Data ---------------------------------------------------------------------------------- */

static f32 show_temp(f32 c)
{
	return wx.fahrenheit ? c * 9 / 5 + 32 : c;
}

static void parse_forecast(const char *body, usize len, Forecast *f)
{
	memset(f, 0, sizeof *f);
	Json *j = json_parse(body, len);
	if (!j)
		return;
	f->utc_offset = (int)json_num(json_get(j, "utc_offset_seconds"), 0);
	Json *cur = json_get(j, "current");
	f->temp = (f32)json_num(json_get(cur, "temperature_2m"), 0);
	f->feels = (f32)json_num(json_get(cur, "apparent_temperature"), f->temp);
	f->humidity = (f32)json_num(json_get(cur, "relative_humidity_2m"), 0);
	f->wind = (f32)json_num(json_get(cur, "wind_speed_10m"), 0);
	f->wind_dir = (f32)json_num(json_get(cur, "wind_direction_10m"), 0);
	f->pressure = (f32)json_num(json_get(cur, "pressure_msl"), 0);
	f->code = (int)json_num(json_get(cur, "weather_code"), 3);
	f->day = json_num(json_get(cur, "is_day"), 1) != 0;
	i64 now = (i64)json_num(json_get(cur, "time"), (f64)time_unix());
	Json *h = json_get(j, "hourly");
	Json *ht = json_get(h, "time");
	usize start = 0;
	while (ht && start + 1 < ht->count && (i64)json_num(json_at(ht, start + 1), 0) <= now)
		start++;
	for (usize i = start; ht && i < ht->count && f->nhours < 24; i++) {
		int k = f->nhours++;
		f->hour_time[k] = (i64)json_num(json_at(ht, i), 0);
		f->hour_temp[k] = (f32)json_num(json_at(json_get(h, "temperature_2m"), i), 0);
		f->hour_code[k] = (int)json_num(json_at(json_get(h, "weather_code"), i), 3);
		f->hour_pop[k] = (int)json_num(json_at(json_get(h, "precipitation_probability"), i), 0);
		f->hour_day[k] = json_num(json_at(json_get(h, "is_day"), i), 1) != 0;
	}
	Json *d = json_get(j, "daily");
	Json *dt = json_get(d, "time");
	for (usize i = 0; dt && i < dt->count && f->ndays < 7; i++) {
		int k = f->ndays++;
		f->day_time[k] = (i64)json_num(json_at(dt, i), 0);
		f->day_max[k] = (f32)json_num(json_at(json_get(d, "temperature_2m_max"), i), 0);
		f->day_min[k] = (f32)json_num(json_at(json_get(d, "temperature_2m_min"), i), 0);
		f->day_code[k] = (int)json_num(json_at(json_get(d, "weather_code"), i), 3);
		f->day_pop[k] = (int)json_num(json_at(json_get(d, "precipitation_probability_max"), i), 0);
		f->sunrise[k] = (i64)json_num(json_at(json_get(d, "sunrise"), i), 0);
		f->sunset[k] = (i64)json_num(json_at(json_get(d, "sunset"), i), 0);
		f->uv[k] = (f32)json_num(json_at(json_get(d, "uv_index_max"), i), 0);
	}
	f->fetched = time_unix();
	json_free(j);
}

static void job_thread(void *arg)
{
	UNUSED(arg);
	HttpResponse *r = http_get(wx.job_url);
	if (r->status == 200) {
		wx.job_body = (char *)r->body;
		r->body = NULL;
	} else {
		str_copy(wx.job_error, r->status ? "The weather service isn't answering right now." : r->error, sizeof wx.job_error);
	}
	http_response_free(r);
	atomic_store(&wx.job, JOB_DONE);
	ui_wake(wx.ui);
}

static void start_job(int kind, const char *url)
{
	if (atomic_load(&wx.job) != JOB_IDLE) {
		/* A forecast request outranks a search waiting behind it. */
		if (!(wx.pending_kind == 1 && kind == 2)) {
			wx.pending_kind = kind;
			str_copy(wx.pending_url, url, sizeof wx.pending_url);
		}
		return;
	}
	str_copy(wx.job_url, url, sizeof wx.job_url);
	wx.job_kind = kind;
	wx.job_body = NULL;
	wx.job_error[0] = 0;
	atomic_store(&wx.job, JOB_RUNNING);
	if (thread_spawn(job_thread, NULL) < 0)
		atomic_store(&wx.job, JOB_IDLE);
}

static void refresh(void)
{
	if (!wx.have_place)
		return;
	char url[1024];
	fmt(url, sizeof url, FORECAST_URL, wx.place.lat, wx.place.lon);
	start_job(1, url);
}

static void search(const char *q)
{
	char *enc = url_encode(q);
	char url[512];
	fmt(url, sizeof url, SEARCH_URL, enc);
	mem_free(enc);
	wx.nresults = 0;
	wx.search_done = false;
	start_job(2, url);
}

static void save_place(void)
{
	conf_set(wx.conf, "name", wx.place.name);
	conf_set(wx.conf, "region", wx.place.region);
	char v[32];
	fmt(v, sizeof v, "%.4f", wx.place.lat);
	conf_set(wx.conf, "lat", v);
	fmt(v, sizeof v, "%.4f", wx.place.lon);
	conf_set(wx.conf, "lon", v);
	conf_save(wx.conf);
}

static void job_finished(Ui *ui)
{
	atomic_store(&wx.job, JOB_IDLE);
	if (wx.job_kind == 1) {
		if (wx.job_body) {
			Forecast *f = NEW(Forecast);
			parse_forecast(wx.job_body, strlen(wx.job_body), f);
			if (f->ndays) {
				mem_free(wx.fc);
				wx.fc = f;
				wx.error[0] = 0;
				file_write(wx.cache_path, wx.job_body, strlen(wx.job_body));
			} else {
				mem_free(f);
				str_copy(wx.error, "The forecast couldn't be read.", sizeof wx.error);
			}
		} else {
			str_copy(wx.error, wx.job_error, sizeof wx.error);
		}
		wx.next_refresh = ui->now + (wx.error[0] ? 60 : 15 * 60);
	} else {
		wx.search_done = true;
		Json *j = wx.job_body ? json_parse(wx.job_body, strlen(wx.job_body)) : NULL;
		Json *res = json_get(j, "results");
		for (usize i = 0; res && i < res->count && wx.nresults < 6; i++) {
			Json *e = json_at(res, i);
			Place *p = &wx.results[wx.nresults++];
			str_copy(p->name, json_str(json_get(e, "name"), "?"), sizeof p->name);
			const char *admin = json_str(json_get(e, "admin1"), ""), *country = json_str(json_get(e, "country"), "");
			fmt(p->region, sizeof p->region, "%s%s%s", admin, admin[0] && country[0] ? ", " : "", country);
			p->lat = json_num(json_get(e, "latitude"), 0);
			p->lon = json_num(json_get(e, "longitude"), 0);
		}
		json_free(j);
		/* First start: take the time zone's city straight away. */
		if (!wx.have_place && wx.nresults) {
			wx.place = wx.results[0];
			wx.have_place = true;
			save_place();
			refresh();
		}
	}
	mem_free(wx.job_body);
	wx.job_body = NULL;
	ui->redraw = true;
	if (wx.pending_kind) {
		int kind = wx.pending_kind;
		wx.pending_kind = 0;
		start_job(kind, wx.pending_url);
	}
}

/* ---- Drawing helpers --------------------------------------------------------------------------- */

static void text_shadowed(Ui *ui, Font f, Rect r, const char *s, Color c, Align a)
{
	ui_text_in(ui, f, rect(r.x, r.y + 1, r.w, r.h), s, color_hex("#00000030"), a);
	ui_text_in(ui, f, r, s, c, a);
}

/* Light icons for the sky-coloured header. */
static bool header_button(Ui *ui, Rect r, const char *icon, const char *tip)
{
	UiInteract it = ui_interact(ui, ui_id(ui, icon), r);
	f32 h = ui_fade(ui, ui_id(ui, icon), it.hovered, 0.12f);
	ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, color_scale_alpha(color_hex("#ffffff40"), h + (it.held ? 0.5f : 0)));
	ui_icon(ui, center_in(r, 18, 18), icon, COLOR_WHITE);
	if (it.hovered)
		ui_tooltip(ui, tip);
	return it.clicked;
}

/* A rounded bar coloured left to right. */
static void hbar(Ui *ui, Rect r, Color a, Color b)
{
	int n = MAX(2, (int)(r.w / 3));
	ui_push_clip(ui, r);
	for (int i = 0; i < n; i++) {
		f32 t = (f32)i / (f32)(n - 1);
		Rect s = rect(r.x + r.w * (f32)i / (f32)n, r.y, r.w / (f32)n + 1, r.h);
		ui_fill(ui, s, color_mix(a, b, t));
	}
	ui_pop_clip(ui);
	/* Round the ends over the slices. */
	ui_circle(ui, r.x + r.h / 2, r.y + r.h / 2, r.h / 2, a);
	ui_circle(ui, r.x + r.w - r.h / 2, r.y + r.h / 2, r.h / 2, b);
}

static void frosted(Ui *ui, Rect r)
{
	ui_round(ui, r, 14, color_hex("#ffffff1c"));
	ui_border(ui, r, 14, 1, color_hex("#ffffff26"));
}

static void local_civil(i64 t, Civil *c)
{
	civil_from_unix(t + (wx.fc ? wx.fc->utc_offset : 0), c);
}

static const char *weekday(i64 t)
{
	static const char *const names[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
	i64 days = (t + (wx.fc ? wx.fc->utc_offset : 0)) / 86400;
	return names[((days % 7) + 7) % 7];
}

static void fmt_time(char *out, usize cap, i64 t)
{
	Civil c;
	local_civil(t, &c);
	fmt(out, cap, "%02d:%02d", c.hour, c.min);
}

/* ---- Sections ------------------------------------------------------------------------------------ */

static void draw_hero(Ui *ui, Rect r)
{
	Forecast *f = wx.fc;
	Color white = COLOR_WHITE, dim = color_hex("#ffffffc0");
	Rect icon = cut_right(&r, minf(220, r.w * 0.38f));
	paint_icon(ui, pad(icon, 6), f->code, f->day);
	text_shadowed(ui, ui_font(ui, 22, 700), cut_top(&r, 32), wx.place.name, white, ALIGN_START);
	char line[160];
	Civil c;
	local_civil(time_unix(), &c);
	static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
	fmt(line, sizeof line, "%s %d %s · %02d:%02d", weekday(time_unix()), c.day, months[CLAMP(c.month, 1, 12) - 1], c.hour, c.min);
	text_shadowed(ui, ui_font(ui, 14, 500), cut_top(&r, 22), line, dim, ALIGN_START);
	char temp[16];
	fmt(temp, sizeof temp, "%.0f°", (double)show_temp(f->temp));
	text_shadowed(ui, ui_font(ui, 84, 300), cut_top(&r, 92), temp, white, ALIGN_START);
	text_shadowed(ui, ui_font(ui, 20, 600), cut_top(&r, 28), condition_text(f->code), white, ALIGN_START);
	if (f->ndays) {
		fmt(line, sizeof line, "H %.0f°   L %.0f°   ·   Feels like %.0f°", (double)show_temp(f->day_max[0]), (double)show_temp(f->day_min[0]),
		    (double)show_temp(f->feels));
		text_shadowed(ui, ui_font(ui, 14, 500), cut_top(&r, 22), line, dim, ALIGN_START);
	}
}

static void draw_hours(Ui *ui, Rect r)
{
	Forecast *f = wx.fc;
	frosted(ui, r);
	Rect in = pad2(r, 10, 10);
	f32 col = 64;
	f32 y = ui_scroll_begin(ui, in, "hours");
	UNUSED(y);
	ui_push_clip(ui, in);
	for (int i = 0; i < f->nhours; i++) {
		Rect c = rect(in.x + (f32)i * col, in.y, col, in.h);
		char t[16];
		if (i == 0)
			str_copy(t, "Now", sizeof t);
		else {
			Civil cv;
			local_civil(f->hour_time[i], &cv);
			fmt(t, sizeof t, "%02d", cv.hour);
		}
		ui_text_in(ui, ui_font(ui, 13, i == 0 ? 700 : 500), rect(c.x, c.y + 2, c.w, 18), t, COLOR_WHITE, ALIGN_CENTER);
		paint_icon(ui, rect(c.x + 14, c.y + 24, 36, 36), f->hour_code[i], f->hour_day[i]);
		if (f->hour_pop[i] >= 20) {
			char p[8];
			fmt(p, sizeof p, "%d%%", f->hour_pop[i]);
			ui_text_in(ui, ui_font(ui, 11, 700), rect(c.x, c.y + 60, c.w, 14), p, color_hex("#9fd3ff"), ALIGN_CENTER);
		}
		char tt[12];
		fmt(tt, sizeof tt, "%.0f°", (double)show_temp(f->hour_temp[i]));
		ui_text_in(ui, ui_font(ui, 15, 600), rect(c.x, c.y + in.h - 22, c.w, 20), tt, COLOR_WHITE, ALIGN_CENTER);
	}
	ui_pop_clip(ui);
	/* Horizontal content: wheel scrolling moves it sideways. */
	ui_scroll_end(ui, in.h);
}

static void draw_days(Ui *ui, Rect r)
{
	Forecast *f = wx.fc;
	frosted(ui, r);
	Rect in = pad2(r, 16, 10);
	ui_text_in(ui, ui_font(ui, 12, 700), cut_top(&in, 22), "7-DAY FORECAST", color_hex("#ffffffa0"), ALIGN_START);
	f32 lo = 1e9f, hi = -1e9f;
	for (int i = 0; i < f->ndays; i++)
		lo = minf(lo, f->day_min[i]), hi = maxf(hi, f->day_max[i]);
	f32 rh = in.h / (f32)MAX(f->ndays, 1);
	for (int i = 0; i < f->ndays; i++) {
		Rect row = cut_top(&in, rh);
		if (i)
			ui_line(ui, row.x, row.y, row.x + row.w, row.y, 1, color_hex("#ffffff1a"));
		Rect name = cut_left(&row, 58);
		ui_text_in(ui, ui_font(ui, 15, 600), name, i == 0 ? "Today" : weekday(f->day_time[i]), COLOR_WHITE, ALIGN_START);
		paint_icon(ui, center_in(cut_left(&row, 34), 30, 30), f->day_code[i], true);
		Rect pop = cut_left(&row, 44);
		if (f->day_pop[i] >= 20) {
			char p[8];
			fmt(p, sizeof p, "%d%%", f->day_pop[i]);
			ui_text_in(ui, ui_font(ui, 12, 700), pop, p, color_hex("#9fd3ff"), ALIGN_CENTER);
		}
		char a[12], b[12];
		fmt(a, sizeof a, "%.0f°", (double)show_temp(f->day_min[i]));
		fmt(b, sizeof b, "%.0f°", (double)show_temp(f->day_max[i]));
		ui_text_in(ui, ui_font(ui, 15, 500), cut_left(&row, 40), a, color_hex("#ffffffb0"), ALIGN_END);
		ui_text_in(ui, ui_font(ui, 15, 600), cut_right(&row, 40), b, COLOR_WHITE, ALIGN_END);
		/* The day's range on the week's scale, coloured cool to warm. */
		Rect bar = center_in(pad2(row, 10, 0), row.w - 20, 5);
		ui_round(ui, bar, 2.5f, color_hex("#00000026"));
		f32 span = maxf(1, hi - lo);
		f32 x0 = bar.x + bar.w * (f->day_min[i] - lo) / span, x1 = bar.x + bar.w * (f->day_max[i] - lo) / span;
		Color c0 = color_mix(color_hex("#6cc4ff"), color_hex("#ffc83d"), (f->day_min[i] - lo) / span);
		Color c1 = color_mix(color_hex("#7fd6a0"), color_hex("#ff8a3d"), (f->day_max[i] - lo) / span);
		hbar(ui, rect(x0, bar.y, maxf(6, x1 - x0), bar.h), c0, c1);
	}
}

static void detail_tile(Ui *ui, Rect r, const char *label, const char *value, const char *note)
{
	frosted(ui, r);
	Rect in = pad2(r, 14, 12);
	ui_text_in(ui, ui_font(ui, 11, 700), cut_top(&in, 16), label, color_hex("#ffffffa0"), ALIGN_START);
	cut_top(&in, 4);
	ui_text_in(ui, ui_font(ui, 24, 600), cut_top(&in, 32), value, COLOR_WHITE, ALIGN_START);
	if (note)
		ui_text_wrapped(ui, ui_font(ui, 12, 500), in, note, color_hex("#ffffffc0"), ALIGN_START);
}

static void draw_details(Ui *ui, Rect r)
{
	Forecast *f = wx.fc;
	char v[6][48], n[6][96];
	fmt(v[0], 48, "%.0f°", (double)show_temp(f->feels));
	str_copy(n[0], f->feels < f->temp - 1 ? "Wind makes it feel cooler." : f->feels > f->temp + 1 ? "Humidity makes it feel warmer." : "Similar to the actual temperature.", 96);
	fmt(v[1], 48, "%.0f%%", (double)f->humidity);
	str_copy(n[1], f->humidity > 75 ? "Damp air today." : f->humidity < 30 ? "Dry air today." : "Comfortable.", 96);
	static const char *const dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	fmt(v[2], 48, "%.0f km/h", (double)f->wind);
	fmt(n[2], 96, "From the %s.", dirs[((int)((f->wind_dir + 22.5f) / 45) % 8 + 8) % 8]);
	f32 uv = f->ndays ? f->uv[0] : 0;
	fmt(v[3], 48, "%.0f", (double)uv);
	str_copy(n[3], uv >= 8 ? "Very high. Avoid the midday sun." : uv >= 6 ? "High. Sunscreen recommended." : uv >= 3 ? "Moderate." : "Low.", 96);
	char rise[8] = "--", set[8] = "--";
	if (f->ndays) {
		fmt_time(rise, sizeof rise, f->sunrise[0]);
		fmt_time(set, sizeof set, f->sunset[0]);
	}
	str_copy(v[4], rise, 48);
	fmt(n[4], 96, "Sunset at %s.", set);
	fmt(v[5], 48, "%.0f hPa", (double)f->pressure);
	str_copy(n[5], f->pressure < 1005 ? "Low: unsettled weather." : f->pressure > 1022 ? "High: settled weather." : "Normal.", 96);
	static const char *const labels[] = { "FEELS LIKE", "HUMIDITY", "WIND", "UV INDEX", "SUNRISE", "PRESSURE" };
	for (int i = 0; i < 6; i++)
		detail_tile(ui, grid_cell(r, 2, 3, i, 12), labels[i], v[i], n[i]);
}

/* ---- Search ---------------------------------------------------------------------------------- */

static void draw_search(Ui *ui, Rect area)
{
	Theme *t = &ui->theme;
	Rect box = rect(area.x + area.w - 360, area.y + 4, 340, 0);
	int rows = wx.nresults;
	box.h = 52 + (f32)(rows ? rows * 52 : 40);
	ui_shadow(ui, box, 12, 24, 6, color_hex("#00000050"));
	ui_round(ui, box, 12, t->window);
	Rect in = pad(box, 8);
	int ev = ui_entry(ui, cut_top(&in, 36), "q", &wx.query, "Find a city", ENTRY_SEARCH | ENTRY_AUTOFOCUS);
	if (ev & ENTRY_CHANGED && wx.query.len >= 2)
		search(sb_str(&wx.query));
	if (ev & ENTRY_ESCAPE)
		wx.searching = false;
	cut_top(&in, 8);
	if (!rows) {
		const char *msg = atomic_load(&wx.job) == JOB_RUNNING ? "Searching…" : wx.query.len < 2 ? "Type a city name" : wx.search_done ? "No places found" : "";
		ui_text_in(ui, ui_font(ui, 13, 400), cut_top(&in, 30), msg, t->text_dim, ALIGN_CENTER);
	}
	for (int i = 0; i < rows; i++) {
		ui_push_id(ui, i);
		Rect row = cut_top(&in, 52);
		UiInteract it = ui_row(ui, row, ui_id(ui, "place"), false);
		ui_icon(ui, rect(row.x + 10, row.y + 17, 18, 18), "location", t->text_dim);
		ui_text_in(ui, ui_font(ui, 14, 600), rect(row.x + 38, row.y + 7, row.w - 44, 20), wx.results[i].name, t->text, ALIGN_START);
		ui_text_in(ui, ui_font(ui, 12, 400), rect(row.x + 38, row.y + 27, row.w - 44, 18), wx.results[i].region, t->text_dim, ALIGN_START);
		if (it.clicked || (i == 0 && (ev & ENTRY_SUBMIT))) {
			wx.place = wx.results[i];
			wx.have_place = true;
			save_place();
			mem_free(wx.fc);
			wx.fc = NULL;
			wx.searching = false;
			sb_clear(&wx.query);
			wx.nresults = 0;
			refresh();
		}
		ui_pop_id(ui);
	}
	if (ui->pressed[1] && !ui_mouse_in(ui, box) && ui->frame_no > wx.search_opened)
		wx.searching = false;
}

/* ---- Frame ----------------------------------------------------------------------------------- */

static void frame(Ui *ui)
{
	if (atomic_load(&wx.job) == JOB_DONE)
		job_finished(ui);
	if (wx.have_place && ui->now >= wx.next_refresh && atomic_load(&wx.job) == JOB_IDLE) {
		wx.next_refresh = ui->now + 60;
		refresh();
	}
	ui->wake_at = wx.next_refresh;
	Color top, bottom;
	sky_colors(wx.fc ? wx.fc->code : 2, wx.fc ? wx.fc->day : true, &top, &bottom);
	ui_window_background(ui, bottom);
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_gradient(ui, win, 0, top, bottom);
	Rect header = cut_top(&win, 46);
	/* Window controls in white over the sky. */
	Theme saved = ui->theme;
	ui->theme.text = ui->theme.text_dim = COLOR_WHITE;
	ui->theme.hover = color_hex("#ffffff30");
	ui_headerbar(ui, header, "", HEADER_TRANSPARENT | HEADER_FLAT | HEADER_NO_TITLE);
	ui->theme = saved;
	Rect hb = pad4(header, 8, 54, 8, 54);
	if (header_button(ui, cut_right(&hb, 30), "search", "Find a place")) {
		wx.searching = !wx.searching;
		wx.search_opened = ui->frame_no;
		sb_clear(&wx.query);
		wx.nresults = 0;
	}
	cut_right(&hb, 4);
	if (header_button(ui, cut_right(&hb, 30), "refresh", "Refresh"))
		refresh();
	cut_right(&hb, 8);
	const char *units[] = { "°C", "°F" };
	int u = wx.fahrenheit;
	if (ui_segmented(ui, cut_right(&hb, 84), units, 2, &u)) {
		wx.fahrenheit = u == 1;
		conf_set_bool(wx.conf, "fahrenheit", wx.fahrenheit);
		conf_save(wx.conf);
	}
	if (atomic_load(&wx.job) == JOB_RUNNING && !wx.searching)
		ui_spinner(ui, center_in(cut_right(&hb, 30), 16, 16));

	Rect body = pad4(win, 6, 28, 24, 28);
	if (!wx.fc) {
		const char *title = !wx.have_place ? "Finding your place…" : atomic_load(&wx.job) == JOB_RUNNING ? "Getting the forecast…" : "No forecast";
		const char *sub = wx.error[0] ? wx.error : !wx.have_place ? "Or search for a city with the button above." : "";
		Rect c = center_in(body, minf(420, body.w), 200);
		paint_icon(ui, center_in(cut_top(&c, 110), 110, 110), 2, true);
		text_shadowed(ui, ui_font(ui, 20, 700), cut_top(&c, 30), title, COLOR_WHITE, ALIGN_CENTER);
		ui_text_wrapped(ui, ui_font(ui, 14, 500), c, sub, color_hex("#ffffffc8"), ALIGN_CENTER);
		if (atomic_load(&wx.job) == JOB_RUNNING)
			ui->redraw = true;
	} else {
		f32 content_h = 210 + 14 + 120 + 14 + 330;
		f32 y = ui_scroll_begin(ui, body, "page");
		Rect page = rect(body.x, y, body.w, maxf(body.h, content_h));
		draw_hero(ui, cut_top(&page, 210));
		cut_top(&page, 14);
		draw_hours(ui, cut_top(&page, 120));
		cut_top(&page, 14);
		Rect lower = cut_top(&page, 330);
		if (lower.w >= 700) {
			Rect days = cut_left(&lower, lower.w * 0.5f);
			cut_left(&lower, 14);
			draw_days(ui, days);
			draw_details(ui, lower);
		} else {
			draw_days(ui, lower);
		}
		if (wx.error[0]) {
			Font fnt = ui_font(ui, 12, 600);
			char m[200];
			fmt(m, sizeof m, "Not updated: %s", wx.error);
			f32 w = ui_text_width(ui, fnt, m) + 24;
			Rect pill = rect(body.x + body.w - w, y + 4, w, 24);
			ui_round(ui, pill, 12, color_hex("#00000040"));
			ui_text_in(ui, fnt, pill, m, COLOR_WHITE, ALIGN_CENTER);
		}
		ui_scroll_end(ui, content_h);
	}
	if (wx.searching)
		draw_search(ui, rect(0, 40, ui->width, ui->height - 40));
}

static void tick(Ui *ui)
{
	ui->redraw = true; /* the clock in the header */
}

static void init(Ui *ui)
{
	wx.ui = ui;
	fmt(wx.conf_path, sizeof wx.conf_path, "%s/.config/zurbax/weather.conf", home_dir());
	fmt(wx.cache_path, sizeof wx.cache_path, "%s/.cache/zurbax/weather.json", home_dir());
	char *d = path_dir(wx.conf_path);
	dir_make(d, 0755);
	mem_free(d);
	d = path_dir(wx.cache_path);
	dir_make(d, 0755);
	mem_free(d);
	wx.conf = conf_load(wx.conf_path);
	wx.fahrenheit = conf_get_bool(wx.conf, "fahrenheit", false);
	const char *name = conf_get(wx.conf, "name", NULL);
	if (name) {
		str_copy(wx.place.name, name, sizeof wx.place.name);
		str_copy(wx.place.region, conf_get(wx.conf, "region", ""), sizeof wx.place.region);
		wx.place.lat = conf_get_float(wx.conf, "lat", 0);
		wx.place.lon = conf_get_float(wx.conf, "lon", 0);
		wx.have_place = true;
		/* Last forecast first, so the window is never empty. */
		usize len;
		char *cached = file_read(wx.cache_path, &len);
		if (cached) {
			Forecast *f = NEW(Forecast);
			parse_forecast(cached, len, f);
			f->fetched = file_mtime(wx.cache_path);
			if (f->ndays)
				wx.fc = f;
			else
				mem_free(f);
			mem_free(cached);
		}
		refresh();
	} else {
		/* Guess from the time zone: "Europe/Bucharest" -> Bucharest. */
		const char *tz = env_get("TZ");
		const char *city = tz ? str_rchr(tz, '/') : NULL;
		if (city && city[1]) {
			char q[64];
			str_copy(q, city + 1, sizeof q);
			for (char *p = q; *p; p++)
				if (*p == '_')
					*p = ' ';
			search(q);
		} else {
			wx.searching = true;
		}
	}
	wx.next_refresh = ui->now + 15 * 60;
}

int main(void)
{
	UiAppDesc d = { .id = "weather", .title = "Weather", .width = 900, .height = 720, .min_width = 520, .min_height = 480, .flags = WIN_RESIZABLE,
			.init = init, .frame = frame, .tick = tick, .tick_ms = 30000 };
	return ui_run(&d);
}
