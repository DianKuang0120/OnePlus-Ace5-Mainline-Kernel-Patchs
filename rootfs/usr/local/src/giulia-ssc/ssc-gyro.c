/* Tiny libssc gyroscope reader (for verifying the giulia SSC gyro). */
#include <libssc/libssc.h>
#include <libssc/libssc-sensor-gyroscope.h>
#include <glib.h>
#include <stdio.h>

static void on_meas(SSCSensorGyroscope *s, gfloat x, gfloat y, gfloat z, gpointer u)
{
	printf("Gyro X=%8.4f Y=%8.4f Z=%8.4f rad/s\n", x, y, z);
	fflush(stdout);
}

int main(void)
{
	GError *err = NULL;
	GMainLoop *loop;

	SSCSensorGyroscope *g = ssc_sensor_gyroscope_new_sync(NULL, &err);
	if (!g) {
		fprintf(stderr, "new_sync: %s\n", err ? err->message : "?");
		return 1;
	}
	g_signal_connect(g, "measurement", G_CALLBACK(on_meas), NULL);
	if (!ssc_sensor_gyroscope_open_sync(g, NULL, &err)) {
		fprintf(stderr, "open_sync: %s\n", err ? err->message : "?");
		return 1;
	}
	loop = g_main_loop_new(NULL, FALSE);
	g_timeout_add_seconds(6, (GSourceFunc)g_main_loop_quit, loop);
	g_main_loop_run(loop);
	ssc_sensor_gyroscope_close_sync(g, NULL, &err);
	return 0;
}
