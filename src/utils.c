/*-
 * Copyright (c) 2011-2024 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2012-2015 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2013-2016 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif

#include <err.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#if __has_include(<libutil.h>)
#include <libutil.h>
#endif
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pwd.h>
#include <pkg.h>
#include <hash.h>
#include <xmalloc.h>
#include <pkg/audit.h>

#include <bsd_compat.h>

#include "pkgcli.h"

struct jobs_sum_number {
	int install;
	int reinstall;
	int downgrade;
	int upgrade;
	int delete;
	int fetch;
	int group_install;
	int group_upgrade;
};

void
append_yesno(bool r, char *yesnomsg, size_t len)
{
	static const char	trunc[] = "\n[truncated] ";
	/* These two strings must be the same length. */
	static const char	yes[] = "[Y/n]: ";
	static const char	no[] = "[y/N]: ";

	size_t	msglen = strlen(yesnomsg);

	if (msglen > len - sizeof yes) {
		yesnomsg[len - sizeof trunc - sizeof yes] = '\0';
		strlcat(yesnomsg, trunc, len);
	}
	if (style_fancy()) {
		char suffix[64];

		snprintf(suffix, sizeof(suffix), "%s%s%s ", S(ST_DIM),
		    r ? "[Y/n]" : "[y/N]", S(ST_RESET));
		strlcat(yesnomsg, suffix, len);
		return;
	}
	strlcat(yesnomsg, r ? yes : no, len);
}

bool
query_tty_yesno(bool r, const char *msg, ...)
{
	int	 c;
	va_list	 ap;
	int	 tty_fd;
	FILE	*tty;
	int	 tty_flags = O_RDWR;
	char	yesnomsg[65536];

#ifdef O_TTY_INIT
	tty_flags |= O_TTY_INIT;
#endif
	tty_fd = open(_PATH_TTY, tty_flags);
	if (tty_fd == -1) {
		/* No ctty -- return the default answer */
		if (default_yes)
			return (true);
		return (r);
	}

	tty = fdopen(tty_fd, "r+");

	strlcpy(yesnomsg, msg, sizeof(yesnomsg));
	append_yesno(default_yes || r, yesnomsg, sizeof yesnomsg);

	va_start(ap, msg);
	pkg_vfprintf(tty, yesnomsg, ap);
	va_end(ap);

	fflush(tty);
	c = getc(tty);
	if (c == 'y' || c == 'Y')
		r = true;
	else if (c == 'n' || c == 'N')
		r = false;
	else if (c == '\n' || c == EOF) {
                if (default_yes)
			r = true;
		/* Else, r is not modified. It's default value is kept. */
		goto cleanup;
	}

	while ((c = getc(tty)) != '\n' && c != EOF)
		continue;

cleanup:
	fclose(tty);

	return (r);
}

static bool
vquery_yesno(bool deft, const char *msg, va_list ap)
{
	char *line = NULL;
	char *out;
	size_t linecap = 0;
	int linelen;
	bool	 r = deft;
	char	yesnomsg[65536];

	/* We use default value of yes or default in case of quiet mode */
	if (quiet)
		return (yes || default_yes || r);

	if (dry_run)
		return (yes || default_yes || r );

	/* Do not query user if we have specified yes flag */
	if (yes)
		return (true);

	strlcpy(yesnomsg, msg, sizeof(yesnomsg));
	append_yesno(default_yes || r, yesnomsg, sizeof yesnomsg);

	out = NULL;
	if (pkg_vasprintf(&out, yesnomsg, ap) < 0)
		out = NULL;
	printf("%s", out != NULL ? out : yesnomsg);

	for (;;) {
		if ((linelen = getline(&line, &linecap, stdin)) != -1) {

			if (linelen == 1 && line[0] == '\n') {
				if (default_yes)
					r = true;
				break;
			}
			else if (linelen == 2) {
				if (line[0] == 'y' || line[0] == 'Y') {
					r = true;
					break;
				}
				else if (line[0] == 'n' || line[0] == 'N') {
					r = false;
					break;
				}
			}
			else {
				if (STRIEQ(line, "yes\n")) {
					r = true;
					break;
				}
				else if (STRIEQ(line, "no\n")) {
					r = false;
					break;
				}
			}
			printf("Please type 'Y[es]' or 'N[o]' to make a selection\n");
			printf("%s", out);
		}
		else {
			if (errno == EINTR)
				continue;
			r = default_yes;
			break;
		}
	}

	free(line);
	free(out);

	return (r);
}

bool
query_yesno(bool deft, const char *msg, ...)
{
	va_list	 ap;
	bool r;

	va_start(ap, msg);
	r = vquery_yesno(deft, msg, ap);
	va_end(ap);

	return (r);
}

int
query_select(const char *msg, const char **opts, int ncnt, int deft)
{
	int i;
	char *str = NULL;
	char *endpntr = NULL;
	size_t n = 0;

	printf("%s\n", msg);
	for (i = 0; i < ncnt; i++) {
		if (i + 1 == deft)
		{
			printf("*[%d] %s\n",
				i + 1, opts[i]);
		} else {
			printf(" [%d] %s\n",
				i + 1, opts[i]);
		}
	}

	i = deft;
	while (getline(&str, &n, stdin) == -1) {
		if (errno == EINTR)
			continue;
		else
			goto cleanup;
	}
	i = (int) strtoul(str, &endpntr, 10);

	if (*endpntr == '\0' || *endpntr == '\n' || *endpntr == '\r') {
		if (i > ncnt || i < 1)
			i = deft;
	} else
		i = -1;

cleanup:
	free(str);
	return (i);
}

/* what the pkg needs to load in order to display the requested info */
int
info_flags(uint64_t opt, bool remote)
{
	int flags = PKG_LOAD_BASIC;

	if (opt & INFO_CATEGORIES)
		flags |= PKG_LOAD_CATEGORIES;
	if (opt & INFO_LICENSES)
		flags |= PKG_LOAD_LICENSES;
	if (opt & (INFO_OPTIONS|INFO_OPTION_DEFAULTS|INFO_OPTION_DESCRIPTIONS))
		flags |= PKG_LOAD_OPTIONS;
	if (opt & INFO_SHLIBS_REQUIRED)
		flags |= PKG_LOAD_SHLIBS_REQUIRED;
	if (opt & INFO_SHLIBS_PROVIDED)
		flags |= PKG_LOAD_SHLIBS_PROVIDED;
	if (opt & INFO_PROVIDED)
		flags |= PKG_LOAD_PROVIDES;
	if (opt & INFO_REQUIRED)
		flags |= PKG_LOAD_REQUIRES;
	if (opt & INFO_ANNOTATIONS)
		flags |= PKG_LOAD_ANNOTATIONS;
	if (opt & INFO_DEPS)
		flags |= PKG_LOAD_DEPS|PKG_LOAD_SHLIBS_REQUIRED|PKG_LOAD_REQUIRES;
	if (opt & INFO_RDEPS)
		flags |= PKG_LOAD_RDEPS|PKG_LOAD_SHLIBS_PROVIDED|PKG_LOAD_PROVIDES;
	if (opt & INFO_FILES)
		flags |= PKG_LOAD_FILES;
	if (opt & INFO_DIRS)
		flags |= PKG_LOAD_DIRS;
	if (opt & INFO_USERS)
		flags |= PKG_LOAD_USERS;
	if (opt & INFO_GROUPS)
		flags |= PKG_LOAD_GROUPS;
	if (opt & INFO_RAW) {
		flags |= PKG_LOAD_CATEGORIES      |
			 PKG_LOAD_LICENSES        |
			 PKG_LOAD_OPTIONS         |
			 PKG_LOAD_SHLIBS_REQUIRED |
			 PKG_LOAD_SHLIBS_PROVIDED |
			 PKG_LOAD_PROVIDES        |
			 PKG_LOAD_REQUIRES        |
			 PKG_LOAD_ANNOTATIONS     |
			 PKG_LOAD_DEPS;
		if (!remote) {
			flags |= PKG_LOAD_FILES  |
				PKG_LOAD_DIRS    |
				PKG_LOAD_USERS   |
				PKG_LOAD_GROUPS  |
				PKG_LOAD_SCRIPTS |
				PKG_LOAD_LUA_SCRIPTS;
		}
	}

	return flags;
}

typedef struct pkgdb_it *(*pkgdb_query_fn)(struct pkgdb *, const char *);

/*
 * A "Label          : " field tag for pkg info.  The fancy form keeps the
 * same text and alignment, only colored.
 */
static void
info_label(const char *label, bool newline)
{
	if (style_fancy())
		printf("%s%-15s%s%s:%s%s", S(ST_LABEL), label, S(ST_RESET),
		    S(ST_DIM), S(ST_RESET), newline ? "\n" : " ");
	else
		printf(newline ? "%-15s:\n" : "%-15s: ", label);
}

/*
 * "name-version" (or "@group") with a bold name and colored version.
 * Returns the visible width, like the pkg_printf() call it replaces.
 */
static int
print_fancy_namever(struct pkg *pkg, bool is_group, bool with_version)
{
	const char *n = NULL, *v = NULL;

	pkg_get(pkg, PKG_ATTR_NAME, &n);
	pkg_get(pkg, PKG_ATTR_VERSION, &v);
	if (n == NULL)
		n = "";
	if (v == NULL)
		v = "";
	if (is_group) {
		printf("%s@%s%s%s%s", S(ST_DIM), S(ST_RESET), S(ST_GROUP), n,
		    S(ST_RESET));
		return (1 + strlen(n));
	}
	if (!with_version) {
		printf("%s%s%s", S(ST_NAME), n, S(ST_RESET));
		return (strlen(n));
	}
	printf("%s%s%s%s-%s%s%s%s", S(ST_NAME), n, S(ST_RESET), S(ST_DIM),
	    S(ST_RESET), S(ST_VERSION), v, S(ST_RESET));
	return (strlen(n) + 1 + strlen(v));
}

static void
print_depinfo(struct pkgdb *db, struct pkg *pkg, bool print_tag,
    int shlib_attr, const char *shlib_label, pkgdb_query_fn shlib_query,
    int provide_attr, const char *provide_label, pkgdb_query_fn provide_query)
{
	struct pkg_stringlist *sl = NULL;
	struct pkg_stringlist_iterator *slit;
	struct pkgbase *pb;
	bool shlibstag = false;
	bool reqtag = false;
	const char *buf;
	struct pkgdb_it *it;
	struct pkg *p = NULL;

	pkg_get(pkg, shlib_attr, &sl);
	pb = pkgbase_new(db);
	slit = pkg_stringlist_iterator(sl);
	while ((buf = pkg_stringlist_next(slit))) {
		if (pkgbase_provide_shlib(pb, buf))
			continue;
		if (!shlibstag) {
			if (print_tag)
				info_label(shlib_label, true);
			shlibstag = true;
		}
		it = shlib_query(db, buf);
		while (pkgdb_it_next(it, &p, PKG_LOAD_BASIC) == EPKG_OK) {
			pkg_printf("\t%n-%v (%S)\n", p, p, buf);
		}
		pkgdb_it_free(it);
	}
	free(slit);
	free(sl);

	pkg_get(pkg, provide_attr, &sl);
	slit = pkg_stringlist_iterator(sl);
	while ((buf = pkg_stringlist_next(slit))) {
		if (!reqtag) {
			if (print_tag)
				info_label(provide_label, true);
			reqtag = true;
		}
		it = provide_query(db, buf);
		while (pkgdb_it_next(it, &p, PKG_LOAD_BASIC) == EPKG_OK) {
			pkg_printf("\t%n-%v (%S)\n", p, p, buf);
		}
		pkgdb_it_free(it);
	}
	free(slit);
	free(sl);
	pkgbase_free(pb);
}

/*
 * Dependency trees for pkg info -d / -r on a terminal.  Installed
 * packages are expanded recursively through the local database; a package
 * already shown higher up is printed once more with "(*)" and not expanded
 * again, which also keeps cycles finite.
 */
#define DEP_TREE_MAXDEPTH	64

int info_tree_depth = 1;

struct dep_tree {
	struct pkgdb	*db;
	hash_t		*seen;
	bool		 reverse;
	bool		 show_locks;
	int		 max_depth;	/* 0: unlimited */
	int		 unique;
	int		 repeats;
	int		 missing;
	int		 hidden;	/* nodes cut off by max_depth */
};

struct dep_node {
	const char	*name;
	const char	*version;
	bool		 locked;
};

static int
dep_node_cmp(const void *a, const void *b)
{
	return (strcmp(((const struct dep_node *)a)->name,
	    ((const struct dep_node *)b)->name));
}

static void
dep_tree_children(struct dep_tree *t, struct pkg *pkg, sb_t *prefix,
    int depth)
{
	struct pkg_dep *dep = NULL;
	struct dep_node *nodes;
	struct pkgdb_it *it;
	struct pkg *child;
	size_t n = 0, cap, plen;
	const char *v;
	bool last, found, locked;

	cap = pkg_list_count(pkg, t->reverse ? PKG_RDEPS : PKG_DEPS);
	if (cap == 0)
		return;
	nodes = xcalloc(cap, sizeof(*nodes));
	while (n < cap && (t->reverse ? pkg_rdeps(pkg, &dep) :
	    pkg_deps(pkg, &dep)) == EPKG_OK) {
		nodes[n].name = pkg_dep_name(dep);
		nodes[n].version = pkg_dep_version(dep);
		nodes[n].locked = pkg_dep_is_locked(dep);
		n++;
	}
	qsort(nodes, n, sizeof(*nodes), dep_node_cmp);

	for (size_t i = 0; i < n; i++) {
		last = (i == n - 1);
		child = NULL;
		it = NULL;
		found = false;
		v = nodes[i].version;
		locked = nodes[i].locked;
		if (t->db != NULL) {
			it = pkgdb_query(t->db, nodes[i].name, MATCH_EXACT);
			if (it != NULL && pkgdb_it_next(it, &child, PKG_LOAD_BASIC |
			    (t->reverse ? PKG_LOAD_RDEPS : PKG_LOAD_DEPS)) ==
			    EPKG_OK) {
				found = true;
				pkg_get(child, PKG_ATTR_VERSION, &v);
				pkg_get(child, PKG_ATTR_LOCKED, &locked);
			}
		}

		printf("%s%s%s%s%s", S(ST_DIM), sb_str(prefix),
		    style_glyph(last ? GL_TREE_LAST : GL_TREE_MID),
		    style_glyph(GL_BOX_H), S(ST_RESET));
		printf(" %s%s%s", S(ST_NAME), nodes[i].name, S(ST_RESET));
		if (v != NULL && *v != '\0')
			printf("%s-%s%s%s%s", S(ST_DIM), S(ST_RESET),
			    found || t->db == NULL ? S(ST_VERSION) : S(ST_DIM),
			    v, S(ST_RESET));
		if (t->show_locks && locked)
			printf(" %s%s locked%s", S(ST_LOCKED),
			    style_glyph(GL_LOCKED), S(ST_RESET));

		if (t->db != NULL && !found) {
			printf(" %s%s not installed%s\n", S(ST_ERROR),
			    style_glyph(GL_ERROR), S(ST_RESET));
			t->missing++;
		} else if (t->db != NULL &&
		    hash_get(t->seen, nodes[i].name) != NULL) {
			printf(" %s(*)%s\n", S(ST_DIM), S(ST_RESET));
			t->repeats++;
		} else if (t->db != NULL && t->max_depth != 0 &&
		    depth + 1 >= t->max_depth &&
		    pkg_list_count(child, t->reverse ? PKG_RDEPS : PKG_DEPS) > 0) {
			/* Depth limit: show how much is folded away. */
			printf(" %s(+%d)%s\n", S(ST_DIM), pkg_list_count(child,
			    t->reverse ? PKG_RDEPS : PKG_DEPS), S(ST_RESET));
			t->unique++;
			t->hidden++;
		} else {
			putchar('\n');
			t->unique++;
			if (t->db != NULL && depth < DEP_TREE_MAXDEPTH) {
				hash_safe_add(t->seen, nodes[i].name, NULL,
				    NULL);
				plen = prefix->len;
				sb_cat(prefix, last ? "   " :
				    style_glyph(GL_TREE_PIPE));
				if (!last)
					sb_cat(prefix, "  ");
				dep_tree_children(t, child, prefix, depth + 1);
				prefix->len = plen;
				prefix->d[plen] = '\0';
			}
		}
		pkg_free(child);
		pkgdb_it_free(it);
	}
	free(nodes);
}

static void
print_dep_tree(struct pkgdb *db, struct pkg *pkg, bool reverse,
    bool show_locks)
{
	struct dep_tree t = { 0 };
	sb_t prefix = sb_init();
	const char *name = NULL;

	/* Only installed packages can be followed through the local db. */
	t.db = pkg_type(pkg) == PKG_INSTALLED ? db : NULL;
	t.reverse = reverse;
	t.show_locks = show_locks;
	t.max_depth = info_tree_depth;
	t.seen = hash_new();
	pkg_get(pkg, PKG_ATTR_NAME, &name);
	if (name != NULL)
		hash_safe_add(t.seen, name, NULL, NULL);

	sb_cat(&prefix, "  ");
	dep_tree_children(&t, pkg, &prefix, 0);
	sb_fini(&prefix);

	if (t.db != NULL) {
		printf("  %s%d package%s", S(ST_DIM), t.unique,
		    t.unique == 1 ? "" : "s");
		if (t.repeats > 0)
			printf(" %s (*) %d shown above", style_glyph(GL_DOT),
			    t.repeats);
		printf("%s", S(ST_RESET));
		if (t.missing > 0)
			printf(" %s%s%s %s%d not installed%s",
			    S(ST_DIM), style_glyph(GL_DOT), S(ST_RESET),
			    S(ST_ERROR), t.missing, S(ST_RESET));
		if (t.hidden > 0)
			printf(" %s%s (+n) more below, -L 0 for the full "
			    "tree%s", S(ST_DIM), style_glyph(GL_DOT),
			    S(ST_RESET));
		putchar('\n');
	}
	hash_destroy(t.seen);
}

/*
 * pkg info -l on a terminal: files as a directory tree.  Directories that
 * only lead to one other directory are folded into one line
 * ("include/curl/").  Installed files are looked at under the package
 * root so they can be colored by type, symlinks show their target, and
 * files that have gone missing are flagged.
 */
struct file_tree {
	int	rootfd;
	int	files;
	int	missing;
};

static int
pathcmp(const void *a, const void *b)
{
	return (strcmp(*(char * const *)a, *(char * const *)b));
}

/* End of the path component starting at off: the next '/' or the NUL. */
static size_t
comp_end(const char *p, size_t off)
{
	const char *slash = strchr(p + off, '/');

	return (slash != NULL ? (size_t)(slash - p) : strlen(p));
}

static void
file_tree_leaf(struct file_tree *t, const char *path, size_t off)
{
	struct stat st;
	const char *name = path + off;
	char target[MAXPATHLEN];
	ssize_t len;

	t->files++;
	if (t->rootfd == -1 ||
	    fstatat(t->rootfd, path + 1, &st, AT_SYMLINK_NOFOLLOW) == -1) {
		if (t->rootfd != -1 && errno == ENOENT) {
			printf(" %s%s%s %s%s missing%s\n", S(ST_ERROR), name,
			    S(ST_RESET), S(ST_ERROR), style_glyph(GL_ERROR),
			    S(ST_RESET));
			t->missing++;
		} else
			printf(" %s\n", name);
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		len = readlinkat(t->rootfd, path + 1, target,
		    sizeof(target) - 1);
		target[len > 0 ? len : 0] = '\0';
		printf(" %s%s%s %s%s %s%s\n", S(ST_VERSION), name, S(ST_RESET),
		    S(ST_DIM), style_glyph(GL_ARROW), target, S(ST_RESET));
	} else if (S_ISREG(st.st_mode) && (st.st_mode & 0111) != 0)
		printf(" %s%s%s\n", S(ST_OK), name, S(ST_RESET));
	else if (strstr(name, ".so") != NULL)
		printf(" %s%s%s\n", S(ST_REPO), name, S(ST_RESET));
	else if (strlen(name) > 7 &&
	    strcmp(name + strlen(name) - 7, ".sample") == 0)
		printf(" %s%s%s\n", S(ST_WARN), name, S(ST_RESET));
	else
		printf(" %s\n", name);
}

/*
 * Extend the directory ending at de (a '/') over following components
 * shared by every path in [lo, hi), so lone directory chains fold.
 */
static size_t
fold_dirs(char **paths, size_t lo, size_t hi, size_t de)
{
	size_t ne, k;

	for (;;) {
		ne = comp_end(paths[lo], de + 1);
		if (paths[lo][ne] != '/')
			return (de);
		for (k = lo + 1; k < hi; k++) {
			if (strncmp(paths[k] + de + 1, paths[lo] + de + 1,
			    ne - de - 1) != 0 || paths[k][ne] != '/')
				return (de);
		}
		de = ne;
	}
}

static void
file_tree_level(struct file_tree *t, char **paths, size_t lo, size_t hi,
    size_t off, sb_t *prefix)
{
	size_t i = lo, j, e, de, plen;
	bool last;

	while (i < hi) {
		e = comp_end(paths[i], off);
		for (j = i + 1; j < hi; j++) {
			if (strncmp(paths[j] + off, paths[i] + off, e - off) != 0 ||
			    (paths[j][e] != '/' && paths[j][e] != '\0'))
				break;
		}
		last = (j == hi);
		printf("%s%s%s%s%s", S(ST_DIM), sb_str(prefix),
		    style_glyph(last ? GL_TREE_LAST : GL_TREE_MID),
		    style_glyph(GL_BOX_H), S(ST_RESET));
		if (paths[i][e] == '\0') {
			file_tree_leaf(t, paths[i], off);
		} else {
			de = fold_dirs(paths, i, j, e);
			printf(" %s%.*s/%s\n", S(ST_LABEL), (int)(de - off),
			    paths[i] + off, S(ST_RESET));
			plen = prefix->len;
			sb_cat(prefix, last ? "   " : style_glyph(GL_TREE_PIPE));
			if (!last)
				sb_cat(prefix, "  ");
			file_tree_level(t, paths, i, j, de + 1, prefix);
			prefix->len = plen;
			prefix->d[plen] = '\0';
		}
		i = j;
	}
}

static void
print_file_tree(struct pkg *pkg)
{
	struct file_tree t = { .rootfd = pkg_get_rootfd() };
	struct pkg_file *file = NULL;
	vec_t(char *) paths = vec_init();
	sb_t prefix = sb_init();
	int64_t flatsize = 0;
	char size[8];
	char *path;
	size_t de;

	while (pkg_files(pkg, &file) == EPKG_OK) {
		path = NULL;
		pkg_asprintf(&path, "%Fn", file);
		/* Paths are absolute; anything else is shown as is. */
		if (path != NULL)
			vec_push(&paths, path);
	}
	if (paths.len == 0)
		return;
	qsort(paths.d, paths.len, sizeof(paths.d[0]), pathcmp);

	/* The directory every file lives under, such as "/usr/local/". */
	de = 0;
	if (paths.d[0][0] == '/')
		de = fold_dirs(paths.d, 0, paths.len, 0);
	printf("  %s%.*s%s\n", S(ST_LABEL), (int)(de + 1), paths.d[0],
	    S(ST_RESET));
	sb_cat(&prefix, "  ");
	file_tree_level(&t, paths.d, 0, paths.len, de + 1, &prefix);
	sb_fini(&prefix);

	pkg_get(pkg, PKG_ATTR_FLATSIZE, &flatsize);
	humanize_number(size, sizeof(size), flatsize, "B", HN_AUTOSCALE,
	    HN_IEC_PREFIXES | HN_DECIMAL);
	printf("  %s%d file%s %s %s%s", S(ST_DIM), t.files,
	    t.files == 1 ? "" : "s", style_glyph(GL_DOT), size, S(ST_RESET));
	if (t.missing > 0)
		printf(" %s%s%s %s%d missing%s", S(ST_DIM),
		    style_glyph(GL_DOT), S(ST_RESET), S(ST_ERROR), t.missing,
		    S(ST_RESET));
	putchar('\n');
	vec_free_and_free(&paths, free);
}

void
print_info(struct pkgdb *db, struct pkg * const pkg, uint64_t options)
{
	bool print_tag = false;
	bool show_locks = false;
	bool is_group = false;
	const char *repourl = NULL;
	unsigned opt;
	int cout = 0;		/* Number of characters output */
	int info_num;		/* Number of different data items to print */
	int outflags = PKG_MANIFEST_EMIT_LOCAL_METADATA;

	pkg_get(pkg, PKG_ATTR_REPOURL, &repourl);

	if (options & INFO_RAW) {
		switch (options & (INFO_RAW_YAML|INFO_RAW_JSON|INFO_RAW_JSON_COMPACT|INFO_RAW_UCL)) {
		case INFO_RAW_YAML:
			outflags |= PKG_MANIFEST_EMIT_PRETTY;
			break;
		case INFO_RAW_UCL:
			outflags |= PKG_MANIFEST_EMIT_UCL;
			break;
		case INFO_RAW_JSON:
			outflags |= PKG_MANIFEST_EMIT_JSON;
			break;
		case INFO_RAW_JSON_COMPACT:
			break;
		default:
			outflags |= PKG_MANIFEST_EMIT_UCL;
		}
		if (pkg_type(pkg) == PKG_REMOTE)
			outflags |= PKG_MANIFEST_EMIT_COMPACT;

		pkg_emit_manifest_file(pkg, stdout, outflags);
		return;
	}

	/* Show locking status when requested to display it and the
	   package is locally installed */
	if (pkg_type(pkg) == PKG_INSTALLED && (options & INFO_LOCKED) != 0)
		show_locks = true;
	if (pkg_type(pkg) == PKG_GROUP_REMOTE || pkg_type(pkg) == PKG_GROUP_INSTALLED)
		is_group = true;

	if (!quiet) {
		/* Print a tag-line identifying the package -- either
		   NAMEVER, ORIGIN or NAME (in that order of
		   preference).  This may be the only output from this
		   function */

		if (style_fancy() && (options & INFO_TAG_NAMEVER))
			cout = print_fancy_namever(pkg, is_group, true);
		else if (style_fancy() && (options & INFO_TAG_NAME))
			cout = print_fancy_namever(pkg, is_group, false);
		else if (options & INFO_TAG_NAMEVER) {
			if (is_group)
				cout = pkg_printf("@%n", pkg);
			else
				cout = pkg_printf("%n-%v", pkg, pkg);
		}
		else if (options & INFO_TAG_ORIGIN) {
			if (is_group)
				return;
			cout = pkg_printf("%o", pkg);
		}
		else if (options & INFO_TAG_NAME) {
			if (is_group)
				cout = pkg_printf("@%n", pkg);
			else
				cout = pkg_printf("%n", pkg);
		}
	}

	/* If we printed a tag, and there are no other items to print,
	   then just return now. If there's only one single-line item
	   to print, show it at column 32 on the same line. If there's
	   one multi-line item to print, start a new line. If there is
	   more than one item to print per pkg, use 'key : value'
	   style to show on a new line.  */

	info_num = 0;
	for (opt = 0x1U; opt <= INFO_LASTFIELD; opt <<= 1)
		if ((opt & options) != 0)
			info_num++;

	if (info_num == 0 && cout > 0) {
	        putchar('\n');
		return;
	}

	if (info_num == 1) {
		/* Only one item to print */
		print_tag = false;
		if (!quiet) {
			if (options & INFO_MULTILINE)
				printf(":\n");
			else {
				if (cout < 31)
					cout = 31 - cout;
				else
					cout = 1;
				printf("%*s", cout, " ");
			}
		}
	} else {
		/* Several items to print */
		print_tag = true;
		if (!quiet)
		        putchar('\n');
	}

	for (opt = 0x1; opt <= INFO_LASTFIELD; opt <<= 1) {
		if ((opt & options) == 0)
			continue;

		switch (opt) {
		case INFO_NAME:
			if (print_tag)
				info_label("Name", false);
			printf("%s", S(ST_NAME));
			pkg_printf("%n", pkg);
			printf("%s\n", S(ST_RESET));
			break;
		case INFO_INSTALLED:
			if (pkg_type(pkg) == PKG_INSTALLED) {
				if (print_tag) {
					info_label("Installed on", false);
					pkg_printf("%t%{%c %Z%}\n", pkg);
				}
			} else if (!print_tag)
				putchar('\n');
			break;
		case INFO_VERSION:
			if (is_group)
				break;
			if (print_tag)
				info_label("Version", false);
			printf("%s", S(ST_VERSION));
			pkg_printf("%v", pkg);
			printf("%s\n", S(ST_RESET));
			break;
		case INFO_ORIGIN:
			if (is_group)
				break;
			if (print_tag)
				info_label("Origin", false);
			pkg_printf("%o\n", pkg);
			break;
		case INFO_PREFIX:
			if (is_group)
				break;
			if (print_tag)
				info_label("Prefix", false);
			pkg_printf("%p\n", pkg);
			break;
		case INFO_REPOSITORY:
			if (is_group)
				break;
			if (pkg_type(pkg) == PKG_REMOTE &&
			    repourl != NULL && repourl[0] != '\0') {
				if (print_tag)
					info_label("Repository", false);
				pkg_printf("%N [%S]\n", pkg, repourl);
			} else if (!print_tag)
				putchar('\n');
			break;
		case INFO_CATEGORIES:
			if (is_group)
				break;
			if (print_tag)
				info_label("Categories", false);
			pkg_printf("%C%{%Cn%| %}\n", pkg);
			break;
		case INFO_LICENSES:
			if (is_group)
				break;
			if (print_tag)
				info_label("Licenses", false);
			pkg_printf("%L%{%Ln%| %l %}\n", pkg);
			break;
		case INFO_MAINTAINER:
			if (is_group)
				break;
			if (print_tag)
				info_label("Maintainer", false);
			pkg_printf("%m\n", pkg);
			break;
		case INFO_WWW:
			if (is_group)
				break;
			if (print_tag)
				info_label("WWW", false);
			printf("%s", S(ST_LINK));
			pkg_printf("%w", pkg);
			printf("%s\n", S(ST_RESET));
			break;
		case INFO_COMMENT:
			if (print_tag)
				info_label("Comment", false);
			printf("%s", S(ST_COMMENT));
			pkg_printf("%c", pkg);
			printf("%s\n", S(ST_RESET));
			break;
		case INFO_OPTIONS:
			if (pkg_list_count(pkg, PKG_OPTIONS) > 0) {
				if (print_tag)
					info_label("Options", true);
				if (quiet)
					pkg_printf("%O%{%-15On: %Ov\n%|%}", pkg);
				else
					pkg_printf("%O%{\t%-15On: %Ov\n%|%}", pkg);
			}
			break;
		case INFO_SHLIBS_REQUIRED:
			if (pkg_list_count(pkg, PKG_SHLIBS_REQUIRED) > 0) {
				if (print_tag)
					info_label("Shared Libs required", true);
				if (quiet)
					pkg_printf("%B%{%Bn\n%|%}", pkg);
				else
					pkg_printf("%B%{\t%Bn\n%|%}", pkg);
			}
			break;
		case INFO_SHLIBS_PROVIDED:
			if (pkg_list_count(pkg, PKG_SHLIBS_PROVIDED) > 0) {
				if (print_tag)
					info_label("Shared Libs provided", true);
				if (quiet)
					pkg_printf("%b%{%bn\n%|%}", pkg);
				else
					pkg_printf("%b%{\t%bn\n%|%}", pkg);
			}
			break;
		case INFO_REQUIRED:
			if (pkg_list_count(pkg, PKG_REQUIRES) > 0) {
				if (print_tag)
					info_label("Requires", true);
				if (quiet)
					pkg_printf("%Y%{%Yn\n%|%}", pkg);
				else
					pkg_printf("%Y%{\t%Yn\n%|%}", pkg);
			}
			break;
		case INFO_PROVIDED:
			if (pkg_list_count(pkg, PKG_PROVIDES) > 0) {
				if (print_tag)
					info_label("Provides", true);
				if (quiet)
					pkg_printf("%y%{%yn\n%|%}", pkg);
				else
					pkg_printf("%y%{\t%yn\n%|%}", pkg);
			}
			break;
		case INFO_ANNOTATIONS:
			if (is_group)
				break;
			if (print_tag)
				info_label("Annotations", true);
			if (quiet)
				pkg_printf("%A%{%-15An: %Av\n%|%}", pkg);
			else
				pkg_printf("%A%{\t%-15An: %Av\n%|%}", pkg);
			break;
		case INFO_FLATSIZE:
			if (is_group)
				break;
			if (print_tag)
				info_label("Flat size", false);
			pkg_printf("%#sB\n", pkg);
			break;
		case INFO_PKGSIZE: /* Remote pkgs only */
			if (pkg_type(pkg) == PKG_REMOTE) {
				if (print_tag)
					info_label("Pkg size", false);
				pkg_printf("%#xB\n", pkg);
			} else if (!print_tag)
			        putchar('\n');
			break;
		case INFO_DESCR:
			if (is_group)
				break;
			if (print_tag)
				info_label("Description", true);
			pkg_printf("%e\n", pkg);
			break;
		case INFO_MESSAGE:
			if (is_group)
				break;
			if (print_tag)
				info_label("Message", true);
			if (pkg_has_message(pkg))
				pkg_printf("%M\n", pkg);
			break;
		case INFO_DEPS:
			if (style_fancy() && !quiet) {
				if (pkg_list_count(pkg, PKG_DEPS) > 0) {
					if (print_tag)
						info_label("Depends on", true);
					print_dep_tree(db, pkg, false,
					    show_locks);
				} else if (!print_tag)
					printf("  %s(no dependencies)%s\n",
					    S(ST_DIM), S(ST_RESET));
			} else if (pkg_list_count(pkg, PKG_DEPS) > 0) {
				if (print_tag)
					info_label("Depends on", true);
				if (quiet) {
					if (show_locks)
						pkg_printf("%d%{%dn-%dv%#dk\n%|%}", pkg);
					else
						pkg_printf("%d%{%dn-%dv\n%|%}", pkg);
				} else {
					if (show_locks)
						pkg_printf("%d%{\t%dn-%dv%#dk\n%|%}", pkg);
					else
						pkg_printf("%d%{\t%dn-%dv\n%|%}", pkg);
				}
			}
			if (db != NULL)
				print_depinfo(db, pkg, print_tag,
				    PKG_ATTR_SHLIBS_REQUIRED, "Shared library requirements",
				    pkgdb_query_shlib_provide,
				    PKG_ATTR_REQUIRES, "Requirements",
				    pkgdb_query_provide);
			break;
		case INFO_RDEPS:
			if (style_fancy() && !quiet) {
				if (pkg_list_count(pkg, PKG_RDEPS) > 0) {
					if (print_tag)
						info_label("Required by", true);
					print_dep_tree(db, pkg, true,
					    show_locks);
				} else if (!print_tag)
					printf("  %s(nothing requires it)%s\n",
					    S(ST_DIM), S(ST_RESET));
			} else if (pkg_list_count(pkg, PKG_RDEPS) > 0) {
				if (print_tag)
					info_label("Required by", true);
				if (quiet) {
					if (show_locks)
						pkg_printf("%r%{%rn-%rv%#rk\n%|%}", pkg);
					else
						pkg_printf("%r%{%rn-%rv\n%|%}", pkg);
				} else {
					if (show_locks)
						pkg_printf("%r%{\t%rn-%rv%#rk\n%|%}", pkg);
					else
						pkg_printf("%r%{\t%rn-%rv\n%|%}", pkg);
				}
			}
			if (db != NULL)
				print_depinfo(db, pkg, print_tag,
				    PKG_ATTR_SHLIBS_PROVIDED, "Shared library provided",
				    pkgdb_query_shlib_require,
				    PKG_ATTR_PROVIDES, "Provides",
				    pkgdb_query_require);
			break;
		case INFO_FILES: /* Installed pkgs only */
			if (style_fancy() && !quiet && !print_tag &&
			    pkg_type(pkg) != PKG_REMOTE &&
			    pkg_list_count(pkg, PKG_FILES) == 0)
				printf("  %s(no files)%s\n", S(ST_DIM),
				    S(ST_RESET));
			if (pkg_type(pkg) != PKG_REMOTE &&
			    pkg_list_count(pkg, PKG_FILES) > 0) {
				if (print_tag)
					info_label("Files", true);
				if (style_fancy() && !quiet)
					print_file_tree(pkg);
				else if (quiet)
					pkg_printf("%F%{%Fn\n%|%}", pkg);
				else
					pkg_printf("%F%{\t%Fn\n%|%}", pkg);
			}
			break;
		case INFO_DIRS:	/* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    pkg_list_count(pkg, PKG_DIRS) > 0) {
				if (print_tag)
					info_label("Directories", true);
				if (quiet)
					pkg_printf("%D%{%Dn\n%|%}", pkg);
				else
					pkg_printf("%D%{\t%Dn\n%|%}", pkg);
			}
			break;
		case INFO_USERS: /* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    pkg_list_count(pkg, PKG_USERS) > 0) {
				if (print_tag)
					info_label("Users", false);
				pkg_printf("%U%{%Un%| %}\n", pkg);
			}
			break;
		case INFO_GROUPS: /* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    pkg_list_count(pkg, PKG_GROUPS) > 0) {
				if (print_tag)
					info_label("Groups", false);
				pkg_printf("%G%{%Gn%| %}\n", pkg);
			}
			break;
		case INFO_ARCH:
			if (is_group)
				break;
			if (print_tag)
				info_label("Architecture", false);
			pkg_printf("%q\n", pkg);
			break;
		case INFO_REPOURL:
			if (pkg_type(pkg) == PKG_REMOTE &&
			    repourl != NULL && repourl[0] != '\0') {
				if (print_tag)
					info_label("Pkg URL", false);
				if (repourl[strlen(repourl) -1] == '/')
					pkg_printf("%S%R\n", repourl, pkg);
				else
					pkg_printf("%S/%R\n", repourl, pkg);
			} else if (!print_tag)
				putchar('\n');
			break;
		case INFO_LOCKED:
			if (print_tag)
				info_label("Locked", false);
			pkg_printf("%?k\n", pkg);
			break;
		}
	}
}

enum pkg_display_type {
	PKG_DISPLAY_LOCKED = 0,
	PKG_DISPLAY_INSTALL,
	PKG_DISPLAY_UPGRADE,
	PKG_DISPLAY_DOWNGRADE,
	PKG_DISPLAY_REINSTALL,
	PKG_DISPLAY_DELETE,
	PKG_DISPLAY_FETCH,
	PKG_DISPLAY_GROUP_INSTALL,
	PKG_DISPLAY_GROUP_UPGRADE,
	PKG_DISPLAY_MAX
};
struct pkg_solved_display {
	struct pkg *new, *old;
	enum pkg_display_type display_type;
	pkg_solved_t solved_type;
	bool vulnerable;
};

typedef vec_t(struct pkg_solved_display *) pkg_solved_display_t;

static void
set_jobs_summary_pkg(struct pkg_jobs *jobs, struct pkg *new_pkg,
    struct pkg *old_pkg, pkg_solved_t type, int64_t *oldsize,
    int64_t *newsize, int64_t *dlsize, pkg_solved_display_t *disp,
    struct jobs_sum_number *sum)
{
	const char *repopath = NULL, *destdir;
	char path[MAXPATHLEN];
	int ret;
	struct stat st;
	int64_t flatsize, oldflatsize, pkgsize;
	struct pkg_solved_display *it;

	flatsize = oldflatsize = pkgsize = 0;

	pkg_get(new_pkg, PKG_ATTR_FLATSIZE, &flatsize);
	pkg_get(new_pkg, PKG_ATTR_PKGSIZE, &pkgsize);
	pkg_get(new_pkg, PKG_ATTR_REPOPATH, &repopath);
	if (old_pkg != NULL)
		pkg_get(old_pkg, PKG_ATTR_FLATSIZE, &oldflatsize);

	it = xmalloc(sizeof (*it));
	it->new = new_pkg;
	it->old = old_pkg;
	it->solved_type = type;
	it->vulnerable = false;
	it->display_type = PKG_DISPLAY_MAX;

	if (old_pkg != NULL && pkg_is_locked(old_pkg)) {
		it->display_type = PKG_DISPLAY_LOCKED;
		vec_push(&disp[it->display_type], it);
		return;
	}

	destdir = pkg_jobs_destdir(jobs);

	switch (type) {
	case PKG_SOLVED_INSTALL:
	case PKG_SOLVED_UPGRADE:
		if (destdir == NULL)
			ret = pkg_repo_cached_name(new_pkg, path, sizeof(path));
		else if (repopath != NULL) {
			snprintf(path, sizeof(path), "%s/%s", destdir, repopath);
			ret = EPKG_OK;
		} else
			break;

		if ((ret == EPKG_OK || ret == EPKG_FATAL) && (stat(path, &st) == -1 || pkgsize != st.st_size)) {
			/* file looks corrupted (wrong size),
					   assume a checksum mismatch will
					   occur later and the file will be
					   fetched from remote again */
			*dlsize += pkgsize;
			nbtodl += 1;
		}

		if (pkg_type(new_pkg) == PKG_GROUP_REMOTE) {
			if (old_pkg == NULL) {
				it->display_type = PKG_DISPLAY_GROUP_INSTALL;
				sum->group_install++;
			} else {
				it->display_type = PKG_DISPLAY_GROUP_UPGRADE;
				sum->group_upgrade++;
			}
		} else {
			if (old_pkg != NULL) {
				switch (pkg_version_change_between(new_pkg, old_pkg)) {
				case PKG_DOWNGRADE:
					it->display_type = PKG_DISPLAY_DOWNGRADE;
					sum->downgrade++;
					break;
				case PKG_REINSTALL:
					it->display_type = PKG_DISPLAY_REINSTALL;
					sum->reinstall++;
					break;
				case PKG_UPGRADE:
					it->display_type = PKG_DISPLAY_UPGRADE;
					sum->upgrade++;
					break;
				}
				*oldsize += oldflatsize;
				*newsize += flatsize;
			} else {
				it->display_type = PKG_DISPLAY_INSTALL;
				sum->install++;
				*newsize += flatsize;
			}
		}
		break;
	case PKG_SOLVED_DELETE:
		*oldsize += flatsize;
		it->display_type = PKG_DISPLAY_DELETE;
		sum->delete++;
		break;
	case PKG_SOLVED_UPGRADE_INSTALL:
	case PKG_SOLVED_UPGRADE_REMOVE:
		/* Ignore split-upgrade packages for display */
		free(it);
		return;

	case PKG_SOLVED_FETCH:
		*newsize += pkgsize;
		it->display_type = PKG_DISPLAY_FETCH;
		if (destdir == NULL)
			pkg_repo_cached_name(new_pkg, path, sizeof(path));
		else
			snprintf(path, sizeof(path), "%s/%s", destdir, repopath);

		if (stat(path, &st) != -1) {
			*oldsize += st.st_size;

			if (pkgsize != st.st_size)
				*dlsize += pkgsize;
			else {
				free(it);
				return;
			}
		}
		else
			*dlsize += pkgsize;
		sum->fetch++;

		break;
	}
	vec_push(&disp[it->display_type], it);
}

static void
display_summary_item(struct pkg_solved_display *it, int64_t dlsize)
{
	const char *why = NULL;
	int64_t pkgsize = 0;
	char size[8], tlsize[8];
	const char *type;

	pkg_get(it->new, PKG_ATTR_PKGSIZE, &pkgsize);

	switch (it->display_type) {
	case PKG_DISPLAY_LOCKED:
		pkg_printf("\tPackage %n-%v is locked ", it->old, it->old);
		switch (it->solved_type) {
		case PKG_SOLVED_INSTALL:
		case PKG_SOLVED_UPGRADE:
		case PKG_SOLVED_UPGRADE_INSTALL:
			/* If it's a new install, then it
			 * cannot have been locked yet. */
			switch (pkg_version_change_between(it->old, it->new)) {
			case PKG_DOWNGRADE:
				type = "downgraded";
				break;
			case PKG_REINSTALL:
				type = "reinstalled";
				break;
			case PKG_UPGRADE:
				type = "upgraded";
				break;
			default: /* appease compiler warnings */
				type = "upgraded";
				break;
			}
			pkg_printf("and may not be %S to version %v\n", type,
			    it->new);
			break;
		case PKG_SOLVED_DELETE:
		case PKG_SOLVED_UPGRADE_REMOVE:
			printf("and may not be deinstalled\n");
			return;
		case PKG_SOLVED_FETCH:
			printf("but a new package can still be fetched\n");
			break;
		}
		break;
	case PKG_DISPLAY_DELETE:
		pkg_get(it->new, PKG_ATTR_REASON, &why);
		pkg_printf("\t%n: %v", it->new, it->new);
		if (why != NULL)
			printf(" (%s)", why);
		putchar('\n');
		break;
	case PKG_DISPLAY_INSTALL:
		pkg_printf("\t%n: %v", it->new, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		if (it->vulnerable)
			printf(" (vulnerable!)");
		putchar('\n');
		break;
	case PKG_DISPLAY_UPGRADE:
		pkg_printf("\t%n: %v -> %v", it->new, it->old, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		if (it->vulnerable)
			printf(" (vulnerable!)");
		putchar('\n');
		break;
	case PKG_DISPLAY_DOWNGRADE:
		pkg_printf("\t%n: %v -> %v", it->new, it->old, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		if (it->vulnerable)
			printf(" (vulnerable!)");
		putchar('\n');
		break;
	case PKG_DISPLAY_REINSTALL:
		pkg_get(it->new, PKG_ATTR_REASON, &why);
		pkg_printf("\t%n-%v", it->new, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		if (why != NULL)
			printf(" (%s)", why);
		if (it->vulnerable)
			printf(" (vulnerable!)");
		putchar('\n');
		break;
	case PKG_DISPLAY_FETCH:
		humanize_number(size, sizeof(size), pkgsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES);
		humanize_number(tlsize, sizeof(tlsize), dlsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES);

		pkg_printf("\t%n: %v ", it->new, it->new);
		printf("(%s: %.2f%% of the %s to download)\n", size,
		    ((double)100 * pkgsize) / (double)dlsize, tlsize);
		break;
	case PKG_DISPLAY_GROUP_UPGRADE:
		pkg_printf("\t%n", it->new, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		putchar('\n');
		break;
	case PKG_DISPLAY_GROUP_INSTALL:
		pkg_printf("\t@%n", it->new, it->new);
		if (pkg_repos_total_count() > 1)
			pkg_printf(" [%N]", it->new);
		putchar('\n');
		break;
	default:
		break;
	}
}


static const char* pkg_display_messages[PKG_DISPLAY_MAX + 1] = {
	[PKG_DISPLAY_LOCKED] = "Installed packages LOCKED",
	[PKG_DISPLAY_DELETE] = "Installed packages to be REMOVED",
	[PKG_DISPLAY_INSTALL] = "New packages to be INSTALLED",
	[PKG_DISPLAY_GROUP_UPGRADE] = "New groups to be UPGRADED",
	[PKG_DISPLAY_DOWNGRADE] = "Installed packages to be DOWNGRADED",
	[PKG_DISPLAY_REINSTALL] = "Installed packages to be REINSTALLED",
	[PKG_DISPLAY_FETCH] = "New packages to be FETCHED",
	[PKG_DISPLAY_GROUP_INSTALL] = "New groups to be INSTALLED",
	[PKG_DISPLAY_UPGRADE] = "Installed packages to be UPGRADED",
	[PKG_DISPLAY_MAX] = NULL
};

static int
namecmp(const void *a, const void *b)
{
	const struct pkg_solved_display *sda = *(const struct pkg_solved_display **) a;
	const struct pkg_solved_display *sdb = *(const struct pkg_solved_display **) b;

	return (pkg_namecmp(sda->new, sdb->new));
}

static hash_t *
audit_check_summary(pkg_solved_display_t *disp)
{
	int pfd[2];
	pid_t cld;
	struct pkg_audit *audit;
	struct pkg_audit_issues *issues;
	hash_t *vulnerable = NULL;
	FILE *in;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	if (pipe(pfd) == -1)
		return (NULL);

	cld = fork();
	switch (cld) {
	case 0:
		close(pfd[0]);
		FILE *out = fdopen(pfd[1], "w");
		if (out == NULL)
			_exit(EXIT_FAILURE);

		audit = pkg_audit_new();
		if (pkg_audit_load(audit, NULL) != EPKG_OK) {
			pkg_audit_free(audit);
			fclose(out);
			_exit(EXIT_SUCCESS);
		}

		pkg_drop_privileges();

#ifdef HAVE_CAPSICUM
#ifndef COVERAGE
		if (cap_enter() < 0 && errno != ENOSYS) {
			warn("cap_enter() failed");
			pkg_audit_free(audit);
			fclose(out);
			_exit(EXIT_FAILURE);
		}
#endif
#endif

		if (pkg_audit_process(audit) == EPKG_OK) {
			for (int type = 0; type < PKG_DISPLAY_MAX; type++) {
				vec_foreach(disp[type], i) {
					const char *name = NULL, *version = NULL;
					issues = NULL;
					if (pkg_audit_is_vulnerable(audit,
					    disp[type].d[i]->new, &issues,
					    true)) {
						pkg_get(disp[type].d[i]->new,
						    PKG_ATTR_NAME, &name);
						pkg_get(disp[type].d[i]->new,
						    PKG_ATTR_VERSION, &version);
						if (name != NULL &&
						    version != NULL)
							fprintf(out, "%s-%s\n",
							    name, version);
					}
					pkg_audit_issues_free(issues);
				}
			}
		}

		pkg_audit_free(audit);
		fclose(out);
		_exit(EXIT_SUCCESS);
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		return (NULL);
	default:
		/* Parent */
		close(pfd[1]);
		in = fdopen(pfd[0], "r");
		if (in == NULL) {
			close(pfd[0]);
			waitpid(cld, NULL, 0);
			return (NULL);
		}

		vulnerable = hash_new();
		while ((linelen = getline(&line, &linecap, in)) > 0) {
			if (line[linelen - 1] == '\n')
				line[linelen - 1] = '\0';
			hash_safe_add(vulnerable, line, NULL, NULL);
		}
		free(line);
		fclose(in);

		while (waitpid(cld, NULL, 0) == -1) {
			if (errno != EINTR)
				break;
		}

		if (hash_count(vulnerable) == 0) {
			hash_destroy(vulnerable);
			return (NULL);
		}

		return (vulnerable);
	}
}

/*
 * "❯ Question?" for the fancy output.  Pass the result to query_yesno() as a
 * "%S" argument: its format string goes through pkg_printf, where %s does
 * not mean string.
 */
static const char *
fancy_prompt(const char *question)
{
	static char buf[256];

	snprintf(buf, sizeof(buf), "\n%s%s%s %s%s%s ", S(ST_ACCENT),
	    style_glyph(GL_PROMPT), S(ST_RESET), S(ST_BOLD), question,
	    S(ST_RESET));
	return (buf);
}

bool
confirm_jobs_action(struct pkg_jobs *jobs)
{
	struct pkg *new_pkg, *old_pkg;
	void *iter = NULL;
	int type, nremovals = 0, explicit_removals = 0;
	bool automatic, ret;

	type = pkg_jobs_type(jobs);

	/*
	 * There is no question to ask when the user asked to assume yes,
	 * and no question at all in quiet mode.
	 */
	if (yes || quiet)
		return (true);

	if (type != PKG_JOBS_INSTALL && type != PKG_JOBS_UPGRADE) {
		if (style_fancy())
			return (query_yesno(false, "%S",
			    fancy_prompt("Proceed with this action?")));
		return (query_yesno(false, "\nProceed with this action? "));
	}

	sb_t names = sb_init();
	char prompt[BUFSIZ];

	while (pkg_jobs_iter(jobs, &iter, &new_pkg, &old_pkg, &type)) {
		if (type != PKG_SOLVED_DELETE)
			continue;

		pkg_get(new_pkg, PKG_ATTR_AUTOMATIC, &automatic);
		nremovals++;
		if (automatic)
			continue;

		explicit_removals++;
		if (names.len > 0)
			sb_cat_c(&names, ' ');
		pkg_sb_printf(&names, "%n-%v", new_pkg, new_pkg);
	}

	if (nremovals > 0 && style_fancy()) {
		printf("\n%s%s %d package(s) will be REMOVED by this "
		    "operation%s", S(ST_WARN), style_glyph(GL_WARN), nremovals,
		    S(ST_RESET));
		if (explicit_removals > 0)
			printf(", including %s%d%s that you installed "
			    "explicitly: %s%s%s", S(ST_BOLD), explicit_removals,
			    S(ST_RESET), S(ST_REMOVE), sb_str(&names),
			    S(ST_RESET));
		putchar('\n');
	} else if (nremovals > 0) {
		printf("\n>>> %d package(s) will be REMOVED by this operation",
		    nremovals);
		if (explicit_removals > 0)
			printf(", including %d that you installed "
			    "explicitly: %s", explicit_removals,
			    sb_str(&names));
		putchar('\n');
	}

	if (style_fancy())
		strlcpy(prompt, fancy_prompt("Proceed with this action?"),
		    sizeof(prompt));
	else if (nremovals > 0)
		snprintf(prompt, sizeof(prompt),
		    "\nProceed with this action? "
		    "(%d package(s) will be REMOVED) ", nremovals);
	else
		snprintf(prompt, sizeof(prompt),
		    "\nProceed with this action? ");

	ret = query_yesno(false, prompt);

	if (ret && explicit_removals > 0 && style_fancy()) {
		printf("\n%s%s These packages were installed explicitly and "
		    "removing them makes them disappear until they are "
		    "installed again:%s %s%s%s\n", S(ST_WARN),
		    style_glyph(GL_WARN), S(ST_RESET), S(ST_REMOVE),
		    sb_str(&names), S(ST_RESET));
		ret = query_yesno(false, "%S",
		    fancy_prompt("Confirm their removal?"));
	} else if (ret && explicit_removals > 0) {
		printf("\n>>> These packages were installed explicitly and "
		    "removing them makes them disappear until they are "
		    "installed again: %s\n", sb_str(&names));
		ret = query_yesno(false, "\nConfirm their removal? ");
	}

	sb_fini(&names);

	return (ret);
}

struct fancy_section {
	enum style_role role;
	enum style_glyph glyph;
	const char *label;
	const char *count_label;
};

static const struct fancy_section fancy_sections[PKG_DISPLAY_MAX] = {
	[PKG_DISPLAY_LOCKED] =
	    { ST_LOCKED, GL_LOCKED, "Locked", NULL },
	[PKG_DISPLAY_INSTALL] =
	    { ST_INSTALL, GL_INSTALL, "Installing", "new" },
	[PKG_DISPLAY_UPGRADE] =
	    { ST_UPGRADE, GL_UPGRADE, "Upgrading", "upgraded" },
	[PKG_DISPLAY_DOWNGRADE] =
	    { ST_DOWNGRADE, GL_DOWNGRADE, "Downgrading", "downgraded" },
	[PKG_DISPLAY_REINSTALL] =
	    { ST_REINSTALL, GL_REINSTALL, "Reinstalling", "reinstalled" },
	[PKG_DISPLAY_DELETE] =
	    { ST_REMOVE, GL_REMOVE, "Removing", "removed" },
	[PKG_DISPLAY_FETCH] =
	    { ST_FETCH, GL_FETCH, "Fetching", "fetched" },
	[PKG_DISPLAY_GROUP_INSTALL] =
	    { ST_GROUP, GL_GROUP, "Installing groups", "new groups" },
	[PKG_DISPLAY_GROUP_UPGRADE] =
	    { ST_GROUP, GL_GROUP, "Upgrading groups", "upgraded groups" },
};

static const char *
attr_str(struct pkg *p, pkg_attr a)
{
	const char *s = NULL;

	if (p != NULL)
		pkg_get(p, a, &s);
	return (s != NULL ? s : "");
}

static bool
fancy_is_pair(struct pkg_solved_display *it)
{
	return (it->old != NULL &&
	    (it->display_type == PKG_DISPLAY_UPGRADE ||
	    it->display_type == PKG_DISPLAY_DOWNGRADE ||
	    it->display_type == PKG_DISPLAY_LOCKED));
}

/* The version column of a row: a single version or "old → new". */
static void
fancy_version(sb_t *sb, struct pkg_solved_display *it, int oldw)
{
	const char *nv = attr_str(it->new, PKG_ATTR_VERSION);

	switch (it->display_type) {
	case PKG_DISPLAY_UPGRADE:
		style_version_pair(sb, attr_str(it->old, PKG_ATTR_VERSION), nv,
		    ST_VER_UP_OLD, ST_VER_UP_NEW, oldw);
		break;
	case PKG_DISPLAY_DOWNGRADE:
		style_version_pair(sb, attr_str(it->old, PKG_ATTR_VERSION), nv,
		    ST_VER_DOWN_OLD, ST_VER_DOWN_NEW, oldw);
		break;
	case PKG_DISPLAY_LOCKED:
		if (it->solved_type == PKG_SOLVED_DELETE ||
		    it->solved_type == PKG_SOLVED_UPGRADE_REMOVE) {
			sb_printf(sb, "%s%s%s", S(ST_DIM),
			    attr_str(it->old, PKG_ATTR_VERSION), S(ST_RESET));
			break;
		}
		style_version_pair(sb, attr_str(it->old, PKG_ATTR_VERSION), nv,
		    ST_DIM, ST_DIM, oldw);
		break;
	case PKG_DISPLAY_DELETE:
		sb_printf(sb, "%s%s%s", S(ST_VER_UP_OLD), nv, S(ST_RESET));
		break;
	default:
		sb_printf(sb, "%s%s%s", S(ST_VERSION), nv, S(ST_RESET));
		break;
	}
}

static void
fancy_summary_item(struct pkg_solved_display *it, int namew, int oldw,
    int verw, int64_t dlsize)
{
	sb_t row = sb_init(), ver = sb_init();
	const char *why = attr_str(it->new, PKG_ATTR_REASON);
	int64_t pkgsize = 0;
	char size[8], tlsize[8];

	sb_cat(&row, "    ");
	sb_printf(&ver, "%s%s%s", S(ST_NAME), attr_str(it->new, PKG_ATTR_NAME),
	    S(ST_RESET));
	style_pad(&row, sb_str(&ver), namew + 2);

	sb_reset(&ver);
	fancy_version(&ver, it, oldw);
	style_pad(&row, sb_str(&ver), verw + 2);
	sb_fini(&ver);

	pkg_get(it->new, PKG_ATTR_PKGSIZE, &pkgsize);
	switch (it->display_type) {
	case PKG_DISPLAY_LOCKED:
		sb_printf(&row, "%s%s locked", S(ST_LOCKED),
		    style_glyph(GL_LOCKED));
		if (it->solved_type == PKG_SOLVED_DELETE ||
		    it->solved_type == PKG_SOLVED_UPGRADE_REMOVE)
			sb_cat(&row, ", may not be deinstalled");
		else if (it->solved_type == PKG_SOLVED_FETCH)
			sb_cat(&row, ", new package can still be fetched");
		else
			sb_cat(&row, ", may not be changed");
		sb_cat(&row, S(ST_RESET));
		break;
	case PKG_DISPLAY_DELETE:
		if (*why != '\0')
			sb_printf(&row, "%s%s%s", S(ST_DIM), why, S(ST_RESET));
		break;
	case PKG_DISPLAY_FETCH:
		humanize_number(size, sizeof(size), pkgsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES | HN_DECIMAL);
		humanize_number(tlsize, sizeof(tlsize), dlsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES | HN_DECIMAL);
		sb_printf(&row, "%s%9s  %4.1f%% of %s%s", S(ST_DIM), size,
		    dlsize > 0 ? (100.0 * pkgsize) / dlsize : 0.0, tlsize,
		    S(ST_RESET));
		break;
	case PKG_DISPLAY_GROUP_INSTALL:
	case PKG_DISPLAY_GROUP_UPGRADE:
		break;
	default:
		humanize_number(size, sizeof(size), pkgsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES | HN_DECIMAL);
		sb_printf(&row, "%s%9s%s", S(ST_DIM), size, S(ST_RESET));
		if (it->display_type == PKG_DISPLAY_REINSTALL && *why != '\0')
			sb_printf(&row, "  %s%s%s", S(ST_DIM), why,
			    S(ST_RESET));
		break;
	}

	if (pkg_repos_total_count() > 1 &&
	    it->display_type != PKG_DISPLAY_DELETE &&
	    it->display_type != PKG_DISPLAY_LOCKED)
		sb_printf(&row, "  %s[%s]%s", S(ST_REPO),
		    attr_str(it->new, PKG_ATTR_REPONAME), S(ST_RESET));
	if (it->vulnerable)
		sb_printf(&row, "  %s%s vulnerable%s", S(ST_ERROR),
		    style_glyph(GL_WARN), S(ST_RESET));

	/* Trim the padding we may have left at the end of the row. */
	while (row.len > 0 && row.d[row.len - 1] == ' ')
		row.d[--row.len] = '\0';
	printf("%s\n", sb_str(&row));
	sb_fini(&row);
}

static void
fancy_size_line(int64_t oldsize, int64_t newsize, int64_t dlsize)
{
	char size[8];
	bool sep = false;

	if (newsize == oldsize && dlsize <= 0)
		return;
	printf("  ");
	if (newsize != oldsize) {
		humanize_number(size, sizeof(size), llabs(newsize - oldsize),
		    "B", HN_AUTOSCALE, HN_IEC_PREFIXES | HN_DECIMAL);
		printf("%sDisk%s %s%s%s%s", S(ST_DIM), S(ST_RESET),
		    newsize > oldsize ? S(ST_WARN) : S(ST_OK),
		    newsize > oldsize ? "+" : "-", size, S(ST_RESET));
		sep = true;
	}
	if (dlsize > 0) {
		humanize_number(size, sizeof(size), dlsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES | HN_DECIMAL);
		printf("%s%sDownload%s %s%s%s", sep ? "    " : "", S(ST_DIM),
		    S(ST_RESET), S(ST_FETCH), size, S(ST_RESET));
	}
	putchar('\n');
}

static int
print_jobs_summary_fancy(pkg_solved_display_t *disp, const char *title,
    int64_t oldsize, int64_t newsize, int64_t dlsize, int vuln_count)
{
	const struct fancy_section *sec;
	struct pkg_solved_display *it;
	int namew = 0, oldw = 0, neww = 0, verw = 0, rulew, w;
	int type, displayed = 0;
	bool first = true;

	/* Shared columns for every section, so the whole summary lines up. */
	for (type = 0; type < PKG_DISPLAY_MAX; type++) {
		vec_foreach(disp[type], i) {
			it = disp[type].d[i];
			w = strlen(attr_str(it->new, PKG_ATTR_NAME));
			if (w > namew)
				namew = w;
			if (fancy_is_pair(it)) {
				w = strlen(attr_str(it->old, PKG_ATTR_VERSION));
				if (w > oldw)
					oldw = w;
				w = strlen(attr_str(it->new, PKG_ATTR_VERSION));
				if (w > neww)
					neww = w;
			}
		}
	}
	if (neww > 0)
		verw = oldw + 2 + style_width(style_glyph(GL_ARROW)) + neww;
	for (type = 0; type < PKG_DISPLAY_MAX; type++) {
		vec_foreach(disp[type], i) {
			it = disp[type].d[i];
			if (fancy_is_pair(it))
				continue;
			w = strlen(attr_str(it->new, PKG_ATTR_VERSION));
			if (w > verw)
				verw = w;
		}
	}

	rulew = 4 + namew + 2 + verw + 2 + 9;
	if (title != NULL && (int)style_width(title) + 2 > rulew)
		rulew = style_width(title) + 2;
	if (rulew > style_term_cols())
		rulew = style_term_cols();

	if (title != NULL) {
		printf("%s%s%s %s%s\n", S(ST_ACCENT), style_glyph(GL_PROMPT),
		    S(ST_BOLD), title, S(ST_RESET));
		style_rule(stdout, rulew);
	}

	for (type = 0; type < PKG_DISPLAY_MAX; type++) {
		if (disp[type].len == 0)
			continue;
		sec = &fancy_sections[type];
		if (!first || title != NULL)
			putchar('\n');
		first = false;
		printf("%s%s %s%s %s%zu%s\n", S(sec->role),
		    style_glyph(sec->glyph), sec->label, S(ST_RESET),
		    S(ST_DIM), disp[type].len, S(ST_RESET));
		qsort(disp[type].d, disp[type].len, sizeof(disp[type].d[0]),
		    namecmp);
		vec_foreach(disp[type], i) {
			fancy_summary_item(disp[type].d[i], namew, oldw, verw,
			    dlsize);
			displayed++;
		}
	}

	putchar('\n');
	style_rule(stdout, rulew);
	printf(" ");
	first = true;
	for (type = 0; type < PKG_DISPLAY_MAX; type++) {
		sec = &fancy_sections[type];
		if (disp[type].len == 0 || sec->count_label == NULL)
			continue;
		if (!first)
			printf(" %s%s%s", S(ST_DIM), style_glyph(GL_DOT),
			    S(ST_RESET));
		printf(" %s%zu%s %s", S(sec->role), disp[type].len,
		    S(ST_RESET), sec->count_label);
		first = false;
	}
	putchar('\n');

	fancy_size_line(oldsize, newsize, dlsize);

	if (vuln_count > 0)
		printf("\n%s%s %d package(s) have known vulnerabilities%s\n",
		    S(ST_ERROR), style_glyph(GL_WARN), vuln_count,
		    S(ST_RESET));

	for (type = 0; type < PKG_DISPLAY_MAX; type++)
		vec_free_and_free(&disp[type], free);

	return (displayed);
}

int
print_jobs_summary(struct pkg_jobs *jobs, const char *msg, ...)
{
	struct pkg *new_pkg, *old_pkg;
	void *iter = NULL;
	char size[8];
	va_list ap;
	int type, displayed = 0;
	int64_t dlsize, oldsize, newsize;
	pkg_solved_display_t disp[PKG_DISPLAY_MAX];
	bool first = true;
	size_t bytes_change, limbytes;
	struct jobs_sum_number sum;

	dlsize = oldsize = newsize = 0;
	type = pkg_jobs_type(jobs);
	memset(disp, 0, sizeof(*disp) * PKG_DISPLAY_MAX);
	memset(&sum, 0, sizeof(sum));

	nbtodl = 0;
	while (pkg_jobs_iter(jobs, &iter, &new_pkg, &old_pkg, &type)) {
		set_jobs_summary_pkg(jobs, new_pkg, old_pkg, type, &oldsize,
		&newsize, &dlsize, disp, &sum);
	}

	/* Check packages against audit database for vulnerabilities */
	hash_t *vulnerable = audit_check_summary(disp);
	int vuln_count = 0;
	if (vulnerable != NULL) {
		for (type = 0; type < PKG_DISPLAY_MAX; type++) {
			vec_foreach(disp[type], i) {
				const char *n = NULL, *v = NULL;
				char nv[BUFSIZ];
				pkg_get(disp[type].d[i]->new, PKG_ATTR_NAME, &n);
				pkg_get(disp[type].d[i]->new, PKG_ATTR_VERSION, &v);
				if (n != NULL && v != NULL) {
					snprintf(nv, sizeof(nv), "%s-%s", n, v);
					if (hash_get(vulnerable, nv) != NULL) {
						disp[type].d[i]->vulnerable = true;
						vuln_count++;
					}
				}
			}
		}
		hash_destroy(vulnerable);
	}

	if (style_fancy()) {
		char *title = NULL;

		if (msg != NULL) {
			va_start(ap, msg);
			if (vasprintf(&title, msg, ap) < 0)
				title = NULL;
			va_end(ap);
		}
		if (title != NULL) {
			size_t l = strlen(title);
			while (l > 0 && (title[l - 1] == '\n' ||
			    title[l - 1] == ':' || title[l - 1] == ' '))
				title[--l] = '\0';
		}
		displayed = print_jobs_summary_fancy(disp, title, oldsize,
		    newsize, dlsize, vuln_count);
		free(title);
		return (displayed);
	}

	for (type = 0; type < PKG_DISPLAY_MAX; type ++) {
		if (disp[type].len != 0) {
			/* Space between each section. */
			if (!first)
				putchar('\n');
			else
				first = false;
			if (msg != NULL) {
				va_start(ap, msg);
				vprintf(msg, ap);
				va_end(ap);
				fflush(stdout);
				msg = NULL;
			}
			printf("%s:\n", pkg_display_messages[type]);
			qsort(disp[type].d, disp[type].len, sizeof(disp[type].d[0]), namecmp);
			vec_foreach(disp[type], i) {
				display_summary_item(disp[type].d[i], dlsize);
				displayed ++;
			}
			vec_free_and_free(&disp[type], free);
		}
	}

	limbytes = pkg_object_int(pkg_config_get("WARN_SIZE_LIMIT"));
	bytes_change = (size_t)llabs(newsize - oldsize);

	putchar('\n');
	if (sum.delete > 0)
		printf("Number of packages to be removed: %d\n", sum.delete);
	if (sum.install > 0)
		printf("Number of packages to be installed: %d\n", sum.install);
	if (sum.upgrade > 0)
		printf("Number of packages to be upgraded: %d\n", sum.upgrade);
	if (sum.reinstall > 0)
		printf("Number of packages to be reinstalled: %d\n",
		    sum.reinstall);
	if (sum.downgrade > 0)
		printf("Number of packages to be downgraded: %d\n",
		    sum.downgrade);
	if (sum.fetch > 0)
		printf("Number of packages to be fetched: %d\n", sum.fetch);
	if (sum.group_install > 0)
		printf("Number of groups to be installed: %d\n", sum.group_install);
	if (sum.group_upgrade > 0)
		printf("Number of groups to be upgraded: %d\n", sum.group_upgrade);
	/* Add an extra line before the size output. */
	if (bytes_change > limbytes || dlsize)
		putchar('\n');

	if (bytes_change > limbytes) {
		if (oldsize > newsize) {
			humanize_number(size, sizeof(size), oldsize - newsize, "B",
			    HN_AUTOSCALE, HN_IEC_PREFIXES);
			printf("The operation will free %s.\n", size);
		} else if (newsize > oldsize) {
			humanize_number(size, sizeof(size), newsize - oldsize, "B",
			    HN_AUTOSCALE, HN_IEC_PREFIXES);
			printf("The process will require %s more space.\n", size);
		}
	}

	if (dlsize > 0) {
		humanize_number(size, sizeof(size), dlsize, "B",
		    HN_AUTOSCALE, HN_IEC_PREFIXES);
		printf("%s to be downloaded.\n", size);
	}

	if (vuln_count > 0)
		printf("\nWARNING: %d package(s) have known vulnerabilities\n",
		    vuln_count);

	return (displayed);
}

int
print_pkg(struct pkg *p, void *ctx)
{
	int *counter = ctx;
	bool locked, vital;
	const char *lock_type;

	pkg_get(p, PKG_ATTR_LOCKED, &locked);
	pkg_get(p, PKG_ATTR_VITAL, &vital);
	if (locked) {
		if (vital)
			lock_type = "vital and locked";
		else
			lock_type = "locked";
	} else {
		if (vital)
			lock_type = "vital";
		else
			lock_type = "invalid, report an issue";
	}
	pkg_printf("\t%n (%S)\n", p, lock_type);
	(*counter)++;

	return 0;
}

void
print_repository(struct pkg_repo *repo, bool pad)
{
	const char	*mirror, *sig;

	switch (pkg_repo_mirror_type(repo)) {
		case SRV:
			mirror = "SRV";
			break;
		case HTTP:
			mirror = "HTTP";
			break;
		case NOMIRROR:
			mirror = "NONE";
			break;
		default:
			mirror = "-unknown-";
			break;
	}
	switch (pkg_repo_signature_type(repo)) {
		case SIG_PUBKEY:
			sig = "PUBKEY";
			break;
		case SIG_FINGERPRINT:
			sig = "FINGERPRINTS";
			break;
		case SIG_NONE:
			sig = "NONE";
			break;
		default:
			sig = "-unknown-";
			break;
	}

	printf("%s%s: { \n    %-16s: \"%s\",\n    %-16s: %s%s,\n"
			"    %-16s: %d",
			pad ? "  " : "",
			pkg_repo_name(repo),
			"url", pkg_repo_url(repo),
			"enabled", pkg_repo_enabled(repo) ? "yes" : "no",
			pkg_repo_overridden(repo) ? " (overridden by pkg)" : "",
			"priority", pkg_repo_priority(repo));

	if (pkg_repo_mirror_type(repo) != NOMIRROR)
		printf(",\n    %-16s: \"%s\"",
				"mirror_type", mirror);
	printf(",\n    %-16s: \"%s\"", "signature_type", sig);
	if (pkg_repo_fingerprints(repo) != NULL)
		printf(",\n    %-16s: \"%s\"",
				"fingerprints", pkg_repo_fingerprints(repo));
	if (pkg_repo_key(repo) != NULL)
		printf(",\n    %-16s: \"%s\"",
				"pubkey", pkg_repo_key(repo));
	if (pkg_repo_ssh_args(repo) != NULL)
		printf(",\n    %-16s: \"%s\"",
				"ssh_args", pkg_repo_ssh_args(repo));
	if (pkg_repo_ip_version(repo) != 0)
		printf(",\n    %-16s: %u",
				"ip_version", pkg_repo_ip_version(repo));
	printf("\n  }\n");
}

void
pkgcli_autoremove(struct pkgdb *db, bool flag)
{
	struct pkg_jobs *jobs = NULL;
	int nbactions;
	pkg_flags f = PKG_FLAG_FORCE;

	if (!flag && !pkg_object_bool(pkg_config_get("AUTOREMOVE")))
		return;
	if (dry_run)
		return;

	if (pkg_jobs_new(&jobs, PKG_JOBS_AUTOREMOVE, db) != EPKG_OK)
		return;

	pkg_jobs_set_flags(jobs, f);

	if (pkg_jobs_solve(jobs) != EPKG_OK) {
		pkg_jobs_free(jobs);
		return;
	}

	if ((nbactions = pkg_jobs_count(jobs)) == 0) {
		pkg_jobs_free(jobs);
		return;
	}

	if (!quiet) {
		print_jobs_summary(jobs,
		    "Autoremoval has been requested for the following "
		    "%d packages:\n\n", nbactions);
	}

	if (yes || query_yesno(false,
	    "\nProceed with autoremoval of packages? ")) {
		pkg_jobs_apply(jobs);
	}

	pkg_jobs_free(jobs);
	pkgdb_compact(db);
}
