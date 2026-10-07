/*
 * Driver interface: how a host feeds a Ui context with input and frames.
 * Used by the app runtime (client.c), offscreen snapshots, and the display
 * server's own shell surfaces.
 */
#pragma once

#include "zui.h"

Ui *ui_new(void);
void ui_free(Ui *ui);
/* Points the context at a canvas of size (w*scale, h*scale). */
void ui_attach(Ui *ui, Canvas *canvas, f32 logical_w, f32 logical_h, f32 scale);
void ui_set_theme(Ui *ui, const ThemeInfo *info);

void ui_input_pointer(Ui *ui, u32 kind, f32 x, f32 y, u32 button, u32 mods);
void ui_input_scroll(Ui *ui, f32 dx, f32 dy, u32 source, u32 phase);
void ui_input_key(Ui *ui, u32 code, u32 state, u32 mods, const char *text);
void ui_input_gamepad(Ui *ui, u32 pad, bool connected, u32 buttons, const f32 *axes, const char *name);
void ui_input_focus(Ui *ui, bool focused);
void ui_input_paste(Ui *ui, const char *text);
void ui_input_drop(Ui *ui, const char *paths);

/* Runs one frame: clears per-frame input after calling draw(ui). Returns true
 * if the result is likely to change again soon (animations in flight). */
bool ui_frame(Ui *ui, void (*draw)(Ui *ui));
/* True when queued input or animations call for a new frame. */
bool ui_needs_frame(const Ui *ui);
