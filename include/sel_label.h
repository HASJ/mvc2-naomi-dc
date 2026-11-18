/* The select screen's assist pillar labels: the move each assist type does, by name, with a symbol of its type. */
#ifndef SEL_LABEL_H
#define SEL_LABEL_H

#define SEL_FONT_FIRST	32		/* the font's first character */
#define SEL_FONT_CHARS	95		/* and how many */
#define SEL_FONT_H		46		/* each glyph's rows */
#define SEL_FONT_ASCENT	37		/* the baseline, from the top */
#define SEL_ICON_TYPES	13		/* the assist types: sel_glow_step_tbl's labels */
#define SEL_ICON_SIZE	64		/* each symbol's texels across and down */

typedef struct SEL_GLYPH {
	Sint8	left;		/* its first column, from the pen */
	Uint8	width;		/* its columns */
	Uint8	advance;	/* the pen's move after it, in quarter texels */
	Uint32	ofs;		/* where its bits start in sel_font_bits */
} SEL_GLYPH;

extern const SEL_GLYPH sel_font_glyph[SEL_FONT_CHARS];
extern const Uint8 sel_font_bits[];
extern const Uint8 sel_icon_bits[SEL_ICON_TYPES][SEL_ICON_SIZE * SEL_ICON_SIZE];

#endif