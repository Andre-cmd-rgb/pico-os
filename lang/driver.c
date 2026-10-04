/*
 * The commands: picoc compiles, pico compiles and runs (or runs an
 * executable), and the loader runs an executable by name.
 */
#include <string.h>

#include "pico.h"
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
	return len >= 3 && !memcmp(data, PICO_MAGIC, 3);
}

/* Takes ownership of data. */
static int run_image(const char *name, uint8_t *data, size_t len, int argc, char **argv)
{
	struct pico_vm *vm = pico_vm_new();
	char err[200];

	if (!vm) {
		port_free(data);
		pico_eprintf("%s: out of memory\n", name);
		return 1;
	}
	vm->prog.image = data;
	vm->prog.image_len = len;
	if (pico_load(vm, data, len, err, sizeof(err))) {
		pico_eprintf("%s: %s\n", name, err);
		pico_vm_free(vm, false);
		return 1;
	}
	const char *plain = port_getenv("PICO_NOQUICKEN");	/* for the tests */
	if (!plain || !*plain)
		pico_quicken(vm);
	int status = pico_vm_run(vm, argc, argv);
	const char *check = port_getenv("PICO_LEAKCHECK");
	pico_vm_free(vm, check && *check && !vm->failed && !vm->halted);
	return status;
}

static int compile_file(const char *prog, const char *path, struct pico_image *img)
{
	uint8_t *src;
	size_t len;
	int err = read_file(path, &src, &len, PICO_MAX_SOURCE);

	if (err) {
		pico_eprintf("%s: %s: %s\n", prog, path, port_strerror(err));
		return -1;
	}
	err = pico_compile(path, (const char *)src, len, img);
	port_free(src);
	return err;
}

/*
 * The length of a source file's extension: .pico, or .al from when the
 * language was called a; 0 if it has neither.
 */
static size_t source_ext(const char *path)
{
	static const char *const exts[] = { ".pico", ".al" };
	size_t len = strlen(path);

	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		size_t n = strlen(exts[i]);

		if (len > n && !strcmp(path + len - n, exts[i]))
			return n;
	}
	return 0;
}

/*
 * Whether this system would run a program compiled elsewhere -- by picoc
 * on a PC, as the packages are: loaded and verified as running it would
 * (the version, the checksum, every instruction and built-in), not run.
 */
static int test_image(const char *path)
{
	struct pico_vm *vm;
	uint8_t *data;
	size_t len;
	char err[200];
	int status = read_file(path, &data, &len, PICO_MAX_IMAGE);

	if (status) {
		pico_eprintf("picoc: %s: %s\n", path, port_strerror(status));
		return 1;
	}
	if (!is_image(data, len)) {
		port_free(data);
		pico_eprintf("picoc: %s: not a compiled program\n", path);
		return 1;
	}
	if (!(vm = pico_vm_new())) {
		port_free(data);
		pico_eprintf("picoc: out of memory\n");
		return 1;
	}
	vm->prog.image = data;		/* the VM's now, freed with it */
	vm->prog.image_len = len;
	status = pico_load(vm, data, len, err, sizeof(err)) != 0;
	if (status)
		pico_eprintf("picoc: %s: %s\n", path, err);
	pico_vm_free(vm, false);
	return status;
}

int pico_main_compile(int argc, char **argv)
{
	const char *out = NULL, *src = NULL;
	bool disasm = false, test = false;
	char name[256];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			out = argv[++i];
		} else if (!strcmp(argv[i], "-d")) {
			disasm = true;
		} else if (!strcmp(argv[i], "-t")) {
			test = true;
		} else if (argv[i][0] == '-' || src) {
			src = NULL;
			break;
		} else {
			src = argv[i];
		}
	}
	if (!src || (test && (out || disasm))) {
		pico_eprintf("usage: picoc [-o program] [-d] file.pico\n"
			     "       picoc -t program\n");
		return 2;
	}
	if (test)
		return test_image(src);
	size_t ext = source_ext(src);
	if (!ext) {
		pico_eprintf("picoc: %s: source files end in .pico\n", src);
		return 2;
	}
	if (!out) {
		const char *base = strrchr(src, '/');
		base = base ? base + 1 : src;
		size_t n = strlen(base) - ext;
		if (n >= sizeof(name) || n == 0) {
			pico_eprintf("picoc: %s: bad file name\n", src);
			return 2;
		}
		memcpy(name, base, n);
		name[n] = '\0';
		out = name;
	}
	if (!strcmp(out, src)) {
		pico_eprintf("picoc: the output would overwrite %s\n", src);
		return 2;
	}

	struct pico_image img;
	if (compile_file("picoc", src, &img))
		return 1;
	if (disasm) {
		struct pico_vm *vm = pico_vm_new();
		char err[200];
		int status = 0;
		if (!vm) {
			port_free(img.data);
			return 1;
		}
		vm->prog.image = img.data;
		if (pico_load(vm, img.data, img.len, err, sizeof(err))) {
			pico_eprintf("picoc: %s: %s\n", src, err);
			status = 1;
		} else {
			pico_disasm(vm);
		}
		pico_vm_free(vm, false);
		return status;
	}
	int err = write_file(out, img.data, img.len);
	port_free(img.data);
	if (err) {
		pico_eprintf("picoc: %s: %s\n", out, port_strerror(err));
		return 1;
	}
	return 0;
}

int pico_main_run(int argc, char **argv)
{
	uint8_t *data;
	size_t len;

	if (argc < 2 || argv[1][0] == '-') {
		pico_eprintf("usage: pico file.pico [args...]    compile and run\n"
			   "       pico program [args...]      run a compiled program\n");
		return 2;
	}
	int err = read_file(argv[1], &data, &len, PICO_MAX_IMAGE);
	if (err) {
		pico_eprintf("pico: %s: %s\n", argv[1], port_strerror(err));
		return 1;
	}
	if (is_image(data, len))
		return run_image(argv[1], data, len, argc - 1, argv + 1);

	struct pico_image img;
	if (len > PICO_MAX_SOURCE) {
		port_free(data);
		pico_eprintf("pico: %s: source file too large\n", argv[1]);
		return 1;
	}
	err = pico_compile(argv[1], (const char *)data, len, &img);
	port_free(data);
	if (err)
		return 1;
	return run_image(argv[1], img.data, img.len, argc - 1, argv + 1);
}

bool pico_probe(const uint8_t *head, size_t n)
{
	return is_image(head, n);
}

int pico_exec(const char *path, int argc, char **argv)
{
	uint8_t *data;
	size_t len;
	int err = read_file(path, &data, &len, PICO_MAX_IMAGE);

	if (err) {
		pico_eprintf("%s: %s\n", path, port_strerror(err));
		return 1;
	}
	return run_image(argc ? argv[0] : path, data, len, argc, argv);
}
