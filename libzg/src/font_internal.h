/* Shared between the glyph tables and the text renderer. */
#pragma once

#include "zg.h"

typedef struct {
	u32 cp;
	const char *def;
} GlyphDef;

typedef struct {
	u32 cp, base, mark;
} GlyphComposite;

typedef struct {
	u32 left, right;
	i32 adjust;
} GlyphKern;

extern const GlyphDef ZURBAX_SANS[];
extern const GlyphDef ZURBAX_MONO[];
extern const GlyphComposite ZURBAX_COMPOSITES[];
extern const GlyphKern ZURBAX_KERNING[];
