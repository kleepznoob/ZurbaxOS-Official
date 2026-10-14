/* What a Windows program says about itself: its name and its icon. */
#pragma once

#include "zg.h"

typedef struct {
	char product[96]; /* the version information's ProductName, "" if none */
	Canvas *icon;     /* the program's own icon, the largest it has; NULL if none */
} PeInfo;

/* Reads a .exe's resources. False if it isn't a Windows program. */
bool pe_read(const char *path, PeInfo *out);
void pe_info_free(PeInfo *info);
