/*
 * Bridge's three shelves: the Windows programs people add (bridge.c), the
 * games Steam installed (steam.c) and games played in the cloud (cloud.c).
 */
#pragma once

#include "zui.h"
#include "zn.h"

extern char bridge_base[400]; /* ~/.local/share/zurbax/bridge */
extern char bridge_msg[200];  /* a line at the foot of the window */

/* A cover as drawn, loaded once. */
typedef struct {
	char id[48];
	Canvas *art, *icon;
	bool wide; /* the art is a store banner, not a tall cover */
	bool loaded;
} Cover;

Cover *cover_slot(const char *key);
void forget_cover(const char *key);
char *cover_path(const char *id, const char *suffix);
Color tile_color(const char *name);
/* tone 0: a colour from the name */
void draw_cover_img(Ui *ui, Rect r, const Canvas *art, bool wide, const Canvas *icon, const char *name, Color tone, f32 radius);
/* Steam's art for an app id into path; src says which ("steam:ID" tall, "wide:ID" a banner) */
bool steam_art(u32 app, const char *path, char *src, usize cap);

/* the window wakes within secs, for what it's waiting on */
void wake_within(Ui *ui, f64 secs);

/* layerrun ARGS... in the Linux session, not waiting for it: its pid */
int layer_start(char **args, bool new_session);
bool layer_has(const char *path);

void steam_tab(Ui *ui, Rect r);
void steam_tools(Ui *ui, Rect *tools);
void cloud_tab(Ui *ui, Rect r);
