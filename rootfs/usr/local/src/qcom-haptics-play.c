// SPDX-License-Identifier: GPL-2.0-only
/*
 * qcom-haptics-play - play a stock haptics waveform on the PM8550B HV haptics
 * device through the standard input FF interface (FF_PERIODIC + FF_CUSTOM).
 *
 * Two ways to select the waveform:
 *   -f <file>            play a raw waveform file directly
 *   -e <effect_id>       look the effect up in vibrator_effect.json for the
 *                        given vibrator type / style and play it
 *
 * Usage:
 *   qcom-haptics-play -e <id> [-t 809] [-s def|soft] [-g gain]
 *   qcom-haptics-play -f <effect.bin> [-r rate_hz] [-g gain]
 *   -d <evdev name>   FF device name  (default qcom-hv-haptics)
 *   -j <json>         effect table    (default /usr/share/haptics/vibrator_effect.json)
 *   -b <dir>          waveform dir    (default /usr/share/haptics)
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
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

static char *read_text(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;
	char *buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc(len + 1);
	if (!buf || fread(buf, 1, len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

/* Minimal helper: get the value of "key" within [start,end). */
static long json_int(const char *s, const char *end, const char *key)
{
	char pat[64];
	const char *p = s;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	while ((p = strstr(p, pat)) && p < end) {
		const char *c = strchr(p, ':');

		if (c && c < end)
			return strtol(c + 1, NULL, 10);
		p++;
	}
	return -1;
}

static int json_str(const char *s, const char *end, const char *key,
		    char *out, size_t outlen)
{
	char pat[64];
	const char *p = s;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	while ((p = strstr(p, pat)) && p < end) {
		const char *c = strchr(p, ':');
		const char *q, *r;

		if (!c || c >= end)
			break;
		q = strchr(c, '"');
		if (!q || q >= end)
			break;
		r = strchr(q + 1, '"');
		if (!r || r >= end)
			break;
		snprintf(out, outlen, "%.*s", (int)(r - q - 1), q + 1);
		return 0;
	}
	return -1;
}

/* effect_id -> file basename + play rate, for type/style */
static int json_lookup(const char *json, const char *type, const char *style,
		       long id, char *file, size_t filelen, unsigned int *rate)
{
	char key[64];
	const char *p, *sec, *secend, *arr, *arrend;

	snprintf(key, sizeof(key), "\"%s\"", type);
	p = strstr(json, key);
	if (!p)
		return -1;

	snprintf(key, sizeof(key), "\"%s_style\"", style);
	sec = strstr(p, key);
	if (!sec)
		return -1;
	secend = strchr(sec, ']');
	arr = strchr(sec, '[');
	if (!arr || !secend || arr > secend)
		return -1;
	arrend = secend;

	for (p = arr; p < arrend; ) {
		const char *ob = strchr(p, '{');
		const char *oe;

		if (!ob || ob > arrend)
			break;
		oe = strchr(ob, '}');
		if (!oe)
			break;

		if (json_int(ob, oe, "effect_id") == id) {
			char path[512];

			if (json_str(ob, oe, "effect_file", path, sizeof(path)))
				return -1;
			snprintf(file, filelen, "%s", basename(path));
			*rate = (unsigned int)json_int(ob, oe, "play_rate_hz");
			return 0;
		}
		p = oe + 1;
	}
	return -1;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s -f <effect.bin> [-r rate_hz] [-g gain] [-d dev]\n"
		"       %s -e <effect_id> [-t type] [-s def|soft] [-g gain] [-d dev]\n",
		p, p);
}

int main(int argc, char **argv)
{
	const char *file = NULL, *devname = "qcom-hv-haptics";
	const char *jsonpath = "/usr/share/haptics/vibrator_effect.json";
	const char *basedir = "/usr/share/haptics";
	const char *type = "809", *style = "def";
	char pathbuf[1024];
	long effect_id = -1;
	unsigned int rate = 0, gain = 0;
	int opt, fd, duration_ms;
	uint8_t *samples = NULL;

	while ((opt = getopt(argc, argv, "f:e:t:s:r:g:d:j:b:h")) != -1) {
		switch (opt) {
		case 'f':
			file = optarg;
			break;
		case 'e':
			effect_id = strtol(optarg, NULL, 0);
			break;
		case 't':
			type = optarg;
			break;
		case 's':
			style = optarg;
			break;
		case 'r':
			rate = strtoul(optarg, NULL, 0);
			break;
		case 'g':
			gain = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			devname = optarg;
			break;
		case 'j':
			jsonpath = optarg;
			break;
		case 'b':
			basedir = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (effect_id >= 0) {
		char *json = read_text(jsonpath);
		char base[256], upath[1024];

		if (!json) {
			fprintf(stderr, "cannot read %s: %s\n", jsonpath,
				strerror(errno));
			return 1;
		}
		if (json_lookup(json, type, style, effect_id, base, sizeof(base),
				&rate)) {
			fprintf(stderr, "effect_id %ld not found in %s/%s\n",
				effect_id, type, style);
			free(json);
			return 1;
		}
		free(json);
		snprintf(upath, sizeof(upath), "%s/%s/%s/%s", basedir, type, style,
			 base);
		file = pathbuf;
		snprintf(pathbuf, sizeof(pathbuf), "%s", upath);
		printf("effect_id %ld -> %s @ %u Hz\n", effect_id, pathbuf, rate);
	}

	if (!file) {
		usage(argv[0]);
		return 1;
	}
	if (!rate)
		rate = 24000;

	fd = find_ff_device(devname);
	if (fd < 0) {
		fprintf(stderr, "FF device '%s' not found\n", devname);
		return 1;
	}

	{
		FILE *fp = fopen(file, "rb");
		long len;

		if (!fp) {
			fprintf(stderr, "cannot open %s: %s\n", file,
				strerror(errno));
			close(fd);
			return 1;
		}
		fseek(fp, 0, SEEK_END);
		len = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		samples = malloc(len);
		if (!samples || fread(samples, 1, len, fp) != (size_t)len) {
			fprintf(stderr, "read %s failed\n", file);
			free(samples);
			fclose(fp);
			close(fd);
			return 1;
		}
		fclose(fp);

		{
			struct custom_fifo_data cfd;
			struct ff_effect eff;
			struct input_event ev;
			struct timespec ts;

			cfd.idx = 0;
			cfd.length = (uint32_t)len;
			cfd.play_rate_hz = rate;
			cfd.data = samples;

			memset(&eff, 0, sizeof(eff));
			eff.type = FF_PERIODIC;
			eff.id = -1;
			eff.u.periodic.waveform = FF_CUSTOM;
			eff.u.periodic.custom_len =
				sizeof(struct custom_fifo_data);
			eff.u.periodic.custom_data = (int16_t *)(void *)&cfd;
			eff.u.periodic.magnitude = gain ? (int16_t)gain : 0x7fff;
			duration_ms = (int)((uint64_t)len * 1000 / rate) + 20;
			eff.replay.length = duration_ms;

			if (ioctl(fd, EVIOCSFF, &eff) < 0) {
				fprintf(stderr,
					"EVIOCSFF failed: %s (len=%u rate=%u)\n",
					strerror(errno), cfd.length, rate);
				free(samples);
				close(fd);
				return 1;
			}

			ev.type = EV_FF;
			ev.code = eff.id;
			ev.value = 1;
			if (write(fd, &ev, sizeof(ev)) < 0) {
				fprintf(stderr, "EV_FF play failed: %s\n",
					strerror(errno));
				ioctl(fd, EVIOCRMFF, eff.id);
				free(samples);
				close(fd);
				return 1;
			}

			printf("playing %s: %ld samples @ %u Hz (%d ms), id %d\n",
			       file, len, rate, duration_ms, eff.id);

			ts.tv_sec = duration_ms / 1000;
			ts.tv_nsec = (long)(duration_ms % 1000) * 1000000L;
			nanosleep(&ts, NULL);

			ev.value = 0;
			write(fd, &ev, sizeof(ev));
			ioctl(fd, EVIOCRMFF, eff.id);
			free(samples);
		}
	}

	close(fd);
	return 0;
}
