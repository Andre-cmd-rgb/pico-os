/*
 * The commands: ac compiles, a compiles and runs (or runs an executable),
 * and the loader runs an executable by name.
 */
#include <string.h>

#include "al.h"
#include "driver.h"
#include "port.h"

static int read_file(const char *path, uint8_t **out, size_t *len, size_t max)
{
	int fd = port_open(path, PORT_O_READ);
	size_t cap = 4096, n = 0;
	uint8_t *buf;

	if (fd < 0)
		return fd;
	if (!(buf = port_alloc(cap))) {
		port_close(fd);
		return -12;
	}
	for (;;) {
		if (n == cap) {
			uint8_t *bigger = cap > max ? NULL : port_realloc(buf, cap * 2);
			if (!bigger) {
				port_free(buf);
				port_close(fd);
				return cap > max ? -27 : -12;	/* EFBIG, ENOMEM */
			}
			buf = bigger;
			cap *= 2;
		}
		long r = port_read(fd, buf + n, cap - n);
		if (r < 0) {
			port_free(buf);
			port_close(fd);
			return r;
		}
		if (r == 0)
			break;
		n += r;
	}
	port_close(fd);
	if (n > max) {
		port_free(buf);
		return -27;
	}
	*out = buf;
	*len = n;
	return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len)
{
	int fd = port_open(path, PORT_O_WRITE);

	if (fd < 0)
		return fd;
	while (len) {
		long r = port_write(fd, data, len);
		if (r <= 0) {
			port_close(fd);
			return r ? (int)r : -5;
		}
		data += r;
		len -= r;
	}
	return port_close(fd);
}

static bool is_image(const uint8_t *data, size_t len)
{
	return len >= 3 && !memcmp(data, AL_MAGIC, 3);
}

/* Takes ownership of data. */
static int run_image(const char *name, uint8_t *data, size_t len, int argc, char **argv)
{
	struct al_vm *vm = al_vm_new();
	char err[200];

	if (!vm) {
		port_free(data);
		al_eprintf("%s: out of memory\n", name);
		return 1;
	}
	vm->prog.image = data;
	vm->prog.image_len = len;
	if (al_load(vm, data, len, err, sizeof(err))) {
		al_eprintf("%s: %s\n", name, err);
		al_vm_free(vm, false);
		return 1;
	}
	const char *plain = port_getenv("AL_NOQUICKEN");	/* for the tests */
	if (!plain || !*plain)
		al_quicken(vm);
	int status = al_vm_run(vm, argc, argv);
	const char *check = port_getenv("AL_LEAKCHECK");
	al_vm_free(vm, check && *check && !vm->failed && !vm->halted);
	return status;
}

static int compile_file(const char *prog, const char *path, struct al_image *img)
{
	uint8_t *src;
	size_t len;
	int err = read_file(path, &src, &len, AL_MAX_SOURCE);

	if (err) {
		al_eprintf("%s: %s: %s\n", prog, path, port_strerror(err));
		return -1;
	}
	err = al_compile(path, (const char *)src, len, img);
	port_free(src);
	return err;
}

int al_main_ac(int argc, char **argv)
{
	const char *out = NULL, *src = NULL;
	bool disasm = false;
	char name[256];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			out = argv[++i];
		} else if (!strcmp(argv[i], "-d")) {
			disasm = true;
		} else if (argv[i][0] == '-' || src) {
			src = NULL;
			break;
		} else {
			src = argv[i];
		}
	}
	if (!src) {
		al_eprintf("usage: ac [-o program] [-d] file.al\n");
		return 2;
	}
	size_t len = strlen(src);
	if (len < 4 || strcmp(src + len - 3, ".al")) {
		al_eprintf("ac: %s: source files end in .al\n", src);
		return 2;
	}
	if (!out) {
		const char *base = strrchr(src, '/');
		base = base ? base + 1 : src;
		size_t n = strlen(base) - 3;
		if (n >= sizeof(name) || n == 0) {
			al_eprintf("ac: %s: bad file name\n", src);
			return 2;
		}
		memcpy(name, base, n);
		name[n] = '\0';
		out = name;
	}
	if (!strcmp(out, src)) {
		al_eprintf("ac: the output would overwrite %s\n", src);
		return 2;
	}

	struct al_image img;
	if (compile_file("ac", src, &img))
		return 1;
	if (disasm) {
		struct al_vm *vm = al_vm_new();
		char err[200];
		int status = 0;
		if (!vm) {
			port_free(img.data);
			return 1;
		}
		vm->prog.image = img.data;
		if (al_load(vm, img.data, img.len, err, sizeof(err))) {
			al_eprintf("ac: %s: %s\n", src, err);
			status = 1;
		} else {
			al_disasm(vm);
		}
		al_vm_free(vm, false);
		return status;
	}
	int err = write_file(out, img.data, img.len);
	port_free(img.data);
	if (err) {
		al_eprintf("ac: %s: %s\n", out, port_strerror(err));
		return 1;
	}
	return 0;
}

int al_main_a(int argc, char **argv)
{
	uint8_t *data;
	size_t len;

	if (argc < 2 || argv[1][0] == '-') {
		al_eprintf("usage: a file.al [args...]    compile and run\n"
			   "       a program [args...]    run a compiled program\n");
		return 2;
	}
	int err = read_file(argv[1], &data, &len, AL_MAX_IMAGE);
	if (err) {
		al_eprintf("a: %s: %s\n", argv[1], port_strerror(err));
		return 1;
	}
	if (is_image(data, len))
		return run_image(argv[1], data, len, argc - 1, argv + 1);

	struct al_image img;
	if (len > AL_MAX_SOURCE) {
		port_free(data);
		al_eprintf("a: %s: source file too large\n", argv[1]);
		return 1;
	}
	err = al_compile(argv[1], (const char *)data, len, &img);
	port_free(data);
	if (err)
		return 1;
	return run_image(argv[1], img.data, img.len, argc - 1, argv + 1);
}

bool al_probe(const uint8_t *head, size_t n)
{
	return is_image(head, n);
}

int al_exec(const char *path, int argc, char **argv)
{
	uint8_t *data;
	size_t len;
	int err = read_file(path, &data, &len, AL_MAX_IMAGE);

	if (err) {
		al_eprintf("%s: %s\n", path, port_strerror(err));
		return 1;
	}
	return run_image(argc ? argv[0] : path, data, len, argc, argv);
}
