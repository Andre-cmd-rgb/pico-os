/*
 * /dev: null, zero and urandom, plus whatever the drivers add at boot
 * (dev_register), the way a character driver appears under Linux's /dev.
 */
#include <string.h>

#include "esp_random.h"

#include "pt/kernel.h"

static ssize_t null_read(struct pt_file *f, void *buf, size_t n)
{
	return 0;
}

static ssize_t zero_read(struct pt_file *f, void *buf, size_t n)
{
	memset(buf, 0, n);
	return n;
}

static ssize_t random_read(struct pt_file *f, void *buf, size_t n)
{
	esp_fill_random(buf, n);
	return n;
}

static ssize_t sink_write(struct pt_file *f, const void *buf, size_t n)
{
	return n;
}

static const struct {
	const char		  *name;
	const struct pt_file_ops   ops;
} devices[] = {
	{ "null", { .read = null_read, .write = sink_write } },
	{ "urandom", { .read = random_read, .write = sink_write } },
	{ "zero", { .read = zero_read, .write = sink_write } },
};

#define N_DEVICES (sizeof(devices) / sizeof(devices[0]))
#define N_EXTRA   4

static struct {
	const char		 *name;
	const struct pt_file_ops *ops;
} extra[N_EXTRA];

static int nextra;

int dev_register(const char *name, const struct pt_file_ops *ops)
{
	if (nextra == N_EXTRA)
		return -ENOSPC;
	extra[nextra].name = name;
	extra[nextra].ops = ops;
	nextra++;
	return 0;
}

/* Built-in devices first, then the ones a driver registered. */
static const struct pt_file_ops *device_ops(const char *entry)
{
	for (size_t i = 0; i < N_DEVICES; i++)
		if (!strcmp(entry, devices[i].name))
			return &devices[i].ops;
	for (int i = 0; i < nextra; i++)
		if (!strcmp(entry, extra[i].name))
			return extra[i].ops;
	return NULL;
}

static bool dev_lookup(const char *entry)
{
	return device_ops(entry) != NULL;
}

static struct pt_file *dev_open(const char *entry, int *err)
{
	struct pt_file *f = file_alloc(device_ops(entry), NULL);

	if (!f)
		*err = -ENOMEM;
	return f;
}

static int dev_readdir(int index, struct pt_dirent *ent)
{
	if (index < 0 || index >= (int)N_DEVICES + nextra)
		return 0;
	strlcpy(ent->name, index < (int)N_DEVICES ? devices[index].name
						  : extra[index - N_DEVICES].name,
		sizeof(ent->name));
	ent->is_dir = false;
	return 1;
}

const struct pt_kernfs devfs = {
	.name = "dev",
	.writable = true,
	.lookup = dev_lookup,
	.open = dev_open,
	.readdir = dev_readdir,
};
