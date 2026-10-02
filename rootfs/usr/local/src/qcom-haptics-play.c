// SPDX-License-Identifier: GPL-2.0-only
/*
 * qcom-haptics-play - play a raw haptics waveform on the PM8550B HV haptics
 * device through the standard input FF interface (FF_PERIODIC + FF_CUSTOM).
 *
 * The waveform files are the stock OnePlus / Qualcomm ones, e.g.
 *   /odm/etc/vibrator/<vibrator-type>/def/effect_N.bin
 * raw unsigned 8-bit samples played at the rate from vibrator_effect.json
 * (24000 Hz for vibrator type 809).
 *
 * Usage:
 *   qcom-haptics-play -f <effect.bin> [-r <rate_hz>] [-d <evdev name>] [-g <0..65535>]
 *   qcom-haptics-play -l <dir>          list .bin effects in a directory
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Must match struct custom_fifo_data in qcom-hv-haptics.c */
struct custom_fifo_data {
	uint32_t idx;
	uint32_t length;
	uint32_t play_rate_hz;
	uint8_t *data;
};

static int find_ff_device(const char *name)
{
	int i;

	for (i = 0; i < 64; i++) {
		char path[64], nm[256] = { 0 };
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(nm)), nm) >= 0 &&
		    !strcmp(nm, name))
			return fd;
		close(fd);
	}
	return -1;
}

static long read_file(const char *path, uint8_t **buf)
{
	FILE *f = fopen(path, "rb");
	long len;

	if (!f) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) {
		fclose(f);
		return -1;
	}
	*buf = malloc(len);
	if (!*buf || fread(*buf, 1, len, f) != (size_t)len) {
		fprintf(stderr, "read %s failed\n", path);
		free(*buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	return len;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s -f <effect.bin> [-r rate_hz] [-d evdev_name] [-g gain]\n"
		"       %s -l <dir>\n", p, p);
}

int main(int argc, char **argv)
{
	const char *file = NULL, *devname = "qcom-hv-haptics";
	unsigned int rate = 24000, gain = 0;
	int opt, fd, duration_ms;

	while ((opt = getopt(argc, argv, "f:r:d:g:h")) != -1) {
		switch (opt) {
		case 'f':
			file = optarg;
			break;
		case 'r':
			rate = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			devname = optarg;
			break;
		case 'g':
			gain = strtoul(optarg, NULL, 0);
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!file) {
		usage(argv[0]);
		return 1;
	}

	fd = find_ff_device(devname);
	if (fd < 0) {
		fprintf(stderr, "FF device '%s' not found\n", devname);
		return 1;
	}

	{
		uint8_t *samples = NULL;
		long len = read_file(file, &samples);
		struct custom_fifo_data cfd;
		struct ff_effect eff;
		struct input_event ev;
		struct timespec ts;

		if (len <= 0) {
			close(fd);
			return 1;
		}

		cfd.idx = 0;
		cfd.length = (uint32_t)len;
		cfd.play_rate_hz = rate;
		cfd.data = samples;

		memset(&eff, 0, sizeof(eff));
		eff.type = FF_PERIODIC;
		eff.id = -1;
		eff.u.periodic.waveform = FF_CUSTOM;
		eff.u.periodic.custom_len = sizeof(struct custom_fifo_data);
		eff.u.periodic.custom_data = (int16_t *)(void *)&cfd;
		eff.u.periodic.magnitude = gain ? (int16_t)gain : 0x7fff;
		duration_ms = (int)((uint64_t)len * 1000 / rate) + 20;
		eff.replay.length = duration_ms;
		eff.replay.delay = 0;

		if (ioctl(fd, EVIOCSFF, &eff) < 0) {
			fprintf(stderr, "EVIOCSFF failed: %s (len=%u rate=%u)\n",
				strerror(errno), cfd.length, rate);
			free(samples);
			close(fd);
			return 1;
		}

		ev.type = EV_FF;
		ev.code = eff.id;
		ev.value = 1;
		if (write(fd, &ev, sizeof(ev)) < 0) {
			fprintf(stderr, "EV_FF play failed: %s\n", strerror(errno));
			ioctl(fd, EVIOCRMFF, eff.id);
			free(samples);
			close(fd);
			return 1;
		}

		printf("playing %s: %ld samples @ %u Hz (%d ms), effect id %d\n",
		       file, len, rate, duration_ms, eff.id);

		ts.tv_sec = duration_ms / 1000;
		ts.tv_nsec = (long)(duration_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);

		ev.value = 0;
		write(fd, &ev, sizeof(ev));
		ioctl(fd, EVIOCRMFF, eff.id);
		free(samples);
	}

	close(fd);
	return 0;
}
