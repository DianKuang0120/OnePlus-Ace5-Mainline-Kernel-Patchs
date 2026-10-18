// tee_probe: enumerate TEE devices, try a QSEECOM client session (by app
// name), and try loading the app on a privileged device.
//
// Build: gcc -O2 -o tee_probe tee_probe.c
// Usage: sudo ./tee_probe [app-name]     (default: uff_jv)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/tee.h>

struct tee_ctx {
	int fd;
	int shm_fd;
	void *shm;
	size_t shm_size;
	uint32_t shm_id;
};

static int tee_open_shm(struct tee_ctx *c, size_t size)
{
	struct tee_ioctl_shm_alloc_data data = {
		.size = size,
		.flags = 0,
	};

	c->shm_fd = ioctl(c->fd, TEE_IOC_SHM_ALLOC, &data);
	if (c->shm_fd < 0) {
		fprintf(stderr, "  TEE_IOC_SHM_ALLOC: %s\n", strerror(errno));
		return -1;
	}

	c->shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      c->shm_fd, 0);
	if (c->shm == MAP_FAILED) {
		fprintf(stderr, "  mmap: %s\n", strerror(errno));
		close(c->shm_fd);
		return -1;
	}

	c->shm_size = size;
	c->shm_id = data.id;
	return 0;
}

static void tee_close_shm(struct tee_ctx *c)
{
	if (c->shm && c->shm != MAP_FAILED)
		munmap(c->shm, c->shm_size);
	if (c->shm_fd >= 0)
		close(c->shm_fd);
	c->shm = NULL;
	c->shm_fd = -1;
}

static int open_named_session(struct tee_ctx *c, const char *app, const char *tag)
{
	size_t arg_size = sizeof(struct tee_ioctl_open_session_arg) +
			  sizeof(struct tee_ioctl_param);
	struct tee_ioctl_open_session_arg *arg = calloc(1, arg_size);
	struct tee_ioctl_buf_data buf = {
		.buf_ptr = (uintptr_t)arg,
		.buf_len = arg_size,
	};
	int ret;

	if (!arg)
		return -1;

	arg->clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	arg->cancel_id = 0x1234;
	arg->num_params = 1;
	arg->params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	arg->params[0].a = 0;
	arg->params[0].b = strlen(app) + 1;
	arg->params[0].c = c->shm_id;

	errno = 0;
	ret = ioctl(c->fd, TEE_IOC_OPEN_SESSION, &buf);
	printf("  %s '%s': ioctl=%d errno=%d (%s)\n", tag, app, ret,
	       errno, strerror(errno));
	printf("  session=0x%x ret=0x%x ret_origin=0x%x\n",
	       arg->session, arg->ret, arg->ret_origin);

	if (!ret && arg->session) {
		struct tee_ioctl_close_session_arg close_arg = {
			.session = arg->session,
		};
		ioctl(c->fd, TEE_IOC_CLOSE_SESSION, &close_arg);
	}
	free(arg);
	return ret;
}

static int probe_device(const char *path, const char *app, int privileged)
{
	struct tee_ioctl_version_data ver = { 0 };
	struct tee_ctx c = { .fd = -1, .shm_fd = -1 };
	int ret;

	c.fd = open(path, O_RDWR);
	if (c.fd < 0) {
		fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
		return -1;
	}

	ret = ioctl(c.fd, TEE_IOC_VERSION, &ver);
	if (ret) {
		fprintf(stderr, "%s: TEE_IOC_VERSION: %s\n", path, strerror(errno));
		close(c.fd);
		return -1;
	}

	printf("%s: impl_id=%u impl_caps=0x%x gen_caps=0x%x%s\n",
	       path, ver.impl_id, ver.impl_caps, ver.gen_caps,
	       (ver.gen_caps & TEE_GEN_CAP_PRIVILEGED) ? " [PRIV]" : "");

	if (privileged && !(ver.gen_caps & TEE_GEN_CAP_PRIVILEGED)) {
		close(c.fd);
		return 0;
	}
	if (!privileged && ver.impl_id != 5) {
		close(c.fd);
		return 0;
	}

	if (tee_open_shm(&c, 4096) < 0) {
		close(c.fd);
		return -1;
	}
	memset(c.shm, 0, c.shm_size);
	strncpy(c.shm, app, c.shm_size - 1);

	ret = open_named_session(&c, app, privileged ? "load" : "open");

	tee_close_shm(&c);
	close(c.fd);
	return ret;
}

int main(int argc, char **argv)
{
	const char *app = argc > 1 ? argv[1] : "uff_jv";
	char path[64];
	int i;

	/* client devices: try to open an existing / boot-loaded app */
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/tee%d", i);
		probe_device(path, app, 0);
	}
	/* privileged devices: try to load the app from firmware */
	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/teepriv%d", i);
		probe_device(path, app, 1);
	}
	return 0;
}
