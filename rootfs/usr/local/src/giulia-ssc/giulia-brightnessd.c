// SPDX-License-Identifier: GPL-2.0-only
/*
 * giulia-brightnessd - automatic brightness for the OnePlus 13R / Ace 5
 * (giulia) mainline port.
 *
 * giulia's ambient light sensor (TCS3720) is owned by the ADSP SSC, which
 * publishes it as the OPLUS vendor data type "wise_rgb" (a float array whose
 * element 9 is lux), not as the standard "ambient_light" SUID.  iio-sensor-proxy
 * and KWin only understand "ambient_light", so this small daemon consumes
 * "wise_rgb" through libssc and drives the panel backlight directly.
 *
 * Build:
 *   gcc giulia-brightnessd.c -o giulia-brightnessd \
 *       $(pkg-config --cflags --libs libssc gio-2.0)
 *
 * Run (user service; brightnessctl works as a normal user here):
 *   giulia-brightnessd [-d DATA_TYPE] [-i LUX_INDEX] [-n] [-v]
 */

#include <gio/gio.h>
#include <glib.h>
#include <libssc/libssc-sensor.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GMainLoop *loop;
static SSCSensor *sensor;

static const char *data_type = "wise_rgb";
static int lux_index = 9;
static double min_lux = 5.0, max_lux = 2000.0;
static int min_pct = 8, max_pct = 100;
static const char *backlight = NULL;	/* NULL = brightnessctl default */
static gboolean dry_run, verbose;

static double ema = -1.0;
static int last_pct = -1;

static int lux_to_pct(double lux)
{
	double t;

	if (lux <= min_lux)
		return min_pct;
	if (lux >= max_lux)
		return max_pct;
	t = (log(lux) - log(min_lux)) / (log(max_lux) - log(min_lux));
	return (int)(min_pct + t * (max_pct - min_pct) + 0.5);
}

static void set_brightness(int pct)
{
	char cmd[256];

	if (pct < 1)
		pct = 1;
	if (pct > 100)
		pct = 100;
	if (pct == last_pct)
		return;

	if (backlight)
		snprintf(cmd, sizeof(cmd), "brightnessctl -d %s set %d%%", backlight, pct);
	else
		snprintf(cmd, sizeof(cmd), "brightnessctl set %d%%", pct);

	if (dry_run) {
		fprintf(stderr, "[dry-run] %s\n", cmd);
		last_pct = pct;
		return;
	}
	if (system(cmd) == 0) {
		last_pct = pct;
		if (verbose)
			fprintf(stderr, "brightness -> %d%%\n", pct);
	}
}

static void on_report(GObject *client, guint32 msg_id, guint64 uid_high,
		      guint64 uid_low, GArray *buf, gpointer user_data)
{
	const guint8 *p = (const guint8 *)buf->data;
	guint flen, n, i;
	double lux;

	(void)client; (void)uid_high; (void)uid_low; (void)user_data;

	if (msg_id != 0x401 || buf->len < 2 || p[0] != 0x0a)
		return;
	flen = p[1];
	if (flen + 2 > buf->len || flen % 4 != 0)
		return;
	n = flen / 4;
	if ((guint)lux_index >= n)
		return;

	for (i = 0; i < n; i++) {
		float f;

		memcpy(&f, p + 2 + i * 4, 4);
		if ((int)i == lux_index)
			lux = f;
		if (verbose)
			fprintf(stderr, "  f[%u]=%g%s", i, f, i + 1 == n ? "\n" : " ");
	}

	if (lux < 0)
		lux = 0;
	ema = (ema < 0.0) ? lux : ema * 0.8 + lux * 0.2;
	if (verbose)
		fprintf(stderr, "lux=%g ema=%g -> %d%%\n", lux, ema, lux_to_pct(ema));
	set_brightness(lux_to_pct(ema));
}

static void on_open(GObject *o, GAsyncResult *res, gpointer user_data)
{
	GError *e = NULL;

	(void)user_data;
	if (!ssc_sensor_open_finish(SSC_SENSOR(o), res, &e)) {
		fprintf(stderr, "open error: %s\n", e ? e->message : "?");
		g_main_loop_quit(loop);
	}
}

static void on_new(GObject *o, GAsyncResult *res, gpointer user_data)
{
	GError *e = NULL;
	GObject *client = NULL;
	gchar *name = NULL, *dt = NULL;

	(void)o; (void)user_data;
	sensor = ssc_sensor_new_finish(res, &e);
	if (!sensor) {
		fprintf(stderr, "sensor '%s' unavailable: %s\n", data_type,
			e ? e->message : "?");
		g_main_loop_quit(loop);
		return;
	}
	g_object_get(sensor, "name", &name, "data-type", &dt, NULL);
	g_object_get(sensor, "client", &client, NULL);
	g_signal_connect(client, "report", G_CALLBACK(on_report), NULL);
	fprintf(stderr, "giulia-brightnessd: %s (%s) lux_index=%d\n",
		dt ? dt : data_type, name ? name : "-", lux_index);
	ssc_sensor_open(sensor, NULL, on_open, NULL);
}

int main(int argc, char **argv)
{
	int opt;

	while ((opt = getopt(argc, argv, "d:i:b:nhv")) != -1) {
		switch (opt) {
		case 'd': data_type = optarg; break;
		case 'i': lux_index = atoi(optarg); break;
		case 'b': backlight = optarg; break;
		case 'n': dry_run = TRUE; break;
		case 'v': verbose = TRUE; break;
		default:
			fprintf(stderr, "usage: %s [-d DATA_TYPE] [-i LUX_INDEX] [-b BACKLIGHT] [-n] [-v]\n", argv[0]);
			return 1;
		}
	}

	loop = g_main_loop_new(NULL, FALSE);
	ssc_sensor_new((gchar *)data_type, NULL, on_new, NULL);
	g_main_loop_run(loop);
	return 0;
}
