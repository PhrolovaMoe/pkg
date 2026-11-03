/*-
 * Terminal styling for the pkg frontend.  See style.h.
 */

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#include <sys/ioctl.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <pkg.h>

#include "style.h"

static bool color_out = false;
static bool color_err = false;
static bool unicode = false;

static const char *codes[ST_MAX] = {
	[ST_RESET]	 = "\033[0m",
	[ST_BOLD]	 = "\033[1m",
	[ST_DIM]	 = "\033[2m",
	[ST_NAME]	 = "\033[1m",
	[ST_VERSION]	 = "\033[36m",
	[ST_VER_UP_OLD]	 = "\033[31m",
	[ST_VER_UP_NEW]	 = "\033[32m",
	[ST_VER_DOWN_OLD] = "\033[32m",
	[ST_VER_DOWN_NEW] = "\033[33m",
	[ST_INSTALL]	 = "\033[1;32m",
	[ST_UPGRADE]	 = "\033[1;36m",
	[ST_DOWNGRADE]	 = "\033[1;33m",
	[ST_REINSTALL]	 = "\033[1;35m",
	[ST_REMOVE]	 = "\033[1;31m",
	[ST_FETCH]	 = "\033[1;34m",
	[ST_LOCKED]	 = "\033[1;90m",
	[ST_GROUP]	 = "\033[1;35m",
	[ST_REPO]	 = "\033[35m",
	[ST_OK]		 = "\033[1;32m",
	[ST_WARN]	 = "\033[1;33m",
	[ST_ERROR]	 = "\033[1;31m",
	[ST_ACCENT]	 = "\033[1;35m",
	[ST_BAR_DONE]	 = "\033[35m",
	[ST_BAR_TODO]	 = "\033[90m",
	[ST_LABEL]	 = "\033[1;34m",
	[ST_COMMENT]	 = "\033[2m",
	[ST_LINK]	 = "\033[4;34m",
};

static const char *glyphs_utf8[GL_MAX] = {
	[GL_INSTALL]	= "✚",
	[GL_UPGRADE]	= "▲",
	[GL_DOWNGRADE]	= "▼",
	[GL_REINSTALL]	= "↻",
	[GL_REMOVE]	= "✖",
	[GL_FETCH]	= "↓",
	[GL_LOCKED]	= "⊘",
	[GL_GROUP]	= "◆",
	[GL_EXTRACT]	= "↳",
	[GL_ARROW]	= "→",
	[GL_PROMPT]	= "❯",
	[GL_OK]		= "✔",
	[GL_WARN]	= "⚠",
	[GL_ERROR]	= "✖",
	[GL_DOT]	= "·",
	[GL_RULE]	= "─",
	[GL_BAR_FULL]	= "━",
	[GL_BAR_HALF]	= "╸",
	[GL_BAR_EMPTY]	= "━",
	[GL_SYNC]	= "⟳",
	[GL_GUTTER]	= "┃",
	[GL_MAIL]	= "✉",
	[GL_BOX_TL]	= "╭",
	[GL_BOX_BL]	= "╰",
	[GL_BOX_H]	= "─",
	[GL_BOX_V]	= "│",
	[GL_BOX_SEP]	= "├",
	[GL_BOX_DASH]	= "╌",
	[GL_TREE_MID]	= "├",
	[GL_TREE_LAST]	= "╰",
	[GL_TREE_PIPE]	= "│",
};

static const char *glyphs_ascii[GL_MAX] = {
	[GL_INSTALL]	= "+",
	[GL_UPGRADE]	= "^",
	[GL_DOWNGRADE]	= "v",
	[GL_REINSTALL]	= "~",
	[GL_REMOVE]	= "x",
	[GL_FETCH]	= "<",
	[GL_LOCKED]	= "#",
	[GL_GROUP]	= "*",
	[GL_EXTRACT]	= ">",
	[GL_ARROW]	= "->",
	[GL_PROMPT]	= ">",
	[GL_OK]		= "ok",
	[GL_WARN]	= "!",
	[GL_ERROR]	= "x",
	[GL_DOT]	= "-",
	[GL_RULE]	= "-",
	[GL_BAR_FULL]	= "=",
	[GL_BAR_HALF]	= ">",
	[GL_BAR_EMPTY]	= "-",
	[GL_SYNC]	= "*",
	[GL_GUTTER]	= "|",
	[GL_MAIL]	= "*",
	[GL_BOX_TL]	= "+",
	[GL_BOX_BL]	= "+",
	[GL_BOX_H]	= "-",
	[GL_BOX_V]	= "|",
	[GL_BOX_SEP]	= "|",
	[GL_BOX_DASH]	= "-",
	[GL_TREE_MID]	= "|",
	[GL_TREE_LAST]	= "`",
	[GL_TREE_PIPE]	= "|",
};

static bool
locale_is_utf8(void)
{
	static const char *vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
	const char *v;

	for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
		v = getenv(vars[i]);
		if (v == NULL || *v == '\0')
			continue;
		return (strcasestr(v, "UTF-8") != NULL ||
		    strcasestr(v, "UTF8") != NULL);
	}
	return (false);
}

void
style_init(void)
{
	const char *mode, *term, *nocolor;
	bool always = false, never = false, auto_ok;

	mode = pkg_object_string(pkg_config_get("PKG_COLOR"));
	if (mode != NULL) {
		if (strcasecmp(mode, "always") == 0 ||
		    strcasecmp(mode, "yes") == 0)
			always = true;
		else if (strcasecmp(mode, "never") == 0 ||
		    strcasecmp(mode, "no") == 0)
			never = true;
	}

	term = getenv("TERM");
	nocolor = getenv("NO_COLOR");
	auto_ok = !never &&
	    (nocolor == NULL || *nocolor == '\0') &&
	    term != NULL && strcmp(term, "dumb") != 0;

	color_out = always || (auto_ok && isatty(STDOUT_FILENO));
	color_err = always || (auto_ok && isatty(STDERR_FILENO));

	/*
	 * The locale is a guess: doas, cron and jails often start pkg with
	 * none at all.  PKG_UNICODE lets the configuration settle it.
	 */
	mode = pkg_object_string(pkg_config_get("PKG_UNICODE"));
	if (mode != NULL && (strcasecmp(mode, "always") == 0 ||
	    strcasecmp(mode, "yes") == 0))
		unicode = true;
	else if (mode != NULL && (strcasecmp(mode, "never") == 0 ||
	    strcasecmp(mode, "no") == 0))
		unicode = false;
	else
		unicode = locale_is_utf8();
}

bool
style_fancy(void)
{
	return (color_out);
}

bool
style_fancy_stderr(void)
{
	return (color_err);
}

const char *
S(enum style_role r)
{
	return (color_out ? codes[r] : "");
}

const char *
SE(enum style_role r)
{
	return (color_err ? codes[r] : "");
}

const char *
style_glyph(enum style_glyph g)
{
	return (unicode ? glyphs_utf8[g] : glyphs_ascii[g]);
}

size_t
style_width(const char *s)
{
	size_t w = 0;

	while (*s != '\0') {
		if (*s == '\033' && s[1] == '[') {
			s += 2;
			while (*s != '\0' && !(*s >= 0x40 && *s <= 0x7e))
				s++;
			if (*s != '\0')
				s++;
			continue;
		}
		if (((unsigned char)*s & 0xC0) != 0x80)
			w++;
		s++;
	}
	return (w);
}

int
style_term_cols(void)
{
	struct winsize ws;
	const char *c;
	int n;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (ws.ws_col);
	c = getenv("COLUMNS");
	if (c != NULL && (n = atoi(c)) > 0)
		return (n);
	return (80);
}

void
style_rule(FILE *f, int width)
{
	fprintf(f, "%s", S(ST_DIM));
	for (int i = 0; i < width; i++)
		fputs(style_glyph(GL_RULE), f);
	fprintf(f, "%s\n", S(ST_RESET));
}

void
style_pad(sb_t *sb, const char *s, size_t width)
{
	size_t w = style_width(s);

	sb_cat(sb, s);
	while (w++ < width)
		sb_cat_c(sb, ' ');
}

static bool
is_version_sep(char c)
{
	return (!isalnum((unsigned char)c));
}

/*
 * Length of the leading part shared by both versions, cut back to the last
 * separator so that "8.10.1 -> 8.11.0" emphasizes "10.1"/"11.0" rather
 * than "0.1"/"1.0".
 */
static size_t
common_prefix(const char *a, const char *b)
{
	size_t i = 0, cut = 0;

	while (a[i] != '\0' && a[i] == b[i]) {
		if (is_version_sep(a[i]))
			cut = i + 1;
		i++;
	}
	if (a[i] == '\0' && b[i] == '\0')
		return (i);
	return (cut);
}

static void
emphasized(sb_t *sb, const char *v, size_t pfx, enum style_role r)
{
	sb_printf(sb, "%s%.*s%s%s%s", S(r), (int)pfx, v, S(ST_BOLD), v + pfx,
	    S(ST_RESET));
}

void
style_version_pair(sb_t *sb, const char *oldv, const char *newv,
    enum style_role oldr, enum style_role newr, int oldpad)
{
	size_t pfx = common_prefix(oldv, newv);
	int w = (int)strlen(oldv);

	emphasized(sb, oldv, pfx, oldr);
	while (w++ < oldpad)
		sb_cat_c(sb, ' ');
	sb_printf(sb, " %s%s%s ", S(ST_DIM), style_glyph(GL_ARROW),
	    S(ST_RESET));
	emphasized(sb, newv, pfx, newr);
}

/*
 * Usage synopses: "Usage:" and the "pkg <command>" it introduces are
 * emphasized, flags are cyan, <placeholders> yellow and the [ ] | grammar
 * dimmed.  The "For more information" hint is dimmed as a whole.
 */
void
style_usage_line(FILE *f, const char *line, bool on)
{
	const char *p = line, *q;
	bool word_start = true, cmd_done = false;

	if (!on) {
		fputs(line, f);
		return;
	}
	if (strncmp(line, "For more information", 20) == 0) {
		q = strchr(line, '\'');
		if (q != NULL && strchr(q + 1, '\'') != NULL) {
			const char *e = strchr(q + 1, '\'');
			fprintf(f, "%s%.*s%s%s%.*s%s%s%s", codes[ST_DIM],
			    (int)(q - line + 1), line, codes[ST_RESET],
			    codes[ST_BOLD], (int)(e - q - 1), q + 1,
			    codes[ST_RESET], codes[ST_DIM], e);
			fputs(codes[ST_RESET], f);
		} else
			fprintf(f, "%s%s%s", codes[ST_DIM], line,
			    codes[ST_RESET]);
		return;
	}
	while (*p != '\0') {
		if (strncmp(p, "Usage:", 6) == 0) {
			fprintf(f, "%sUsage:%s", codes[ST_ACCENT], codes[ST_RESET]);
			p += 6;
			continue;
		}
		if (word_start && !cmd_done && strncmp(p, "pkg ", 4) == 0) {
			/* "pkg" and the command word after it. */
			q = p + 4;
			while (*q != '\0' && (islower((unsigned char)*q) ||
			    *q == '-'))
				q++;
			if (q > p + 4 && q[-1] != '-')
				fprintf(f, "%s%.*s%s", codes[ST_BOLD],
				    (int)(q - p), p, codes[ST_RESET]);
			else {
				q = p + 3;
				fprintf(f, "%spkg%s", codes[ST_BOLD],
				    codes[ST_RESET]);
			}
			cmd_done = true;
			p = q;
			word_start = false;
			continue;
		}
		if (*p == '<' && (q = strchr(p, '>')) != NULL) {
			fprintf(f, "%s%.*s%s", codes[ST_WARN], (int)(q - p + 1),
			    p, codes[ST_RESET]);
			p = q + 1;
			word_start = false;
			continue;
		}
		if (word_start && *p == '-' && p[1] != '\0' && p[1] != ' ') {
			q = p;
			while (*q != '\0' && !isspace((unsigned char)*q) &&
			    *q != ']' && *q != '|' && *q != '[')
				q++;
			fprintf(f, "%s%.*s%s", codes[ST_UPGRADE], (int)(q - p),
			    p, codes[ST_RESET]);
			p = q;
			word_start = false;
			continue;
		}
		if (*p == '[' || *p == ']' || *p == '|' || *p == '{' ||
		    *p == '}') {
			fprintf(f, "%s%c%s", codes[ST_DIM], *p, codes[ST_RESET]);
			word_start = (*p != ']' && *p != '}');
			p++;
			continue;
		}
		fputc(*p, f);
		word_start = isspace((unsigned char)*p);
		p++;
	}
}

int
usage_printf(const char *fmt, ...)
{
	va_list ap;
	char *text = NULL, *line, *nl;
	int len;

	va_start(ap, fmt);
	if (!color_err) {
		len = vfprintf(stderr, fmt, ap);
		va_end(ap);
		return (len);
	}
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return (len);
	for (line = text; *line != '\0'; line = nl + 1) {
		nl = strchr(line, '\n');
		if (nl != NULL)
			*nl = '\0';
		style_usage_line(stderr, line, true);
		if (nl == NULL)
			break;
		fputc('\n', stderr);
	}
	free(text);
	return (len);
}
