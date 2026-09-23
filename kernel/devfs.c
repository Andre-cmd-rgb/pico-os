/*
 * /dev: null, zero and urandom, stdin stdout stderr and tty, plus whatever
 * the drivers add at boot (dev_register), the way a character driver
 * appears under Linux's /dev.
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

/*
 * Names for what the opening process already has, as on Linux: /dev/stdin
 * and the rest open the same file as its fd 0, 1 or 2 (sharing its place
 * in it), and /dev/tty its terminal, whichever of the three that is.
 */
static const struct {
	const char	*name;
	int		 fd;		/* -1: the terminal */
} aliases[] = {
	{ "stderr", 2 }, { "stdin", 0 }, { "stdout", 1 }, { "tty", -1 },
};

#define N_ALIASES (sizeof(aliases) / sizeof(aliases[0]))

static int alias_of(const char *entry)
{
	for (size_t i = 0; i < N_ALIASES; i++)
		if (!strcmp(entry, aliases[i].name))
			return (int)i;
	return -1;
}

static struct pt_file *alias_open(int which, int *err)
{
	struct proc *p = proc_current();
	struct pt_file *f = NULL;

	if (p && aliases[which].fd >= 0)
		f = p->fd[aliases[which].fd];
	for (int i = 0; p && aliases[which].fd < 0 && i < 3 && !f; i++)
		if (p->fd[i] && p->fd[i]->is_tty)
			f = p->fd[i];
	if (!f)
		*err = aliases[which].fd < 0 ? -ENXIO : -EBADF;
	return file_get(f);
}

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
	return device_ops(entry) != NULL || alias_of(entry) >= 0;
}

static struct pt_file *dev_open(const char *entry, int *err)
{
	if (alias_of(entry) >= 0)
		return alias_open(alias_of(entry), err);

	struct pt_file *f = file_alloc(device_ops(entry), NULL);

	if (!f)
		*err = -ENOMEM;
	return f;
}

static int dev_readdir(int index, struct pt_dirent *ent)
{
	if (index < 0 || index >= (int)(N_DEVICES + N_ALIASES) + nextra)
		return 0;
	strlcpy(ent->name, index < (int)N_DEVICES ? devices[index].name
		: index < (int)(N_DEVICES + N_ALIASES) ? aliases[index - N_DEVICES].name
		: extra[index - N_DEVICES - N_ALIASES].name, sizeof(ent->name));
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
