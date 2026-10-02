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
 *   power, volume_change, slider_up, slider_mid, slider_down,
 *   notification, ui_desktop, ui_showdesktop, ui_maximize, ui_snap,
 *   ui_minimize
 * Any event whose effect id is 0 is ignored.
 *
 * A session D-Bus method org.giulia.Haptic.Trigger(s) (at
 * /org/giulia/Haptic) lets other components -- e.g. the bundled KWin
 * "Giulia Haptics" script -- trigger any named event.
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
#include <systemd/sd-bus.h>
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
static int vol_fd = -1;
static long vol_last_try;
static int vol_last_pct = -1;
static char vol_buf[256];
static size_t vol_len;

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
	int magnitude;
	pid_t pid;

	effect = event_effect(name);
	if (effect <= 0)
		return;

	t = now_ms();
	if (t - last < cfg_min_gap_ms)
		return;
	last = t;

	/*
	 * cfg_gain is a percentage (0..100); qcom-haptics-play -g expects the
	 * raw FF periodic magnitude, and the driver scales it as
	 * vmax = magnitude * fifo_vmax_mv / 0x7fff.  Passing a percentage
	 * straight through would drive ~0.3% of vmax.
	 */
	if (cfg_gain <= 0 || cfg_gain >= 100)
		magnitude = 0x7fff;
	else
		magnitude = cfg_gain * 0x7fff / 100;

	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		char e[16], g[16];

		prctl(PR_SET_PDEATHSIG, SIGKILL);
		snprintf(e, sizeof(e), "%d", effect);
		snprintf(g, sizeof(g), "%d", magnitude);
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

/* ---- volume watch (PipeWire / PulseAudio sink) ----------------------- */

static int sink_volume_percent(void)
{
	FILE *f;
	char buf[128];
	double v;
	int pct = -1;

	f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!f)
		return -1;
	if (fgets(buf, sizeof(buf), f) && sscanf(buf, "Volume:%lf", &v) == 1)
		pct = (int)(v * 100 + 0.5);
	pclose(f);
	return pct;
}

static void start_vol_watch(void)
{
	int p[2];
	pid_t pid;

	vol_len = 0;
	vol_last_pct = sink_volume_percent();
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
		execlp("pactl", "pactl", "subscribe", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	vol_fd = p[0];
	fcntl(vol_fd, F_SETFL, O_NONBLOCK);
}

static void handle_vol_watch(void)
{
	ssize_t r = read(vol_fd, vol_buf + vol_len,
			 sizeof(vol_buf) - vol_len - 1);

	if (r > 0) {
		char *nl;

		vol_len += r;
		vol_buf[vol_len] = 0;
		while ((nl = strchr(vol_buf, '\n'))) {
			int pct;

			*nl = 0;
			if (strstr(vol_buf, "on sink")) {
				pct = sink_volume_percent();
				if (pct >= 0 && pct != vol_last_pct) {
					vol_last_pct = pct;
					play("volume_change");
				}
			}
			memmove(vol_buf, nl + 1, strlen(nl + 1) + 1);
		}
		vol_len = strlen(vol_buf);
	} else if (r == 0 || (r < 0 && errno != EAGAIN &&
			      errno != EWOULDBLOCK)) {
		close(vol_fd);
		vol_fd = -1;
	}
}

/* ---- control fifo ---------------------------------------------------- */

static void start_fifo(const char *path)
{
	mkfifo(path, 0600);
	fifo_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* ---- dbus service (org.giulia.Haptic) -------------------------------- */

static sd_bus *bus;

static int method_trigger(sd_bus_message *m, void *userdata,
			  sd_bus_error *ret_error)
{
	const char *event;
	int r;

	r = sd_bus_message_read(m, "s", &event);
	if (r < 0)
		return r;
	play(event);
	(void)userdata;
	(void)ret_error;
	return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable haptic_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Trigger", "s", "", method_trigger,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

static void start_dbus_service(void)
{
	int r;

	r = sd_bus_default_user(&bus);
	if (r < 0)
		goto fail;
	r = sd_bus_add_object_vtable(bus, NULL, "/org/giulia/Haptic",
				     "org.giulia.Haptic", haptic_vtable, NULL);
	if (r < 0)
		goto fail;
	sd_bus_request_name(bus, "org.giulia.Haptic", 0);
	return;
fail:
	if (bus) {
		sd_bus_unref(bus);
		bus = NULL;
	}
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
	const char *cfg = "/etc/giulia-hapticd.conf";
	char fifo[128];
	const char *rundir;
	struct pollfd fds[MAX_DEVS + MAX_WATCHES + 3];
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
	start_dbus_service();

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
		if (vol_fd < 0 && now - vol_last_try > 5000) {
			vol_last_try = now;
			start_vol_watch();
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
		if (bus) {
			int ev = sd_bus_get_events(bus);

			fds[n].fd = sd_bus_get_fd(bus);
			fds[n].events = ev > 0 ? ev : POLLIN;
			n++;
		}
		if (vol_fd >= 0) {
			fds[n].fd = vol_fd;
			fds[n].events = POLLIN;
			n++;
		}

		if (poll(fds, n, 5000) <= 0) {
			/* Rescan occasionally in case a device appeared */
			if (bus)
				sd_bus_process(bus, NULL);
			scan_devices();
			continue;
		}

		for (i = 0; i < n; i++) {
			int j, handled = 0;

			if (!(fds[i].revents & POLLIN))
				continue;

			if (bus && fds[i].fd == sd_bus_get_fd(bus)) {
				int r;

				do {
					r = sd_bus_process(bus, NULL);
				} while (r > 0);
				if (r < 0) {
					sd_bus_unref(bus);
					bus = NULL;
				} else {
					sd_bus_flush(bus);
				}
				continue;
			}

			if (fds[i].fd == vol_fd) {
				handle_vol_watch();
				continue;
			}

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
