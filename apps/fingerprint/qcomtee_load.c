// qcomtee_load: load the QSEE5/QTEE fingerprint TA through the qcomtee
// (smcinvoke) object interface.
//
// The legacy QSEECOM APP_START SMC is not supported on SM8650; the vendor
// loader uses smcinvoke's QSEEComCompatAppLoader (UID 122).  This tool:
//   1. root object op 5 (REG_WITH_CREDENTIALS) -> client env object
//   2. client env op 0 (OPEN, UID 122)        -> app loader object
//   3. app loader op 1 (loadFromBuffer)       -> app controller object
//
// Build:
//   gcc -O2 -I/home/DianKuang/Mainline-Kernel/linux/include/uapi \
//       -o qcomtee_load qcomtee_load.c
// Usage:
//   sudo ./qcomtee_load [app-name]     (default uff_jv)

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

#define QCOMTEE_ROOT_OP_REG_WITH_CREDENTIALS 5
#define QCOMTEE_CLIENT_ENV_OPEN 0
#define QCOMTEE_OBJREF_FLAG_MEM (1 << 2)
#define QSEECOM_COMPAT_APP_LOADER_UID 122
#define QSEECOM_COMPAT_APPLOADER_OP_LOAD_FROM_REGION 0
#define QSEECOM_COMPAT_APPLOADER_OP_LOAD_FROM_BUFFER 1
#define QSEECOM_COMPAT_APPLOADER_OP_LOOKUP_TA 2

static int tee_object_invoke(int fd, uint64_t id, uint32_t op,
			     struct tee_ioctl_param *params, uint32_t n,
			     uint32_t *ret)
{
	size_t len = sizeof(struct tee_ioctl_object_invoke_arg) +
		     n * sizeof(struct tee_ioctl_param);
	struct tee_ioctl_object_invoke_arg *arg = calloc(1, len);
	struct tee_ioctl_buf_data buf = {
		.buf_ptr = (uintptr_t)arg,
		.buf_len = len,
	};
	int rc;

	if (!arg)
		return -1;

	arg->id = id;
	arg->op = op;
	arg->num_params = n;
	if (n)
		memcpy(arg->params, params, n * sizeof(*params));

	errno = 0;
	rc = ioctl(fd, TEE_IOC_OBJECT_INVOKE, &buf);
	if (rc < 0)
		fprintf(stderr, "  invoke id=0x%llx op=%u: %s\n",
			(unsigned long long)id, op, strerror(errno));
	else if (ret)
		*ret = arg->ret;

	if (n)
		memcpy(params, arg->params, n * sizeof(*params));
	free(arg);
	return rc;
}

static void *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;
	void *p;

	if (!f) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc(n);
	if (!p || fread(p, 1, n, f) != (size_t)n) {
		fprintf(stderr, "%s: read failed\n", path);
		free(p);
		p = NULL;
		n = 0;
	}
	fclose(f);
	*size = n;
	return p;
}

/* Assemble <name>.mdt + every <name>.bNN, matching the vendor loader. */
static void *load_ta_image(const char *name, size_t *out_size)
{
	char path[256];
	size_t mdt_size, total, off;
	uint8_t *mdt, *img;
	uint16_t phnum;
	unsigned int i;

	snprintf(path, sizeof(path), "/lib/firmware/%s.mdt", name);
	mdt = read_file(path, &mdt_size);
	if (!mdt)
		return NULL;

	if (mdt_size < 64 || memcmp(mdt, "\177ELF", 4)) {
		fprintf(stderr, "%s: not an ELF image\n", path);
		free(mdt);
		return NULL;
	}

	/* Both ELF32 and ELF64 .mdt are supported. */
	if (mdt[4] == 1) {
		phnum = *(uint16_t *)(mdt + 44);
	} else {
		phnum = *(uint16_t *)(mdt + 56);
	}

	total = mdt_size;
	for (i = 0; i < phnum; i++) {
		size_t sz;
		void *seg;

		snprintf(path, sizeof(path), "/lib/firmware/%s.b%02u", name, i);
		seg = read_file(path, &sz);
		if (!seg) {
			free(mdt);
			return NULL;
		}
		free(seg);
		total += sz;
	}

	img = malloc(total);
	if (!img) {
		free(mdt);
		return NULL;
	}
	memcpy(img, mdt, mdt_size);
	off = mdt_size;
	for (i = 0; i < phnum; i++) {
		size_t sz;
		void *seg;

		snprintf(path, sizeof(path), "/lib/firmware/%s.b%02u", name, i);
		seg = read_file(path, &sz);
		if (!seg) {
			free(img);
			free(mdt);
			return NULL;
		}
		memcpy(img + off, seg, sz);
		off += sz;
		free(seg);
	}
	free(mdt);
	*out_size = total;
	return img;
}

int main(int argc, char **argv)
{
	const char *app = argc > 1 ? argv[1] : "uff_jv";
	struct tee_ioctl_param p[4];
	uint64_t client_env = 0, app_loader = 0, app_ctl = 0;
	uint32_t uid = QSEECOM_COMPAT_APP_LOADER_UID;
	uint32_t ret = 0;
	char dist_name[64] = {0};
	size_t img_size;
	void *img;
	int fd, rc;

	fd = open("/dev/tee0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "/dev/tee0: %s\n", strerror(errno));
		return 1;
	}

	/* 1. root -> client env */
	memset(p, 0, sizeof(p));
	p[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT;
	p[0].a = TEE_OBJREF_NULL;
	p[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT;
	rc = tee_object_invoke(fd, TEE_OBJREF_NULL,
			       QCOMTEE_ROOT_OP_REG_WITH_CREDENTIALS, p, 2, &ret);
	printf("client env: rc=%d ret=%u id=0x%llx flags=0x%llx\n",
	       rc, ret, (unsigned long long)p[1].a,
	       (unsigned long long)p[1].b);
	if (rc < 0 || ret)
		goto out;
	client_env = p[1].a;

	/* 2. client env -> app loader (UID 122) */
	memset(p, 0, sizeof(p));
	p[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT;
	p[0].a = (uintptr_t)&uid;
	p[0].b = sizeof(uid);
	p[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT;
	rc = tee_object_invoke(fd, client_env, QCOMTEE_CLIENT_ENV_OPEN, p, 2, &ret);
	printf("app loader: rc=%d ret=%u id=0x%llx flags=0x%llx\n",
	       rc, ret, (unsigned long long)p[1].a,
	       (unsigned long long)p[1].b);
	if (rc < 0 || ret)
		goto out;
	app_loader = p[1].a;

	img = load_ta_image(app, &img_size);
	if (!img)
		goto out;
	printf("TA image: %zu bytes\n", img_size);

	/*
	 * 3. app loader -> app controller.  The 9 MB ELF cannot go through a
	 * UBUF (qcomtee copies it into the object message); put it in a TEE
	 * SHM and pass that as a memory object to loadFromRegion.
	 */
	{
		struct tee_ioctl_shm_alloc_data shm = {
			.size = img_size,
			.flags = 0,
		};
		int shm_fd = ioctl(fd, TEE_IOC_SHM_ALLOC, &shm);
		void *shm_va;

		if (shm_fd < 0) {
			fprintf(stderr, "TEE_IOC_SHM_ALLOC: %s\n",
				strerror(errno));
			free(img);
			goto out;
		}

		shm_va = mmap(NULL, img_size, PROT_READ | PROT_WRITE,
			      MAP_SHARED, shm_fd, 0);
		if (shm_va == MAP_FAILED) {
			fprintf(stderr, "mmap shm: %s\n", strerror(errno));
			close(shm_fd);
			free(img);
			goto out;
		}
		memcpy(shm_va, img, img_size);

		memset(p, 0, sizeof(p));
		p[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_UBUF_INPUT;
		p[0].a = (uintptr_t)app;
		p[0].b = strlen(app);
		p[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_INPUT;
		p[1].a = shm.id;
		p[1].b = QCOMTEE_OBJREF_FLAG_MEM;
		p[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_OBJREF_OUTPUT;
		rc = tee_object_invoke(fd, app_loader,
				       QSEECOM_COMPAT_APPLOADER_OP_LOAD_FROM_REGION,
				       p, 3, &ret);
		printf("loadFromRegion: rc=%d ret=%u ctl=0x%llx ctlflags=0x%llx\n",
		       rc, ret, (unsigned long long)p[2].a,
		       (unsigned long long)p[2].b);
		if (rc == 0 && !ret)
			app_ctl = p[2].a;

		munmap(shm_va, img_size);
		close(shm_fd);
	}

	free(img);
out:
	if (app_ctl)
		printf("SUCCESS: app controller object 0x%llx\n",
		       (unsigned long long)app_ctl);
	close(fd);
	return 0;
}
