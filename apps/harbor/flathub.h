/* Flathub in Harbor: thousands of Linux apps, installed with Flatpak into
 * the user's Linux session (flathub.c). */
#pragma once

#include "zui.h"

enum { FLAT_IDLE, FLAT_INSTALL, FLAT_REMOVE, FLAT_UPDATE };

typedef struct {
	char id[128];
	char name[96];
	char summary[200];
	char developer[96];
	char category[32];
	char license[48];
	char version[32];
	char icon_url[320];
	char shot_url[400];
	bool verified;
	char *description;      /* plain text, paragraphs apart */
	Canvas *icon, *shot;
	int icon_state;         /* atomic: 0 not asked, 1 loading, 2 done */
	int details;            /* atomic: 0 not asked, 1 loading, 2 loaded, 3 failed */
	u64 download_size, installed_size, runtime_size;
	char runtime[96];
	/* on this computer */
	char installed[32];     /* version, or "" */
	bool update;
	int op;                 /* FLAT_*, while a job runs for it */
	char progress[120];
	f32 fraction;           /* 0..1, or < 0 when unknown */
	char error[200];
} FlatApp;

void flathub_init(Ui *ui, const char *cache_dir);
void flathub_refresh(void);                  /* popular apps and what's installed */
void flathub_search(const char *query);      /* "" clears the results */
void flathub_details(FlatApp *a);            /* description, screenshot, sizes */
void flathub_job(FlatApp *a, int op);        /* a NULL app with FLAT_UPDATE updates everything */
void flathub_poll(Ui *ui);                   /* each frame: finished jobs */

FlatApp **flathub_popular(int *n);
FlatApp **flathub_results(int *n);
FlatApp **flathub_gaming(int *n);           /* Zurbax's picks for playing */
FlatApp **flathub_installed(int *n);
bool flathub_loading(void);
bool flathub_searching(void);
bool flathub_busy(void);
bool flathub_available(void);                /* the Linux layer has Flatpak */
const char *flathub_error(void);
int flathub_updates(void);

void flathub_icon(Ui *ui, Rect r, FlatApp *a);

/* The lists and the apps' fields change on Harbor's threads: hold the lock
 * while reading them. */
void flathub_lock(void);
void flathub_unlock(void);
FlatApp *flathub_all(void);                  /* the progress of updating everything */
