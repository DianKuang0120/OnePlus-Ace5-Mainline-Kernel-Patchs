// SPDX-License-Identifier: GPL-2.0-only
/*
 * fpctl - drive the SM8650 uff_jv (Jiiov JV0301) TA through qcomtee.
 *
 * The kernel qcomtee driver exposes /dev/qcomtee_fp.  A command is sent as
 * the HAL does it:
 *
 *   u32 type = 2;          // offset 0
 *   u32 cmd;               // offset 4, FP_CMD_* below
 *   u32 status;            // offset 8, filled by the TA
 *   ...
 *   u8  payload[];         // offset 0x30
 *
 * The 64-byte QSEEComCompat request header has the total buffer length in
 * its first word; the kernel driver fills that in.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FP_DEV		"/dev/qcomtee_fp"
#define GF_DEV		"/dev/goodix_fp"
#define GF_IOC_ENABLE_POWER _IO('g', 7)

struct fp_send_data_arg {
	uint64_t req;
	uint32_t req_len;
	uint32_t resp_len;
	uint64_t resp;
	uint64_t data;
	uint32_t data_len;
	uint32_t data_out_len;
	uint32_t qsee_len;
	uint32_t data_cap;
};

#define FP_IOCTL_SEND_DATA _IOWR('F', 0x11, struct fp_send_data_arg)

#define FP_HDR			0x30
#define FP_TYPE			2

static int dump_data;
static void dump_buf(const char *tag, const void *buf, size_t len);

/* Normal fingerprint core commands handled by ta_fpcore_handle_message(). */
#define FP_CMD_FPCORE_INIT			0x7d0
#define FP_CMD_FPCORE_DEINIT			0x7d1
#define FP_CMD_FPCORE_SET_ACTIVE_GROUP		0x7d2
#define FP_CMD_ALGO_GET_FEATURE			0x7d3
#define FP_CMD_FPCORE_CAPTURE_IMG		0x7d4
#define FP_CMD_FPCORE_PRE_ENROLL		0x7d5
#define FP_CMD_FPCORE_ENROLL			0x7d6
#define FP_CMD_FPCORE_ENROLL_DOWN		0x7d7
#define FP_CMD_ALGO_ENROLL_IMAGE		0x7d8
#define FP_CMD_FPCORE_SAVE_TEMPLATE		0x7d9
#define FP_CMD_FPCORE_POST_ENROLL		0x7da
#define FP_CMD_FPCORE_GET_AUTHENTICATOR_ID	0x7db
#define FP_CMD_FPCORE_AUTHENTICATE		0x7dc
#define FP_CMD_FPCORE_AUTHENTICATE_DOWN		0x7dd
#define FP_CMD_FPCORE_AUTHENTICATE_COMPARE	0x7de
#define FP_CMD_FPCORE_AUTHENTICATE_STUDY	0x7df
#define FP_CMD_FPCORE_AUTHENTICATE_FINISH	0x7e0
#define FP_CMD_FPCORE_REMOVE			0x7e1
#define FP_CMD_FPCORE_ENUMERATE			0x7e2
#define FP_CMD_FPCORE_CANCEL			0x7e3
#define FP_CMD_FPCORE_SET_HMACKEY		0x7e4
#define FP_CMD_FPCORE_CUSTOMENC			0x7e5
#define FP_CMD_FPCORE_GET_DCSINFO		0x7e6
#define FP_CMD_FPCORE_SET_SENSOR_MODE		0x7e7
#define FP_CMD_FPCORE_AUTH_STUDY_SPLIT		0x800
#define FP_CMD_FPCORE_UPDATE_TEMPLATE		0x801
#define FP_CMD_FPCORE_INIT_TA_CONFIG		0x834

/*
 * Captured from the Android HAL startup (fps_hal), command 0x834.
 * Payload only; the kernel/fp_send fills type and command id.
 * The strings are the display/build id, panel vendor and sensor name.
 */
static const uint8_t init_ta_config[0x80] = {
	0x32, 0x33, 0x38, 0x35, 0x31, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x42, 0x4f, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x47, 0x5f, 0x4f, 0x50, 0x54, 0x49, 0x43, 0x41,
	0x4c, 0x5f, 0x4a, 0x56, 0x30, 0x33, 0x30, 0x31,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x4e, 0x55, 0x4c, 0x4c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct fp_cmd_info {
	uint32_t cmd;
	const char *name;
	uint32_t len;
};

static const struct fp_cmd_info fp_cmds[] = {
	{ FP_CMD_FPCORE_INIT, "init", 0x1c0 },
	{ FP_CMD_FPCORE_DEINIT, "deinit", 0x78 },
	{ FP_CMD_FPCORE_SET_ACTIVE_GROUP, "set-active-group", 0x100 },
	{ FP_CMD_ALGO_GET_FEATURE, "get-feature", 0xa0 },
	{ FP_CMD_FPCORE_CAPTURE_IMG, "capture", 0xb8 },
	{ FP_CMD_FPCORE_PRE_ENROLL, "pre-enroll", 0x80 },
	{ FP_CMD_FPCORE_ENROLL, "enroll", 0xc8 },
	{ FP_CMD_FPCORE_ENROLL_DOWN, "enroll-down", 0x90 },
	{ FP_CMD_ALGO_ENROLL_IMAGE, "enroll-image", 0xb0 },
	{ FP_CMD_FPCORE_SAVE_TEMPLATE, "save-template", 0x80 },
	{ FP_CMD_FPCORE_POST_ENROLL, "post-enroll", 0x78 },
	{ FP_CMD_FPCORE_GET_AUTHENTICATOR_ID, "auth-id", 0x80 },
	{ FP_CMD_FPCORE_AUTHENTICATE, "authenticate", 0x88 },
	{ FP_CMD_FPCORE_AUTHENTICATE_DOWN, "auth-down", 0xa0 },
	{ FP_CMD_FPCORE_AUTHENTICATE_COMPARE, "auth-compare", 0xf0 },
	{ FP_CMD_FPCORE_AUTHENTICATE_STUDY, "auth-study", 0x80 },
	{ FP_CMD_FPCORE_AUTHENTICATE_FINISH, "auth-finish", 0x78 },
	{ FP_CMD_FPCORE_REMOVE, "remove", 0x80 },
	{ FP_CMD_FPCORE_ENUMERATE, "enumerate", 0x98 },
	{ FP_CMD_FPCORE_CANCEL, "cancel", 0x30 },
	{ FP_CMD_FPCORE_SET_HMACKEY, "set-hmackey", 0x130 },
	{ FP_CMD_FPCORE_CUSTOMENC, "customenc", 0x238 },
	{ FP_CMD_FPCORE_GET_DCSINFO, "get-dcsinfo", 0x240 },
	{ FP_CMD_FPCORE_SET_SENSOR_MODE, "set-sensor-mode", 0x80 },
	{ FP_CMD_FPCORE_INIT_TA_CONFIG, "init-ta-config", 0xb0 },
};

static int hex2bin(const char *s, uint8_t *out, size_t max)
{
	size_t n = 0;
	while (*s && n < max) {
		unsigned int v;
		while (*s == ' ' || *s == '\t' || *s == ':')
			s++;
		if (!*s)
			break;
		if (sscanf(s, "%2x", &v) != 1)
			break;
		out[n++] = v;
		s += 2;
	}
	return n;
}

static int fp_needs_extra(uint32_t cmd)
{
	switch (cmd) {
	case FP_CMD_ALGO_GET_FEATURE:
	case FP_CMD_FPCORE_CAPTURE_IMG:
	case FP_CMD_FPCORE_ENROLL_DOWN:
	case FP_CMD_ALGO_ENROLL_IMAGE:
	case FP_CMD_FPCORE_SAVE_TEMPLATE:
		return 1;
	default:
		return 0;
	}
}

static int fp_send(int fd, uint32_t cmd, uint32_t total_len,
		   const void *payload, size_t payload_len,
		   uint32_t qsee_len, uint32_t data_cap,
		   void *out, size_t out_len)
{
	struct fp_send_data_arg a;
	uint8_t req[64] = { 0 };
	uint8_t *resp;
	uint8_t *data;
	uint32_t cap;
	uint32_t status;
	size_t data_out_len;
	int ret;

	if (total_len < FP_HDR || total_len > (1U << 20)) {
		errno = EINVAL;
		return -1;
	}
	if (payload_len > total_len - FP_HDR) {
		errno = E2BIG;
		return -1;
	}

	cap = data_cap ? data_cap : total_len;
	if (cap < total_len)
		cap = total_len;
	data = calloc(1, cap);
	resp = calloc(1, out_len ? out_len : 256);
	if (!data || !resp) {
		free(data);
		free(resp);
		errno = ENOMEM;
		return -1;
	}

	*(uint32_t *)(data + 0) = FP_TYPE;
	*(uint32_t *)(data + 4) = cmd;
	if (cmd == FP_CMD_FPCORE_INIT) {
		struct timespec ts;

		if (!clock_gettime(CLOCK_MONOTONIC, &ts))
			*(uint64_t *)(data + 0x10) =
				(uint64_t)ts.tv_sec * 1000000000ULL +
				ts.tv_nsec;
	}
	if (payload_len)
		memcpy(data + FP_HDR, payload, payload_len);

	a.req = (uint64_t)(uintptr_t)req;
	a.req_len = sizeof(req);
	a.resp_len = out_len ? out_len : 256;
	a.resp = (uint64_t)(uintptr_t)resp;
	a.data = (uint64_t)(uintptr_t)data;
	a.data_len = total_len;
	a.qsee_len = qsee_len;
	a.data_cap = cap;

	ret = ioctl(fd, FP_IOCTL_SEND_DATA, &a);
	if (ret) {
		ret = -errno;
	} else {
		data_out_len = a.data_out_len ? a.data_out_len : total_len;
		status = *(uint32_t *)(data + 8);
		if (dump_data) {
			FILE *f = fopen("/tmp/fp-last.bin", "wb");

			if (f) {
				fwrite(data, 1, data_out_len, f);
				fclose(f);
			}
			printf("saved %zu bytes to /tmp/fp-last.bin\n", data_out_len);
		}
		if (out && out_len)
			memcpy(out, data, data_out_len < out_len ? data_out_len : out_len);
		ret = status;
		if (dump_data)
			dump_buf("rsp", resp, a.resp_len);
	}

	free(data);
	free(resp);
	return ret;
}

static void dump_buf(const char *tag, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t i;

	printf("%s:", tag);
	for (i = 0; i < len; i++) {
		if (!(i % 16))
			printf("\n%04zx:", i);
		printf(" %02x", p[i]);
	}
	printf("\n");
}

static void usage(const char *prog)
{
	size_t i;

	fprintf(stderr,
		"usage: %s [-p] [-x] init|init-config|status|raw <cmd> <len> [hex]\n"
		"       %s [-p] <command> [hex-payload]\n"
		"       %s power-hold <seconds>\n"
		"       %s rawbig <cmd> <len> [hex]\n\n"
		"  -p  enable fingerprint vdd first (no reset pulse)\n"
		"  -x  hex-dump the returned command buffer\n\n"
		"commands:\n", prog, prog, prog, prog);
	for (i = 0; i < sizeof(fp_cmds) / sizeof(fp_cmds[0]); i++)
		fprintf(stderr, "  %-18s 0x%03x len 0x%x\n",
			fp_cmds[i].name, fp_cmds[i].cmd, fp_cmds[i].len);
}

int main(int argc, char **argv)
{
	int gf_fd = -1;
	int fd, ret, i = 1;
	int do_power = 0;
	int dump = 0;
	uint8_t result[0x400] = { 0 };
	const char *cmd;
	uint8_t payload[4096] = { 0 };
	size_t payload_len = 0;
	uint32_t id, len;

	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "-p")) {
			do_power = 1;
			i++;
		} else if (!strcmp(argv[i], "-x")) {
			dump = 1;
			i++;
		} else {
			break;
		}
	}
	dump_data = dump;
	if (i >= argc) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[i++];

	if (!strcmp(cmd, "power-hold")) {
		int seconds = i < argc ? atoi(argv[i]) : 600;

		gf_fd = open(GF_DEV, O_RDWR);
		if (gf_fd < 0) {
			perror("open " GF_DEV);
			return 1;
		}
		ioctl(gf_fd, GF_IOC_ENABLE_POWER);
		usleep(5000);
		printf("holding fingerprint vdd for %d s\n", seconds);
		fflush(stdout);
		sleep(seconds);
		close(gf_fd);
		return 0;
	}

	if (do_power) {
		gf_fd = open(GF_DEV, O_RDWR);
		if (gf_fd < 0) {
			perror("open " GF_DEV);
			return 1;
		}
		ioctl(gf_fd, GF_IOC_ENABLE_POWER);
		usleep(5000);
	}

	fd = open(FP_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " FP_DEV);
		if (gf_fd >= 0)
			close(gf_fd);
		return 1;
	}

	if (!strcmp(cmd, "init")) {
		ret = fp_send(fd, FP_CMD_FPCORE_INIT_TA_CONFIG, 0xb0,
			      init_ta_config, sizeof(init_ta_config),
			      0, 0xb0, result, sizeof(result));
		printf("init-ta-config: %d (0x%x)\n", ret, ret);
		if (dump) dump_buf("config", result, 0xb0);
		ret = fp_send(fd, FP_CMD_FPCORE_INIT, 0x1c0,
			      NULL, 0, 0xf000 + 0x1c0, 0x1c0,
			      result, sizeof(result));
		printf("init: %d (0x%x)\n", ret, ret);
		if (dump) dump_buf("init", result, 0x1c0);
	} else if (!strcmp(cmd, "init-config")) {
		ret = fp_send(fd, FP_CMD_FPCORE_INIT_TA_CONFIG, 0xb0,
			      init_ta_config, sizeof(init_ta_config),
			      0, 0xb0, result, sizeof(result));
		printf("init-ta-config: %d (0x%x)\n", ret, ret);
		if (dump) dump_buf("config", result, 0xb0);
	} else if (!strcmp(cmd, "status")) {
		ret = fp_send(fd, FP_CMD_FPCORE_GET_AUTHENTICATOR_ID, 0x80,
			      NULL, 0, 0xf000 + 0x80, 0x80,
			      result, sizeof(result));
		printf("auth-id: %d (0x%x)\n", ret, ret);
		if (dump) dump_buf("auth-id", result, 0x80);
	} else if (!strcmp(cmd, "rawbig")) {
		if (i + 1 >= argc) {
			usage(argv[0]);
			return 2;
		}
		id = strtoul(argv[i++], NULL, 0);
		len = strtoul(argv[i++], NULL, 0);
		if (i < argc)
			payload_len = hex2bin(argv[i++], payload,
					      sizeof(payload));
		/* Keep the command length in QSEE word 0, but allow extra space. */
		ret = fp_send(fd, id, len, payload, payload_len,
			      len, 0xf000 + len, result, sizeof(result));
		if (dump) dump_buf("rawbig", result, len);
		printf("rawbig 0x%x len 0x%x: %d (0x%x)\n",
		       id, len, ret, ret);
	} else if (!strcmp(cmd, "raw")) {
		if (i + 1 >= argc) {
			usage(argv[0]);
			return 2;
		}
		id = strtoul(argv[i++], NULL, 0);
		len = strtoul(argv[i++], NULL, 0);
		if (i < argc)
			payload_len = hex2bin(argv[i++], payload,
					      sizeof(payload));
		ret = fp_send(fd, id, len, payload, payload_len,
			      0xf000 + len, len,
			      result, sizeof(result));
		if (dump) dump_buf("raw", result, len);
		printf("cmd 0x%x len 0x%x: %d (0x%x)\n",
		       id, len, ret, ret);
	} else {
		size_t n;

		for (n = 0; n < sizeof(fp_cmds) / sizeof(fp_cmds[0]); n++)
			if (!strcmp(cmd, fp_cmds[n].name))
				break;
		if (n == sizeof(fp_cmds) / sizeof(fp_cmds[0])) {
			usage(argv[0]);
			return 2;
		}
		if (i < argc)
			payload_len = hex2bin(argv[i++], payload,
					      sizeof(payload));
		ret = fp_send(fd, fp_cmds[n].cmd, fp_cmds[n].len,
			      payload, payload_len,
			      0xf000 + fp_cmds[n].len,
			      fp_needs_extra(fp_cmds[n].cmd) ?
				      0xf000 + fp_cmds[n].len :
				      fp_cmds[n].len,
			      result, sizeof(result));
		printf("%s: %d (0x%x)\n", fp_cmds[n].name, ret, ret);
		if (dump) dump_buf(fp_cmds[n].name, result, fp_cmds[n].len);
	}

	close(fd);
	if (gf_fd >= 0)
		close(gf_fd);
	return 0;
}
