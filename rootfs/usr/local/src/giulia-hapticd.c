// SPDX-License-Identifier: GPL-2.0-only
/*
 * giulia-hapticd - user-space haptics daemon for OnePlus 13R / Ace 5 (giulia)
 *
 * Watches the input devices that are available to the AP on mainline:
 *   - pmic_pwrkey      KEY_POWER
 *   - pmic_resin       KEY_VOLUMEUP
 *   - gpio-keys        KEY_VOLUMEDOWN
 *   - ak09970-slider   EV_ABS ABS_X  (0 = up, 1 = mid, 2 = down)
 *
 * plus desktop events:
 *   - a session D-Bus notification (org.freedesktop.Notifications Notify)
 *
 * and plays a stock Android haptics waveform through qcom-haptics-play,
 * chosen from a small config file (see giulia-hapticd.conf).
 *
 * It also listens on a FIFO so other tools (scripts, compositor plugins)
 * can trigger a named event.
 *
 * Event names used in the config:
 *   power, volume_up, volume_down, slider_up, slider_mid, slider_down,
 *   notification, ui_desktop, ui_showdesktop
 * Any event whose effect id is 0 is ignored.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_DEVS	16
#define MAX_EVENTS	16
#define MAX_WATCHES	8
#define NAME_LEN	64
#define STR_LEN		128

struct event_map {
	char	name[STR_LEN];
	int	effect;		/* 0 = disabled */
};

struct dev {
	struct dev	*next;
	int		fd;
	char		name[NAME_LEN];	/* evdev name, for the slider match */
	int		is_slider;
};

/*
 * Desktop events are watched by running one dbus-monitor per entry and
 * playing "event" when the matching message contains "needle".
 */
struct dbus_watch {
	const char	*rule;
	const char	*needle;
	const char	*event;
	int		fd;
	long		last_try;
	char		buf[1024];
	size_t		len;
};

static struct dbus_watch watches[] = {
	{ "interface='org.freedesktop.Notifications',member='Notify'",
	  "member=Notify", "notification", -1, 0, { 0 }, 0 },
	{ "type='signal',path='/VirtualDesktopManager',"
	  "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
	  "current", "ui_desktop", -1, 0, { 0 }, 0 },
	{ "type='signal',sender='org.kde.KWin',path='/KWin',"
	  "interface='org.kde.KWin',member='showingDesktopChanged'",
	  "member=showingDesktopChanged", "ui_showdesktop", -1, 0, { 0 }, 0 },
};

static const int n_watches = sizeof(watches) / sizeof(watches[0]);

static const char *cfg_player = "/usr/local/bin/qcom-haptics-play";
static int cfg_gain = 100;
static long cfg_min_gap_ms = 40;
static struct event_map events[MAX_EVENTS];
static int n_events;

static const char *dev_power = "pmic_pwrkey";
static const char *dev_volup = "pmic_resin";
static const char *dev_voldown = "gpio-keys";
static const char *dev_slider = "ak09970-slider";

static struct dev *devs;
static int fifo_fd = -1;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int event_effect(const char *name)
{
	int i;

	for (i = 0; i < n_events; i++)
		if (!strcmp(events[i].name, name))
			return events[i].effect;
	return 0;
}

/* ---- config ---------------------------------------------------------- */

static void trim(char *s)
{
	size_t len;
	char *p = s;

	while (*p == ' ' || *p == '\t')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);

	len = strlen(s);
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
		       s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = 0;
}

static void read_config(const char *path)
{
	FILE *f;
	char line[STR_LEN];

	f = fopen(path, "re");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *eq;
		trim(line);
		eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq++ = 0;
		trim(line);
		trim(eq);

		if (!strcmp(line, "player")) {
			cfg_player = strdup(eq);
		} else if (!strcmp(line, "gain")) {
			cfg_gain = atoi(eq);
		} else if (!strcmp(line, "min_gap_ms")) {
			cfg_min_gap_ms = atol(eq);
		} else if (!strcmp(line, "dev_power")) {
			dev_power = strdup(eq);
		} else if (!strcmp(line, "dev_volup")) {
			dev_volup = strdup(eq);
		} else if (!strcmp(line, "dev_voldown")) {
			dev_voldown = strdup(eq);
		} else if (!strcmp(line, "dev_slider")) {
			dev_slider = strdup(eq);
		} else if (n_events < MAX_EVENTS) {
			snprintf(events[n_events].name,
				 sizeof(events[n_events].name), "%s", line);
			events[n_events].effect = atoi(eq);
			n_events++;
		}
	}
	fclose(f);
}

/* ---- playing --------------------------------------------------------- */

static void play(const char *name)
{
	static long last;
	long t;
	int effect;
	int gain;
	pid_t pid;

	effect = event_effect(name);
	if (effect <= 0)
		return;

	t = now_ms();
	if (t - last < cfg_min_gap_ms)
		return;
	last = t;

	gain = cfg_gain ? cfg_gain : 100;

	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		char e[16], g[16];

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		snprintf(e, sizeof(e), "%d", effect);
		snprintf(g, sizeof(g), "%d", gain);
		execl(cfg_player, cfg_player, "-e", e, "-g", g, (char *)NULL);
		_exit(127);
	}
}

/* ---- evdev ----------------------------------------------------------- */

static int open_evdev(const char *path, char *name, size_t nlen)
{
	int fd;

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ioctl(fd, EVIOCGNAME(nlen), name) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int wanted_device(const char *name)
{
	return !strcmp(name, dev_power) || !strcmp(name, dev_volup) ||
	       !strcmp(name, dev_voldown) || !strcmp(name, dev_slider);
}

static void add_dev(int fd, const char *name)
{
	struct dev *d = calloc(1, sizeof(*d));

	if (!d) {
		close(fd);
		return;
	}
	d->fd = fd;
	snprintf(d->name, sizeof(d->name), "%s", name);
	d->is_slider = !strcmp(name, dev_slider);
	d->next = devs;
	devs = d;
}

static void scan_devices(void)
{
	DIR *dir;
	struct dirent *ent;

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((ent = readdir(dir))) {
		char path[128], name[NAME_LEN] = { 0 };
		struct dev *d;
		int fd, dup = 0;

		if (strncmp(ent->d_name, "event", 5) ||
		    strlen(ent->d_name) > 40)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		fd = open_evdev(path, name, sizeof(name));
		if (fd < 0)
			continue;
		if (!wanted_device(name)) {
			close(fd);
			continue;
		}
		for (d = devs; d; d = d->next)
			if (!strcmp(d->name, name))
				dup = 1;
		if (dup)
			close(fd);
		else
			add_dev(fd, name);
	}
	closedir(dir);
}

static void handle_event(const char *devname, int is_slider,
			 const struct input_event *ev)
{
	if (ev->type == EV_KEY && ev->code == KEY_POWER && ev->value == 1)
		play("power");
	else if (ev->type == EV_KEY && ev->code == KEY_VOLUMEUP && ev->value == 1)
		play("volume_up");
	else if (ev->type == EV_KEY && ev->code == KEY_VOLUMEDOWN && ev->value == 1)
		play("volume_down");
	else if (is_slider && ev->type == EV_ABS && ev->code == ABS_X) {
		if (ev->value == 0)
			play("slider_up");
		else if (ev->value == 1)
			play("slider_mid");
		else if (ev->value == 2)
			play("slider_down");
	}
	(void)devname;
}

/* ---- dbus watches (desktop events) ----------------------------------- */

static void start_watch(struct dbus_watch *w)
{
	int p[2];
	pid_t pid;

	w->len = 0;
	if (pipe(p) < 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return;
	}
	if (pid == 0) {
		dup2(p[1], STDOUT_FILENO);
		close(p[0]);
		close(p[1]);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		execlp("dbus-monitor", "dbus-monitor", w->rule, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	w->fd = p[0];
	fcntl(w->fd, F_SETFL, O_NONBLOCK);
}

static void handle_watch(struct dbus_watch *w)
{
	ssize_t r = read(w->fd, w->buf + w->len,
			 sizeof(w->buf) - w->len - 1);

	if (r > 0) {
		w->len += r;
		w->buf[w->len] = 0;
		if (strstr(w->buf, w->needle)) {
			play(w->event);
			w->len = 0;
		} else if (w->len > sizeof(w->buf) - 256) {
			w->len = 0;
		}
	} else if (r == 0 || (r < 0 && errno != EAGAIN &&
			      errno != EWOULDBLOCK)) {
		close(w->fd);
		w->fd = -1;
		w->len = 0;
	}
}

/* ---- control fifo ---------------------------------------------------- */

static void start_fifo(const char *path)
{
	mkfifo(path, 0600);
	fifo_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *cfg = "/etc/giulia-hapticd.conf";
	char fifo[128];
	const char *rundir;
	struct pollfd fds[MAX_DEVS + MAX_WATCHES + 1];
	int i;

	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc)
			cfg = argv[++i];
	}

	read_config(cfg);
	scan_devices();
	rundir = getenv("XDG_RUNTIME_DIR");
	snprintf(fifo, sizeof(fifo), "%s/giulia-hapticd.fifo",
		 rundir ? rundir : "/tmp");
	start_fifo(fifo);

	for (;;) {
		struct dev *d;
		long now;
		int n = 0;

		now = now_ms();
		for (i = 0; i < n_watches; i++) {
			if (watches[i].fd < 0 &&
			    now - watches[i].last_try > 5000) {
				watches[i].last_try = now;
				start_watch(&watches[i]);
			}
		}

		for (d = devs; d; d = d->next) {
			fds[n].fd = d->fd;
			fds[n].events = POLLIN;
			n++;
		}
		for (i = 0; i < n_watches; i++) {
			if (watches[i].fd >= 0) {
				fds[n].fd = watches[i].fd;
				fds[n].events = POLLIN;
				n++;
			}
		}
		if (fifo_fd >= 0) {
			fds[n].fd = fifo_fd;
			fds[n].events = POLLIN;
			n++;
		}

		if (poll(fds, n, 5000) <= 0) {
			/* Rescan occasionally in case a device appeared */
			scan_devices();
			continue;
		}

		for (i = 0; i < n; i++) {
			int j, handled = 0;

			if (!(fds[i].revents & POLLIN))
				continue;

			if (fds[i].fd == fifo_fd) {
				char buf[256];
				ssize_t r = read(fifo_fd, buf, sizeof(buf) - 1);

				if (r > 0) {
					char *tok, *save = NULL;

					buf[r] = 0;
					for (tok = strtok_r(buf, "\r\n", &save);
					     tok; tok = strtok_r(NULL, "\r\n", &save))
						if (*tok)
							play(tok);
				}
				continue;
			}

			for (j = 0; j < n_watches; j++) {
				if (fds[i].fd == watches[j].fd) {
					handle_watch(&watches[j]);
					handled = 1;
					break;
				}
			}
			if (handled)
				continue;

			{
				struct input_event ev;
				struct dev *d;

				for (d = devs; d && d->fd != fds[i].fd;
				     d = d->next)
					;
				while (read(fds[i].fd, &ev, sizeof(ev)) ==
				       sizeof(ev))
					if (d)
						handle_event(d->name,
							     d->is_slider, &ev);
			}
		}
	}
	return 0;
}
