/*-
 * Terminal styling for the pkg frontend.
 *
 * Styling is only ever applied when the target stream is a terminal (or
 * PKG_COLOR=always).  When it is off every helper returns an empty string
 * and callers fall back to the original plain output, so pipes, logs and
 * scripts see exactly what upstream pkg prints.
 */

#ifndef _PKG_STYLE_H
#define _PKG_STYLE_H

#include <stdbool.h>
#include <stdio.h>
#include <pkg/sb.h>

enum style_role {
	ST_RESET = 0,
	ST_BOLD,
	ST_DIM,
	ST_NAME,		/* package names */
	ST_VERSION,		/* a version with no direction */
	ST_VER_UP_OLD,		/* upgrade: version we leave (red) */
	ST_VER_UP_NEW,		/* upgrade: version we get (green) */
	ST_VER_DOWN_OLD,	/* downgrade: version we leave (green) */
	ST_VER_DOWN_NEW,	/* downgrade: version we get (yellow) */
	ST_INSTALL,
	ST_UPGRADE,
	ST_DOWNGRADE,
	ST_REINSTALL,
	ST_REMOVE,
	ST_FETCH,
	ST_LOCKED,
	ST_GROUP,
	ST_REPO,
	ST_OK,
	ST_WARN,
	ST_ERROR,
	ST_ACCENT,
	ST_BAR_DONE,
	ST_BAR_TODO,
	ST_MAX
};

enum style_glyph {
	GL_INSTALL = 0,
	GL_UPGRADE,
	GL_DOWNGRADE,
	GL_REINSTALL,
	GL_REMOVE,
	GL_FETCH,
	GL_LOCKED,
	GL_GROUP,
	GL_EXTRACT,
	GL_ARROW,
	GL_PROMPT,
	GL_OK,
	GL_WARN,
	GL_ERROR,
	GL_DOT,
	GL_RULE,
	GL_BAR_FULL,
	GL_BAR_HALF,
	GL_BAR_EMPTY,
	GL_MAX
};

void		 style_init(void);
/* Styling enabled for stdout: also selects the fancy layouts. */
bool		 style_fancy(void);
bool		 style_fancy_stderr(void);
/* Escape sequence for a role on stdout / stderr ("" when disabled). */
const char	*S(enum style_role);
const char	*SE(enum style_role);
const char	*style_glyph(enum style_glyph);
/* Visible width of a string, ignoring escapes and counting UTF-8 chars. */
size_t		 style_width(const char *);
int		 style_term_cols(void);
void		 style_rule(FILE *, int width);
/* Append "old → new" with the changed part of each version emphasized. */
void		 style_version_pair(sb_t *, const char *oldv, const char *newv,
		    enum style_role oldr, enum style_role newr, int oldpad);
void		 style_pad(sb_t *, const char *s, size_t width);

#endif
