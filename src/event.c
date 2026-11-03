/*-
 * SPDX-License-Identifier: LicenseRef-scancode-bsd-unchanged
 *
 * Copyright (c) 2011-2025 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011 Will Andrews <will@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 * Copyright (c) 2015 Matthew Seaman <matthew@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#include <sys/resource.h>
#include <sys/types.h>
#ifdef HAVE_LIBJAIL
#include <sys/sysctl.h>
#endif
#include <sys/wait.h>
#include <sys/socket.h>

#include <err.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#if __has_include(<libutil.h>)
#include <libutil.h>
#endif

#include <bsd_compat.h>

#include "pkg.h"
#include "pkgcli.h"
#include "xmalloc.h"

#define STALL_TIME 5

sb_t *messages  = NULL;
sb_t conflicts = sb_init();

struct cleanup {
	void *data;
	void (*cb)(void *);
};

static char *progress_message = NULL;
static sb_t msg_buf = sb_init();
static int last_progress_percent = -1;
static bool progress_started = false;
static bool progress_interrupted = false;
static bool progress_debit = false;
static int64_t last_tick = 0;
static int64_t stalled;
static int64_t bytes_per_second;
static time_t last_update;
static time_t begin = 0;
static int add_deps_depth;
static vec_t(struct cleanup *) cleanup_list = vec_init();
static bool signal_handler_installed = false;
static size_t nbactions = 0;
static size_t nbdigits = 0;
static size_t nbdone = 0;

/* units for format_size */
static const char *unit_SI[] = { " ", "k", "M", "G", "T", };

static void draw_progressbar(int64_t current, int64_t total);

/* "✚ Installing" style action verb for the fancy output. */
static void
fancy_verb(sb_t *msg, enum style_role r, enum style_glyph g, const char *verb)
{
	sb_printf(msg, "%s%s %s%s ", S(r), style_glyph(g), verb, S(ST_RESET));
}

/* A step that belongs to the action line above it, e.g. extraction. */
static void
fancy_substep(sb_t *msg, const char *verb)
{
	sb_printf(msg, "%s%s %s%s ", S(ST_DIM), style_glyph(GL_EXTRACT), verb,
	    S(ST_RESET));
}

static void
fancy_namever(sb_t *msg, const char *n, const char *v)
{
	sb_printf(msg, "%s%s%s %s%s%s", S(ST_NAME), n, S(ST_RESET),
	    S(ST_VERSION), v, S(ST_RESET));
}

/* Redraw the current progress line as "message  ━━━━━━╸━━━  42%". */
static void
fancy_bar(int percent, bool debit)
{
	int cols, msgw, pad, width, halves, i;

	cols = style_term_cols();
	msgw = style_width(progress_message);
	pad = cols / 2 < 46 ? cols / 2 : 46;
	if (msgw > pad)
		pad = msgw;
	width = cols - pad - 2 - 5 - (debit ? 32 : 0) - 1;
	if (width > 36)
		width = 36;

	printf("\r%s", progress_message);
	for (i = msgw; i < pad; i++)
		putchar(' ');
	if (width >= 8) {
		halves = percent * width * 2 / 100;
		printf("  %s", percent >= 100 ? S(ST_OK) : S(ST_BAR_DONE));
		for (i = 0; i < halves / 2; i++)
			fputs(style_glyph(GL_BAR_FULL), stdout);
		if (halves % 2) {
			fputs(style_glyph(GL_BAR_HALF), stdout);
			i++;
		}
		printf("%s", S(ST_BAR_TODO));
		for (; i < width; i++)
			fputs(style_glyph(GL_BAR_EMPTY), stdout);
		printf("%s", S(ST_RESET));
	}
	printf(" %s%3d%%%s", percent >= 100 ? S(ST_OK) : S(ST_BOLD), percent,
	    S(ST_RESET));
}

static void
cleanup_handler(int sig)
{
	static const char msg[] = "\nsignal received, cleaning up\n";
	struct cleanup *ev;

	if (cleanup_list.len == 0) {
		signal(sig, SIG_DFL);
		kill(getpid(), sig);
		_exit(128 + sig);
	}
	write(STDERR_FILENO, msg, sizeof(msg) - 1);
	vec_foreach(cleanup_list, i) {
		ev = cleanup_list.d[i];
		ev->cb(ev->data);
	}
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
	_exit(128 + sig);
}

static void
format_rate_SI(char *buf, int size, off_t bytes)
{
	int i;

	bytes *= 100;
	for (i = 0; bytes >= 100*1000 && unit_SI[i][0] != 'T'; i++)
		bytes = (bytes + 500) / 1000;
	if (i == 0) {
		i++;
		bytes = (bytes + 500) / 1000;
	}
	snprintf(buf, size, "%3lld.%1lld %sB",
	    (long long) (bytes + 5) / 100,
	    (long long) (bytes + 5) / 10 % 10,
	    unit_SI[i]);
}

void
job_status_end(sb_t *msg)
{

	printf("%s\n", msg->d);
	sb_reset(msg);
}

static int
count_digits(int n){
	if (n == 0)
		return (1);
	if (n < 0)
		n = -n;
	int c = 0;
	while (n > 0) {
		n /= 10; c++;
	}
	return (c);
}


void
job_status_begin(sb_t *msg)
{
	int n;

	sb_reset(msg);
#ifdef HAVE_LIBJAIL
	static char hostname[MAXHOSTNAMELEN] = "";
	static int jailed = -1;
	size_t intlen;

	if (jailed == -1) {
		intlen = sizeof(jailed);
		if (sysctlbyname("security.jail.jailed", &jailed, &intlen,
		    NULL, 0) == -1)
			jailed = 0;
	}

	if (jailed == 1) {
		if (hostname[0] == '\0')
			gethostname(hostname, sizeof(hostname));

		sb_printf(msg, "[%s] ", hostname);
	}
#endif

	/* Only used for pkg-add right now. */
	if (add_deps_depth) {
		if (add_deps_depth > 1) {
			for (n = 0; n < (2 * add_deps_depth); ++n) {
				if (n % 4 == 0 && n < (2 * add_deps_depth))
					sb_printf(msg, "|");
				else
					sb_printf(msg, " ");
			}
		}
		sb_printf(msg, "`-- ");
	}

	if ((nbtodl > 0 || nbactions > 0) && nbdone > 0) {
		if (nbdigits == 0)
			nbdigits = count_digits(nbtodl ? nbtodl : nbactions);
		sb_printf(msg, "%s[%*zu/%zu]%s ", S(ST_DIM), (int)nbdigits,
		    nbdone, (nbtodl) ? nbtodl : nbactions, S(ST_RESET));
	}
	if (nbtodl > 0 && nbtodl == nbdone) {
		nbtodl = 0;
		nbdone = 0;
	}
}

void
progressbar_start(const char *pmsg)
{
	if (progress_message) {
		free(progress_message);
		progress_message = NULL;
	}

	if (quiet)
		return;
	if (pmsg != NULL)
		progress_message = xstrdup(pmsg);
	else {

		progress_message = xstrdup(sb_str(&msg_buf));
	}
	last_progress_percent = -1;
	last_tick = 0;
	begin = last_update = time(NULL);
	bytes_per_second = 0;
	stalled = 0;

	progress_started = true;
	progress_interrupted = false;
	if (!isatty(STDOUT_FILENO))
		printf("%s: ", progress_message);
	else if (style_fancy()) {
		fancy_bar(0, progress_debit);
		fflush(stdout);
	} else
		printf("%s:   0%%", progress_message);
}

void
progressbar_tick(int64_t current, int64_t total)
{
	int percent;

	if (!quiet && progress_started) {
		if (isatty(STDOUT_FILENO))
			draw_progressbar(current, total);
		else {
			if (progress_interrupted) {
				printf("%s...", progress_message);
			} else if (!getenv("NO_TICK")){
				percent = (total != 0) ? (current * 100. / total) : 100;
				if (last_progress_percent / 10 < percent / 10) {
					last_progress_percent = percent;
					putchar('.');
					fflush(stdout);
				}
			}
			if (current >= total)
				progressbar_stop();
		}
	}
	progress_interrupted = false;
}

void
progressbar_stop(void)
{
	if (progress_started) {
		if (!isatty(STDOUT_FILENO))
			printf(" done");
		putchar('\n');
	}
	last_progress_percent = -1;
	progress_started = false;
	progress_interrupted = false;
}

static void
draw_progressbar(int64_t current, int64_t total)
{
	int percent;
	int64_t transferred;
	time_t elapsed = 0, now = 0;
	char buf[9];
	int64_t bytes_left;
	int cur_speed;
	int hours, minutes, seconds;
	float age_factor;

	if (!progress_started) {
		progressbar_stop();
		return;
	}

	if (progress_debit) {
		now = time(NULL);
		elapsed = (now >= last_update) ? now - last_update : 0;
	}

	percent = (total != 0) ? (current * 100. / total) : 100;

	/**
	 * Wait for interval for debit bars to keep calc per second.
	 * If not debit, show on every % change, or if ticking after
	 * an interruption (which removed our progressbar output).
	 */
	if (current >= total || (progress_debit && elapsed >= 1) ||
	    (!progress_debit &&
	    (percent != last_progress_percent || progress_interrupted))) {
		last_progress_percent = percent;

		if (style_fancy())
			fancy_bar(percent, progress_debit);
		else
			printf("\r%s: %3d%%", progress_message, percent);
		if (progress_debit && style_fancy())
			printf("%s", S(ST_DIM));
		if (progress_debit) {
			transferred = current - last_tick;
			last_tick = current;
			bytes_left = total - current;
			if (bytes_left <= 0) {
				elapsed = now - begin;
				/* Always show at least 1 second at end. */
				if (elapsed == 0)
					elapsed = 1;
				/* Calculate true total speed when done */
				transferred = total;
				bytes_per_second = 0;
			}

			if (elapsed != 0)
				cur_speed = (transferred / elapsed);
			else
				cur_speed = transferred;

#define AGE_FACTOR_SLOW_START 3
			if (now - begin <= AGE_FACTOR_SLOW_START)
				age_factor = 0.4;
			else
				age_factor = 0.9;

			if (bytes_per_second != 0) {
				bytes_per_second =
				    (bytes_per_second * age_factor) +
				    (cur_speed * (1.0 - age_factor));
			} else
				bytes_per_second = cur_speed;

			humanize_number(buf, sizeof(buf),
			    current,"B", HN_AUTOSCALE, HN_IEC_PREFIXES);
			printf(" %*s", (int)sizeof(buf), buf);

			if (bytes_left > 0)
				format_rate_SI(buf, sizeof(buf), transferred);
			else /* Show overall speed when done */
				format_rate_SI(buf, sizeof(buf),
				    bytes_per_second);
			printf(" %s/s ", buf);

			if (!transferred)
				stalled += elapsed;
			else
				stalled = 0;

			if (stalled >= STALL_TIME)
				printf(" - stalled -");
			else if (bytes_per_second == 0 && bytes_left > 0)
				printf("   --:-- ETA");
			else {
				if (bytes_left > 0)
					seconds = bytes_left / bytes_per_second;
				else
					seconds = elapsed;

				hours = seconds / 3600;
				seconds -= hours * 3600;
				minutes = seconds / 60;
				seconds -= minutes * 60;

				if (hours != 0)
					printf("%02d:%02d:%02d", hours,
					    minutes, seconds);
				else
					printf("   %02d:%02d", minutes, seconds);

				if (bytes_left > 0)
					printf(" ETA");
				else
					printf("    ");
			}
			last_update = now;
		}
		if (style_fancy())
			printf("%s\033[K", S(ST_RESET));
		fflush(stdout);
	}
	if (current >= total)
		progressbar_stop();
}

static const char *
str_or_unknown(const char *str)
{
	if (str == NULL || str[0] == '\0')
		return "???";
	return str;
}

typedef int (*event_handler_fn)(struct pkg_event *ev, int *debug);

static int
event_cb_errno(struct pkg_event *ev, int *debug __unused)
{
	if (style_fancy_stderr())
		fprintf(stderr, "%s%s%s %s%s:%s %s(%s): %s\n", SE(ST_ERROR),
		    style_glyph(GL_ERROR), SE(ST_RESET), SE(ST_DIM),
		    getprogname(), SE(ST_RESET), ev->e_errno.func,
		    ev->e_errno.arg, strerror(ev->e_errno.no));
	else
		warnx("%s(%s): %s", ev->e_errno.func, ev->e_errno.arg,
		    strerror(ev->e_errno.no));
	return (0);
}

static int
event_cb_error(struct pkg_event *ev, int *debug __unused)
{
	if (style_fancy_stderr())
		fprintf(stderr, "%s%s%s %s%s:%s %s\n", SE(ST_ERROR),
		    style_glyph(GL_ERROR), SE(ST_RESET), SE(ST_DIM),
		    getprogname(), SE(ST_RESET), ev->e_pkg_error.msg);
	else
		warnx("%s", ev->e_pkg_error.msg);
	return (0);
}

static int
event_cb_notice(struct pkg_event *ev, int *debug __unused)
{
	if (!quiet)
		printf("%s\n", ev->e_pkg_notice.msg);
	return (0);
}

static int
event_cb_developer_mode(struct pkg_event *ev, int *debug __unused)
{
	warnx("DEVELOPER_MODE: %s", ev->e_pkg_error.msg);
	return (0);
}

static int
event_cb_update_add(struct pkg_event *ev, int *debug __unused)
{
	if (quiet || !isatty(STDOUT_FILENO))
		return (0);
	printf("\rPushing new entries %d/%d", ev->e_upd_add.done, ev->e_upd_add.total);
	if (ev->e_upd_add.total == ev->e_upd_add.done)
	        putchar('\n');
	return (0);
}

static int
event_cb_update_remove(struct pkg_event *ev, int *debug __unused)
{
	if (quiet || !isatty(STDOUT_FILENO))
		return (0);
	printf("\rRemoving entries %d/%d", ev->e_upd_remove.done, ev->e_upd_remove.total);
	if (ev->e_upd_remove.total == ev->e_upd_remove.done)
		putchar('\n');
	return (0);
}

static int
event_cb_fetch_begin(struct pkg_event *ev, int *debug __unused)
{
	const char *filename, *tmp;

	if (nbtodl > 0)
		nbdone++;
	if (quiet)
		return (0);
	filename = strrchr(ev->e_fetching.url, '/');
	if (filename != NULL) {
		filename++;
	} else {
		/*
		 * We failed at being smart, so display
		 * the entire url.
		 */
		filename = ev->e_fetching.url;
	}
	job_status_begin(&msg_buf);
	progress_debit = true;
	tmp = strrchr(filename, '~');
	if (style_fancy()) {
		int len = strlen(filename);

		if (tmp != NULL)
			len = tmp - filename;
		else if ((tmp = strrchr(filename, '.')) != NULL &&
		    strcmp(tmp, ".pkg") == 0)
			len = tmp - filename;
		fancy_verb(&msg_buf, ST_FETCH, GL_FETCH, "Fetching");
		sb_printf(&msg_buf, "%s%.*s%s", S(ST_NAME), len, filename,
		    S(ST_RESET));
	} else if (tmp != NULL)
		sb_printf(&msg_buf, "Fetching %.*s",
				(int)(tmp - filename), filename);
	else {
		tmp = strrchr(filename, '.');
		if (tmp != NULL && strcmp(tmp, ".pkg") == 0)
			sb_printf(&msg_buf, "Fetching %.*s",
					(int)(tmp - filename), filename);
		else
			sb_printf(&msg_buf, "Fetching %s",
					filename);
	}
	return (0);
}

static int
event_cb_fetch_finished(struct pkg_event *ev __unused, int *debug __unused)
{
	progress_debit = false;
	return (0);
}

static int
event_cb_install_begin(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	const char *n, *v;

	if (quiet)
		return (0);
	job_status_begin(&msg_buf);

	pkg = ev->e_install_begin.pkg;
	pkg_get(pkg, PKG_ATTR_NAME, &n);
	pkg_get(pkg, PKG_ATTR_VERSION, &v);

	if (style_fancy()) {
		fancy_verb(&msg_buf, ST_INSTALL, GL_INSTALL, "Installing");
		fancy_namever(&msg_buf, n, v);
		sb_cat_c(&msg_buf, '\n');
	} else
		sb_printf(&msg_buf, "Installing %s-%s...\n", n, v);

	printf("%s",sb_str(&msg_buf));
	return (0);
}

static int
event_cb_extract_begin(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	const char *n, *v;

	if (quiet)
		return (0);
	job_status_begin(&msg_buf);
	pkg = ev->e_install_begin.pkg;
	pkg_get(pkg, PKG_ATTR_NAME, &n);
	pkg_get(pkg, PKG_ATTR_VERSION, &v);
	if (style_fancy()) {
		fancy_substep(&msg_buf, "Extracting");
		fancy_namever(&msg_buf, n, v);
	} else
		sb_printf(&msg_buf, "Extracting %s-%s", n, v);

	return (0);
}

static int
event_cb_add_deps_begin(struct pkg_event *ev __unused, int *debug __unused)
{
	++add_deps_depth;
	return (0);
}

static int
event_cb_add_deps_finished(struct pkg_event *ev __unused, int *debug __unused)
{
	--add_deps_depth;
	return (0);
}

static int
event_cb_integritycheck_begin(struct pkg_event *ev __unused, int *debug __unused)
{
	if (quiet)
		return (0);
	printf("Checking integrity...");
	return (0);
}

static int
event_cb_integritycheck_finished(struct pkg_event *ev, int *debug __unused)
{
	if (quiet)
		return (0);
	if (style_fancy() && ev->e_integrity_finished.conflicting == 0)
		printf(" %s%s%s %sno conflicts%s\n", S(ST_OK),
		    style_glyph(GL_OK), S(ST_RESET), S(ST_DIM), S(ST_RESET));
	else if (style_fancy())
		printf(" %s%s %d conflicting%s\n", S(ST_WARN),
		    style_glyph(GL_WARN), ev->e_integrity_finished.conflicting,
		    S(ST_RESET));
	else
		printf(" done (%d conflicting)\n",
		    ev->e_integrity_finished.conflicting);
	if (conflicts.len != 0) {
		char *c = sb_get(&conflicts);
		printf("%s", c);
		free(c);
	}
	return (0);
}

static int
event_cb_integritycheck_conflict(struct pkg_event *ev, int *debug)
{
	struct pkg_event_conflict *cur_conflict;

	if (*debug == 0)
		return (0);
	printf("\nConflict found on path %s between %s and ",
	    ev->e_integrity_conflict.pkg_path,
	    ev->e_integrity_conflict.pkg_uid);
	cur_conflict = ev->e_integrity_conflict.conflicts;
	while (cur_conflict) {
		if (cur_conflict->next)
			printf("%s, ", cur_conflict->uid);
		else
			printf("%s", cur_conflict->uid);

		cur_conflict = cur_conflict->next;
	}
	printf("\n");
	return (0);
}

static int
event_cb_deinstall_begin(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	const char *n, *v;

	if (quiet)
		return (0);

	job_status_begin(&msg_buf);

	pkg = ev->e_install_begin.pkg;
	pkg_get(pkg, PKG_ATTR_NAME, &n);
	pkg_get(pkg, PKG_ATTR_VERSION, &v);
	if (style_fancy()) {
		fancy_verb(&msg_buf, ST_REMOVE, GL_REMOVE, "Deinstalling");
		fancy_namever(&msg_buf, n, v);
		sb_cat_c(&msg_buf, '\n');
	} else
		sb_printf(&msg_buf, "Deinstalling %s-%s...\n", n, v);

	printf("%s", sb_str(&msg_buf));
	return (0);
}

static int
event_cb_delete_files_begin(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	const char *n, *v;

	if (quiet)
		return (0);
	job_status_begin(&msg_buf);
	pkg = ev->e_install_begin.pkg;
	pkg_get(pkg, PKG_ATTR_NAME, &n);
	pkg_get(pkg, PKG_ATTR_VERSION, &v);
	if (style_fancy()) {
		fancy_substep(&msg_buf, "Deleting files for");
		fancy_namever(&msg_buf, n, v);
	} else
		sb_printf(&msg_buf, "Deleting files for %s-%s", n, v);
	return (0);
}

static int
event_cb_upgrade_begin(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg_new, *pkg_old;
	const char *n, *ov, *v;

	if (quiet)
		return (0);
	pkg_new = ev->e_upgrade_begin.n;
	pkg_old = ev->e_upgrade_begin.o;

	job_status_begin(&msg_buf);

	pkg_get(pkg_new, PKG_ATTR_NAME, &n);
	pkg_get(pkg_new, PKG_ATTR_VERSION, &v);
	pkg_get(pkg_old, PKG_ATTR_VERSION, &ov);
	if (style_fancy()) {
		switch (pkg_version_change_between(pkg_new, pkg_old)) {
		case PKG_DOWNGRADE:
			fancy_verb(&msg_buf, ST_DOWNGRADE, GL_DOWNGRADE,
			    "Downgrading");
			sb_printf(&msg_buf, "%s%s%s ", S(ST_NAME), n,
			    S(ST_RESET));
			style_version_pair(&msg_buf, ov, v, ST_VER_DOWN_OLD,
			    ST_VER_DOWN_NEW, 0);
			break;
		case PKG_REINSTALL:
			fancy_verb(&msg_buf, ST_REINSTALL, GL_REINSTALL,
			    "Reinstalling");
			fancy_namever(&msg_buf, n, v);
			break;
		case PKG_UPGRADE:
			fancy_verb(&msg_buf, ST_UPGRADE, GL_UPGRADE,
			    "Upgrading");
			sb_printf(&msg_buf, "%s%s%s ", S(ST_NAME), n,
			    S(ST_RESET));
			style_version_pair(&msg_buf, ov, v, ST_VER_UP_OLD,
			    ST_VER_UP_NEW, 0);
			break;
		}
		sb_cat_c(&msg_buf, '\n');
	} else switch (pkg_version_change_between(pkg_new, pkg_old)) {
	case PKG_DOWNGRADE:
		sb_printf(&msg_buf, "Downgrading %s from %s to %s...\n",
		    n, ov, v);
		break;
	case PKG_REINSTALL:
		sb_printf(&msg_buf, "Reinstalling %s-%s...\n", n, v);
		break;
	case PKG_UPGRADE:
		sb_printf(&msg_buf, "Upgrading %s from %s to %s...\n",
		    n, ov, v);
		break;
	}

	printf("%s", sb_str(&msg_buf));
	return (0);
}

static int
event_cb_locked(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;

	pkg = ev->e_locked.pkg;
	pkg_printf("\n%n-%v is locked and may not be modified\n", pkg, pkg);
	return (0);
}

static int
event_cb_required(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;

	pkg = ev->e_required.pkg;
	pkg_printf("\n%n-%v is required by: %r%{%rn-%rv%| %}", pkg, pkg, pkg);
	if (ev->e_required.force == 1)
		fprintf(stderr, ", deleting anyway\n");
	else
		fprintf(stderr, "\n");
	return (0);
}

static int
event_cb_already_installed(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;

	if (quiet)
		return (0);
	pkg = ev->e_already_installed.pkg;
	pkg_printf("the most recent version of %n-%v is already installed\n",
			pkg, pkg);
	return (0);
}

static int
event_cb_not_found(struct pkg_event *ev, int *debug __unused)
{
	printf("Package '%s' was not found in "
	    "the repositories\n", ev->e_not_found.pkg_name);
	return (0);
}

static int
event_cb_missing_dep(struct pkg_event *ev, int *debug __unused)
{
	warnx("Missing dependency '%s'",
	    pkg_dep_name(ev->e_missing_dep.dep));
	return (0);
}

static int
event_cb_noremotedb(struct pkg_event *ev, int *debug __unused)
{
	fprintf(stderr, "Unable to open remote database \"%s\". "
	    "Try running '%s update' first.\n", ev->e_remotedb.repo,
	    getprogname());
	return (0);
}

static int
event_cb_nolocaldb(struct pkg_event *ev __unused, int *debug __unused)
{
	fprintf(stderr, "Local package database nonexistent!\n");
	return (0);
}

static int
event_cb_newpkgversion(struct pkg_event *ev __unused, int *debug __unused)
{
	newpkgversion = true;
	printf("New version of pkg detected; it needs to be "
	    "installed first.\n");
	return (0);
}

static int
event_cb_file_mismatch(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;

	pkg = ev->e_file_mismatch.pkg;
	pkg_fprintf(stderr, "%n-%v: checksum mismatch for %Fn\n", pkg,
	    pkg, ev->e_file_mismatch.file);
	return (0);
}

static int
event_cb_file_missing(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;

	pkg = ev->e_file_missing.pkg;
	pkg_fprintf(stderr, "%n-%v: missing file %Fn\n", pkg, pkg,
	    ev->e_file_missing.file);
	return (0);
}

static int
event_cb_dir_meta_mismatch(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	struct pkg_dir *dir;

	pkg = ev->e_file_meta_mismatch.pkg;
	dir = ev->e_dir_meta_mismatch.dir;
	pkg_fprintf(stderr, "%n-%v: %Dn [%S] %S -> %S\n", pkg, pkg, dir,
		    pkg_meta_attribute_tostring(ev->e_dir_meta_mismatch.attrib),
		    str_or_unknown(ev->e_dir_meta_mismatch.db_val),
		    str_or_unknown(ev->e_dir_meta_mismatch.fs_val));
	return (0);
}

static int
event_cb_file_meta_mismatch(struct pkg_event *ev, int *debug __unused)
{
	struct pkg *pkg;
	struct pkg_file *file;

	pkg = ev->e_file_meta_mismatch.pkg;
	file = ev->e_file_meta_mismatch.file;
	pkg_fprintf(stderr, "%n-%v: %Fn [%S] %S -> %S\n", pkg, pkg, file,
		    pkg_meta_attribute_tostring(ev->e_file_meta_mismatch.attrib),
		    str_or_unknown(ev->e_file_meta_mismatch.db_val),
		    str_or_unknown(ev->e_file_meta_mismatch.fs_val));
	return (0);
}

static int
event_cb_plugin_errno(struct pkg_event *ev, int *debug __unused)
{
	warnx("%s: %s(%s): %s",
	    pkg_plugin_get(ev->e_plugin_errno.plugin, PKG_PLUGIN_NAME),
	    ev->e_plugin_errno.func, ev->e_plugin_errno.arg,
	    strerror(ev->e_plugin_errno.no));
	return (0);
}

static int
event_cb_plugin_error(struct pkg_event *ev, int *debug __unused)
{
	warnx("%s: %s",
	    pkg_plugin_get(ev->e_plugin_error.plugin, PKG_PLUGIN_NAME),
	    ev->e_plugin_error.msg);
	return (0);
}

static int
event_cb_plugin_info(struct pkg_event *ev, int *debug __unused)
{
	if (quiet)
		return (0);
	printf("%s: %s\n",
	    pkg_plugin_get(ev->e_plugin_info.plugin, PKG_PLUGIN_NAME),
	    ev->e_plugin_info.msg);
	return (0);
}

static int
event_cb_incremental_update(struct pkg_event *ev, int *debug __unused)
{
	if (!quiet && style_fancy())
		printf("%s%s%s %s%s%s repository update completed. %s%d%s "
		    "packages processed.\n", S(ST_OK), style_glyph(GL_OK),
		    S(ST_RESET), S(ST_REPO), ev->e_incremental_update.reponame,
		    S(ST_RESET), S(ST_BOLD), ev->e_incremental_update.processed,
		    S(ST_RESET));
	else if (!quiet)
		printf("%s repository update completed. %d packages processed.\n",
		    ev->e_incremental_update.reponame,
		    ev->e_incremental_update.processed);
	return (0);
}

static int
event_cb_debug(struct pkg_event *ev, int *debug __unused)
{
	fprintf(stderr, "DBG(%d)[%d]> %s\n", ev->e_debug.level,
		(int)getpid(), ev->e_debug.msg);
	return (0);
}

static int
event_cb_query_yesno(struct pkg_event *ev, int *debug __unused)
{
	return (ev->e_query_yesno.deft ?
		query_yesno(true, ev->e_query_yesno.msg, "[Y/n]") :
		query_yesno(false, ev->e_query_yesno.msg, "[y/N]"));
}

static int
event_cb_query_select(struct pkg_event *ev, int *debug __unused)
{
	return query_select(ev->e_query_select.msg, ev->e_query_select.items,
		ev->e_query_select.ncnt, ev->e_query_select.deft);
}

static int
event_cb_sandbox_call(struct pkg_event *ev, int *debug __unused)
{
	return (pkg_handle_sandboxed_call(ev->e_sandbox_call.call,
			ev->e_sandbox_call.fd,
			ev->e_sandbox_call.userdata));
}

static int
event_cb_sandbox_get_string(struct pkg_event *ev, int *debug __unused)
{
	return (pkg_handle_sandboxed_get_string(ev->e_sandbox_call_str.call,
			ev->e_sandbox_call_str.result,
			ev->e_sandbox_call_str.len,
			ev->e_sandbox_call_str.userdata));
}

static int
event_cb_progress_start(struct pkg_event *ev, int *debug __unused)
{
	progressbar_start(ev->e_progress_start.msg);
	return (0);
}

static int
event_cb_progress_tick(struct pkg_event *ev, int *debug __unused)
{
	progressbar_tick(ev->e_progress_tick.current,
	    ev->e_progress_tick.total);
	return (0);
}

static int
event_cb_backup(struct pkg_event *ev __unused, int *debug __unused)
{
	sb_printf(&msg_buf, "Backing up");
	return (0);
}

static int
event_cb_restore(struct pkg_event *ev __unused, int *debug __unused)
{
	sb_printf(&msg_buf, "Restoring");
	return (0);
}

static int
event_cb_new_action(struct pkg_event *ev, int *debug __unused)
{
	nbactions = ev->e_action.total;
	nbdigits = 0;
	nbdone = ev->e_action.current;
	return (0);
}

/* Each pkg-message on its own, so print_pkg_messages() can frame them. */
static vec_t(char *) fancy_messages = vec_init();

static int
event_cb_message(struct pkg_event *ev, int *debug __unused)
{
	static sb_t msgs = sb_init();
	if (messages == NULL)
		messages = &msgs;
	sb_cat(messages, ev->e_pkg_message.msg);
	if (style_fancy())
		vec_push(&fancy_messages, xstrdup(ev->e_pkg_message.msg));
	return (0);
}

/* One "│  text" line per line of s, up to (not including) end. */
static void
print_framed_lines(const char *s, const char *end, const char *frame)
{
	const char *nl;

	while (s < end) {
		nl = memchr(s, '\n', end - s);
		if (nl == NULL)
			nl = end;
		printf("%s%s%s", frame, style_glyph(GL_BOX_V), S(ST_RESET));
		if (nl > s)
			printf("  %.*s", (int)(nl - s), s);
		putchar('\n');
		s = nl + 1;
	}
}

/*
 * Render one message as a card.  libpkg formats them as
 *   "=====\nMessage from NAME-VERSION:\n\n--\ntext\n--\ntext\n" (install)
 *   "Message from NAME-VERSION:\ntext\n"                         (removal)
 * Anything else, such as script output, is printed as-is.  The text itself
 * is never rewrapped so commands and URLs in it stay intact.
 */
static void
print_fancy_message(const char *msg)
{
	const char *p = msg, *title, *title_end, *dash, *end, *sec, *next;
	const char *sec_end, *frame;
	bool removal, first = true;

	removal = strncmp(p, "=====\n", 6) != 0;
	if (!removal)
		p += 6;
	if (strncmp(p, "Message from ", 13) != 0 ||
	    (title_end = strstr(p, ":\n")) == NULL) {
		printf("%s", msg);
		return;
	}
	title = p + 13;
	p = title_end + 2;
	if (*p == '\n')
		p++;
	frame = removal ? S(ST_DIM) : S(ST_ACCENT);

	/* Header: ╭─ ✉ Message from curl-8.11.0 */
	printf("\n%s%s%s%s ", frame, style_glyph(GL_BOX_TL),
	    style_glyph(GL_BOX_H), S(ST_RESET));
	printf("%s%s%s %sMessage from%s ", removal ? S(ST_REMOVE) : S(ST_ACCENT),
	    style_glyph(GL_MAIL), S(ST_RESET), S(ST_BOLD), S(ST_RESET));
	dash = title_end;
	while (dash > title && *dash != '-')
		dash--;
	if (dash > title)
		printf("%s%.*s%s%s-%s%s%.*s%s", S(ST_NAME), (int)(dash - title),
		    title, S(ST_RESET), S(ST_DIM), S(ST_RESET), S(ST_VERSION),
		    (int)(title_end - dash - 1), dash + 1, S(ST_RESET));
	else
		printf("%s%.*s%s", S(ST_NAME), (int)(title_end - title), title,
		    S(ST_RESET));
	if (removal)
		printf(" %s(on removal)%s", S(ST_DIM), S(ST_RESET));
	putchar('\n');

	/* Body: sections separated by "--" lines, empty ones skipped. */
	end = p + strlen(p);
	while (end > p && end[-1] == '\n')
		end--;
	for (sec = p; sec < end; sec = next) {
		if (strncmp(sec, "--\n", 3) == 0)
			sec += 3;
		next = strstr(sec, "\n--\n");
		if (next == NULL || next >= end)
			next = end;
		else
			next++;
		sec_end = next;
		while (sec_end > sec && sec_end[-1] == '\n')
			sec_end--;
		if (sec_end <= sec)
			continue;
		if (!first) {
			printf("%s%s", frame, style_glyph(GL_BOX_SEP));
			for (int i = 0; i < 3; i++)
				fputs(style_glyph(GL_BOX_DASH), stdout);
			printf("%s\n", S(ST_RESET));
		}
		first = false;
		print_framed_lines(sec, sec_end, frame);
	}
	printf("%s%s%s%s\n", frame, style_glyph(GL_BOX_BL),
	    style_glyph(GL_BOX_H), S(ST_RESET));
}

void
print_pkg_messages(void)
{
	if (messages == NULL || messages->len == 0)
		return;
	if (!style_fancy() || fancy_messages.len == 0) {
		printf("%s", sb_str(messages));
		return;
	}
	vec_foreach(fancy_messages, i)
		print_fancy_message(fancy_messages.d[i]);
	vec_free_and_free(&fancy_messages, free);
}

static int
event_cb_cleanup_callback_register(struct pkg_event *ev, int *debug __unused)
{
	struct cleanup *evtmp;

	if (!signal_handler_installed) {
		signal(SIGINT, cleanup_handler);
		signal_handler_installed = true;
	}
	evtmp = xmalloc(sizeof(struct cleanup));
	evtmp->cb = ev->e_cleanup_callback.cleanup_cb;
	evtmp->data = ev->e_cleanup_callback.data;
	vec_push(&cleanup_list, evtmp);
	return (0);
}

static int
event_cb_cleanup_callback_unregister(struct pkg_event *ev, int *debug __unused)
{
	struct cleanup *evtmp;

	if (!signal_handler_installed)
		return (0);
	vec_foreach(cleanup_list, i) {
		evtmp = cleanup_list.d[i];
		if (evtmp->cb == ev->e_cleanup_callback.cleanup_cb &&
		    evtmp->data == ev->e_cleanup_callback.data) {
			vec_remove_and_free(&cleanup_list, i, free);
			break;
		}
	}
	return (0);
}

static int
event_cb_conflicts(struct pkg_event *ev, int *debug __unused)
{
	const char *reponame = NULL;
	const char *n, *v;
	pkg_get(ev->e_conflicts.p1, PKG_ATTR_NAME, &n);
	pkg_get(ev->e_conflicts.p1, PKG_ATTR_VERSION, &v);

	sb_printf(&conflicts, "  - %s-%s", n, v);
	if (pkg_repos_total_count() > 1) {
		pkg_get(ev->e_conflicts.p1, PKG_ATTR_REPONAME, &reponame);
		sb_printf(&conflicts, " [%s]",
		    reponame == NULL ? "installed" : reponame);
	}
	pkg_get(ev->e_conflicts.p2, PKG_ATTR_NAME, &n);
	pkg_get(ev->e_conflicts.p2, PKG_ATTR_VERSION, &v);
	sb_printf(&conflicts, " conflicts with %s-%s", n, v);
	if (pkg_repos_total_count() > 1) {
		pkg_get(ev->e_conflicts.p2, PKG_ATTR_REPONAME, &reponame);
		sb_printf(&conflicts, " [%s]",
		    reponame == NULL ? "installed" : reponame);
	}
	sb_printf(&conflicts, " on %s\n",
	    ev->e_conflicts.path);
	return (0);
}

static int
event_cb_trigger(struct pkg_event *ev, int *debug __unused)
{
	if (!quiet) {
		if (ev->e_trigger.cleanup)
			printf("==> Cleaning up trigger: %s\n", ev->e_trigger.name);
		else
			printf("==> Running trigger: %s\n", ev->e_trigger.name);
	}
	return (0);
}

static int
event_cb_rc_script(struct pkg_event *ev, int *debug __unused)
{
	if (!quiet) {
		switch (ev->e_rc_script.action) {
		case PKG_RC_START:
			printf("Starting %s\n", ev->e_rc_script.name);
			break;
		case PKG_RC_STOP:
			printf("Stopping %s\n", ev->e_rc_script.name);
			break;
		case PKG_RC_RESTART:
			printf("Restarting %s\n", ev->e_rc_script.name);
			break;
		default:
			printf("Restarting %s\n", ev->e_rc_script.name);
			break;
		}
	}
	return (0);
}

static const event_handler_fn event_handlers[PKG_EVENT_LAST] = {
	[PKG_EVENT_INSTALL_BEGIN]                = event_cb_install_begin,
	[PKG_EVENT_DEINSTALL_BEGIN]              = event_cb_deinstall_begin,
	[PKG_EVENT_UPGRADE_BEGIN]                = event_cb_upgrade_begin,
	[PKG_EVENT_EXTRACT_BEGIN]                = event_cb_extract_begin,
	[PKG_EVENT_DELETE_FILES_BEGIN]            = event_cb_delete_files_begin,
	[PKG_EVENT_ADD_DEPS_BEGIN]               = event_cb_add_deps_begin,
	[PKG_EVENT_ADD_DEPS_FINISHED]            = event_cb_add_deps_finished,
	[PKG_EVENT_FETCH_BEGIN]                  = event_cb_fetch_begin,
	[PKG_EVENT_FETCH_FINISHED]               = event_cb_fetch_finished,
	[PKG_EVENT_UPDATE_ADD]                   = event_cb_update_add,
	[PKG_EVENT_UPDATE_REMOVE]                = event_cb_update_remove,
	[PKG_EVENT_INTEGRITYCHECK_BEGIN]         = event_cb_integritycheck_begin,
	[PKG_EVENT_INTEGRITYCHECK_FINISHED]      = event_cb_integritycheck_finished,
	[PKG_EVENT_INTEGRITYCHECK_CONFLICT]      = event_cb_integritycheck_conflict,
	[PKG_EVENT_NEWPKGVERSION]                = event_cb_newpkgversion,
	[PKG_EVENT_NOTICE]                       = event_cb_notice,
	[PKG_EVENT_DEBUG]                        = event_cb_debug,
	[PKG_EVENT_INCREMENTAL_UPDATE]           = event_cb_incremental_update,
	[PKG_EVENT_QUERY_YESNO]                  = event_cb_query_yesno,
	[PKG_EVENT_QUERY_SELECT]                 = event_cb_query_select,
	[PKG_EVENT_SANDBOX_CALL]                 = event_cb_sandbox_call,
	[PKG_EVENT_SANDBOX_GET_STRING]           = event_cb_sandbox_get_string,
	[PKG_EVENT_PROGRESS_START]               = event_cb_progress_start,
	[PKG_EVENT_PROGRESS_TICK]                = event_cb_progress_tick,
	[PKG_EVENT_BACKUP]                       = event_cb_backup,
	[PKG_EVENT_RESTORE]                      = event_cb_restore,
	[PKG_EVENT_ERROR]                        = event_cb_error,
	[PKG_EVENT_ERRNO]                        = event_cb_errno,
	[PKG_EVENT_ALREADY_INSTALLED]            = event_cb_already_installed,
	[PKG_EVENT_LOCKED]                       = event_cb_locked,
	[PKG_EVENT_REQUIRED]                     = event_cb_required,
	[PKG_EVENT_MISSING_DEP]                  = event_cb_missing_dep,
	[PKG_EVENT_NOREMOTEDB]                   = event_cb_noremotedb,
	[PKG_EVENT_NOLOCALDB]                    = event_cb_nolocaldb,
	[PKG_EVENT_FILE_MISMATCH]                = event_cb_file_mismatch,
	[PKG_EVENT_DEVELOPER_MODE]               = event_cb_developer_mode,
	[PKG_EVENT_PLUGIN_ERRNO]                 = event_cb_plugin_errno,
	[PKG_EVENT_PLUGIN_ERROR]                 = event_cb_plugin_error,
	[PKG_EVENT_PLUGIN_INFO]                  = event_cb_plugin_info,
	[PKG_EVENT_NOT_FOUND]                    = event_cb_not_found,
	[PKG_EVENT_NEW_ACTION]                   = event_cb_new_action,
	[PKG_EVENT_MESSAGE]                      = event_cb_message,
	[PKG_EVENT_FILE_MISSING]                 = event_cb_file_missing,
	[PKG_EVENT_CLEANUP_CALLBACK_REGISTER]    = event_cb_cleanup_callback_register,
	[PKG_EVENT_CLEANUP_CALLBACK_UNREGISTER]  = event_cb_cleanup_callback_unregister,
	[PKG_EVENT_CONFLICTS]                    = event_cb_conflicts,
	[PKG_EVENT_TRIGGER]                      = event_cb_trigger,
	[PKG_EVENT_FILE_META_MISMATCH]           = event_cb_file_meta_mismatch,
	[PKG_EVENT_DIR_META_MISMATCH]            = event_cb_dir_meta_mismatch,
	[PKG_EVENT_RC_SCRIPT]                    = event_cb_rc_script,
};
_Static_assert(NELEM(event_handlers) == PKG_EVENT_LAST,
    "event_handlers table size does not match pkg_event_t enum");

int
event_callback(void *data, struct pkg_event *ev)
{
	int *debug = data;

	/* Interrupt progressbar for most event types */
	if (progress_started && ev->type != PKG_EVENT_PROGRESS_TICK &&
	    ev->type != PKG_EVENT_FILE_META_OK &&
	    ev->type != PKG_EVENT_DIR_META_OK &&
	    !progress_interrupted) {
		putchar('\n');
		progress_interrupted = true;
	}

	if (ev->type < PKG_EVENT_LAST && event_handlers[ev->type] != NULL)
		return (event_handlers[ev->type](ev, debug));

	return (0);
}
