/* Actual pkg.c, POSIX scratch files, and faults at its filesystem/PSA boundary. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../bin/pkg.c"

static char test_home[PT_PATH_MAX];
static bool missing_home;
static int short_write, write_error, close_error, rename_error, unlink_error;
static int printf_short, hash_update_error, hash_finish_short, compiler_error;
static int check_error, compiles, checks;
static bool check_interrupted;
static char checked[64];
static const char *read_error_path;
static bool writable[1024], read_error_fd[1024];
static int hash_aborts;
static int current_pid = 1, living_pid = 1, living_group;

int pt_getpid(void) { return current_pid; }
const char *pt_getenv(const char *name) { return !strcmp(name, "HOME") && !missing_home ? test_home : NULL; }
bool proc_alive(int pid) { return pid == living_pid; }
bool proc_group_alive(int pgid) { return pgid == living_group; }

size_t strlcpy(char *out, const char *src, size_t n)
{
	size_t len = strlen(src);

	if (n) {
		size_t copy = len < n - 1 ? len : n - 1;

		memcpy(out, src, copy);
		out[copy] = '\0';
	}
	return len;
}

void program_register(struct pt_program *p) { (void)p; }
void completion_register(struct pt_completion *p) { (void)p; }
const struct pt_program *program_first(void) { return NULL; }
void *pt_calloc(size_t n, size_t size) { return calloc(n, size); }
void pt_free(void *p) { free(p); }
void pt_sigcatch(bool on) { (void)on; }
int pt_kill(int pid, int sig) { (void)pid; (void)sig; return 0; }
int pt_ioctl(int fd, int req, void *arg) { (void)fd; (void)req; (void)arg; return -ENOTTY; }
int pt_mkdir(const char *path) { return mkdir(path, 0700) ? -errno : 0; }
int pt_unlink(const char *path)
{
	if (unlink_error)
		return -unlink_error;
	return unlink(path) ? -errno : 0;
}
int pt_rename(const char *from, const char *to)
{
	if (rename_error)
		return -rename_error;
	return rename(from, to) ? -errno : 0;
}
int pt_open(const char *path, int flags)
{
	int fd = open(path, flags, 0600);

	if (fd < 0)
		return -errno;
	assert(fd < 1024);
	writable[fd] = (flags & O_ACCMODE) != O_RDONLY;
	read_error_fd[fd] = read_error_path && strstr(path, read_error_path);
	return fd;
}
ssize_t pt_read(int fd, void *buf, size_t n)
{
	ssize_t got;

	if (read_error_fd[fd])
		return -EIO;
	got = read(fd, buf, n);
	return got < 0 ? -errno : got;
}
ssize_t pt_write(int fd, const void *buf, size_t n)
{
	ssize_t wrote;

	if (write_error)
		return -write_error;
	if (short_write && n > (size_t)short_write)
		n = short_write;
	wrote = write(fd, buf, n);
	return wrote < 0 ? -errno : wrote;
}
int pt_close(int fd)
{
	int err = close(fd) ? -errno : 0;

	if (!err && writable[fd] && close_error)
		err = -close_error;
	writable[fd] = read_error_fd[fd] = false;
	return err;
}
int pt_dprintf(int fd, const char *fmt, ...)
{
	char text[512];
	va_list ap;
	int n;
	size_t done = 0;

	va_start(ap, fmt);
	n = vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	assert(n >= 0 && n < (int)sizeof(text));
	if (printf_short)
		n--;
	while (done < (size_t)n) {
		ssize_t w = pt_write(fd, text + done, n - done);

		if (w <= 0)
			return w < 0 ? (int)w : -EIO;
		done += w;
	}
	return n;
}
int pt_printf(const char *fmt, ...) { (void)fmt; return 0; }
int fail(const char *prog, const char *what, int err)
{
	(void)prog; (void)what;
	assert(err < 0);
	return 1;
}
int utf8_width(const char *s, size_t n) { (void)s; return n; }
size_t utf8_prefix(const char *s, size_t n, int cols)
{
	(void)s;
	return cols < 0 ? 0 : n < (size_t)cols ? n : (size_t)cols;
}
int pt_home_file(const char *dir, const char *name, const char *old, char *out, size_t size)
{
	char base[PT_PATH_MAX];

	(void)old;
	if ((size_t)snprintf(base, sizeof(base), "%s/%s", test_home, dir) >= sizeof(base))
		return -ENAMETOOLONG;
	pt_mkdir(base);
	return (size_t)snprintf(out, size, "%s/%s", base, name) < size ? 0 : -ENAMETOOLONG;
}

/* The cryptographic primitive is a controllable shim, not a SHA-256 test. */
int psa_crypto_init(void) { return PSA_SUCCESS; }
int psa_hash_setup(psa_hash_operation_t *op, int alg)
{
	assert(alg == PSA_ALG_SHA_256);
	op->sum = 0;
	return PSA_SUCCESS;
}
int psa_hash_update(psa_hash_operation_t *op, const uint8_t *buf, size_t n)
{
	if (hash_update_error)
		return -1;
	for (size_t i = 0; i < n; i++)
		op->sum = op->sum * 31 + buf[i];
	return PSA_SUCCESS;
}
int psa_hash_finish(psa_hash_operation_t *op, uint8_t *sum, size_t size, size_t *len)
{
	assert(size == 32);
	for (size_t i = 0; i < size; i++)
		sum[i] = (op->sum >> ((i % 4) * 8)) & 255;
	*len = hash_finish_short ? 31 : 32;
	return PSA_SUCCESS;
}
int psa_hash_abort(psa_hash_operation_t *op) { (void)op; hash_aborts++; return PSA_SUCCESS; }

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	assert(f && fputs(text, f) >= 0 && !fclose(f));
}
static void read_text(const char *path, char *data, size_t size)
{
	FILE *f = fopen(path, "r");
	size_t n;

	assert(f);
	n = fread(data, 1, size - 1, f);
	assert(!ferror(f) && !fclose(f));
	data[n] = '\0';
}
static void expect_text(const char *path, const char *text)
{
	char data[2048];

	read_text(path, data, sizeof(data));
	assert(!strcmp(data, text));
}
static void absent(const char *path) { assert(access(path, F_OK) && errno == ENOENT); }
static void test_path(char *out, size_t n, const char *base, const char *tail)
{
	assert((size_t)snprintf(out, n, "%s/%s", base, tail) < n);
}

/* picoc -o compiles, as pid 123; picoc -t, quiet, checks the program it is given, as 124. */
int pt_spawn(const struct pt_spawn *req)
{
	assert(req->pgid == current_pid && !strcmp(req->cmd, "picoc"));
	if (req->argc == 3) {
		assert(!strcmp(req->argv[1], "-t") && req->fd[1] >= 0 && req->fd[2] >= 0);
		read_text(req->argv[2], checked, sizeof(checked));
		checks++;
		return 124;
	}
	assert(req->argc == 4 && strstr(req->argv[3], ".pico") && req->fd[1] < 0 && req->fd[2] < 0);
	write_text(req->argv[2], compiler_error ? "partial image" : "new image");
	compiles++;
	return 123;
}
int pt_wait(int pid, int *status, int flags)
{
	(void)flags;
	assert(pid == 123 || pid == 124);
	if (pid == 124 && check_interrupted) {
		check_interrupted = false;	/* Ctrl-C: passed on, then waited for again */
		return -EINTR;
	}
	*status = pid == 123 ? compiler_error : check_error;
	return pid;
}

static void entries(void)
{
	struct entry entry[8];
	struct pkgs p = { .e = entry };
	const char *names[] = { "../notes", "a/b", "/tmp/x", "UPPER", "-option",
				"", "abcdefghijklmnopqrstuvwxyz", "a b", NULL };
	char line[256];
	const char *sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	for (int i = 0; names[i]; i++) {
		snprintf(line, sizeof(line), "%s\t1.0\t20\t%s\tabout", names[i], sha);
		index_line(line, &p);
		assert(!p.n);
		snprintf(line, sizeof(line), "%s 1.0", names[i]);
		installed_line(line, &p);
		assert(!p.n);
	}
	snprintf(line, sizeof(line), "abcdefghijklmnopqrstuvwx\t1.0\t20\t%s\tabout", sha);
	index_line(line, &p);
	assert(p.n == 1 && !strcmp(entry[0].name, "abcdefghijklmnopqrstuvwx"));
	const char *sizes[] = { "-1", "12garbage", "9999999999999999999999999", "", NULL };

	for (int i = 0; sizes[i]; i++) {
		snprintf(line, sizeof(line), "snake\t1.0\t%s\t%s\tabout", sizes[i], sha);
		index_line(line, &p);
		assert(p.n == 1);
	}
	strcpy(line, "snake\t1.0\t20\tnot-a-hash\tabout");
	index_line(line, &p);
	assert(p.n == 1);
	snprintf(line, sizeof(line), "snake\t1.0 bad\t20\t%s\tabout", sha);
	index_line(line, &p);
	assert(p.n == 1);

	/* images.txt: only snake's line for this very source, and the first of those */
	const char *other = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	const char *images[] = { "# snake\t%s\t20\t%s", "guess\t%s\t20\t%s", "snake\t%.63s\t20\t%s",
				 "snake\t%s\t-1\t%s", "snake\t%s\t20\t%.63s", "snake\t%s\t20\t%s\tmore",
				 "snake\t%s\t20", "snake\t%s\t\t%s", NULL };
	struct image im = { .name = "snake", .source = sha };

	for (int i = 0; images[i]; i++) {
		snprintf(line, sizeof(line), images[i], sha, other);
		image_line(line, &im);
		assert(!im.found);
	}
	snprintf(line, sizeof(line), "snake\t%s\t20\t%s", other, other);
	image_line(line, &im);
	assert(!im.found);
	snprintf(line, sizeof(line), "snake\t%s\t20\t%s", sha, other);
	image_line(line, &im);
	assert(im.found && im.size == 20 && !strcmp(im.sha, other));
	snprintf(line, sizeof(line), "snake\t%s\t30\t%s", sha, sha);
	image_line(line, &im);
	assert(im.size == 20 && !strcmp(im.sha, other));
}

static void io_failures(void)
{
	struct entry e = { .name = "snake", .version = "2.0" };
	struct pkgs p = { .e = &e, .n = 1 }, loaded = { 0 };
	char path[PT_PATH_MAX], temp[PT_PATH_MAX + 8], from[PT_PATH_MAX], to[PT_PATH_MAX];
	char sha[65], line[300];
	long size;
	int aborts;

	assert(pkg_path("installed", path, sizeof(path)));
	snprintf(temp, sizeof(temp), "%s.new", path);
	write_text(path, "snake 1.0\n");
	write_error = ENOSPC;
	assert(save_installed(&p) == -ENOSPC);
	write_error = 0;
	expect_text(path, "snake 1.0\n"); absent(temp);
	close_error = ENOSPC;
	assert(save_installed(&p) == -ENOSPC);
	close_error = 0;
	expect_text(path, "snake 1.0\n"); absent(temp);
	printf_short = 1;
	assert(save_installed(&p) == -EIO);
	printf_short = 0;
	expect_text(path, "snake 1.0\n"); absent(temp);
	rename_error = EIO;
	assert(save_installed(&p) == -EIO);
	rename_error = 0;
	expect_text(path, "snake 1.0\n"); absent(temp);
	short_write = 2;
	assert(!save_installed(&p));
	short_write = 0;
	expect_text(path, "snake 2.0\n");
	read_error_path = "installed";
	assert(load(&loaded, "installed", installed_line) == -EIO);
	read_error_path = NULL;
	memset(line, 'x', sizeof(line) - 1); line[sizeof(line) - 1] = '\0';
	write_text(path, line);
	assert(load(&loaded, "installed", installed_line) == -EOVERFLOW);
	pt_free(loaded.e);

	test_path(from, sizeof(from), test_home, "from");
	test_path(to, sizeof(to), test_home, "to");
	write_text(from, "the full source file");
	short_write = 3;
	assert(!copy(from, to));
	short_write = 0;
	expect_text(to, "the full source file");
	close_error = ENOSPC;
	assert(copy(from, to) == -ENOSPC);
	close_error = 0;
	assert(sha256_of(from, sha, &size) && size == 20);
	aborts = hash_aborts;
	read_error_path = "/from";
	assert(!sha256_of(from, sha, &size));
	read_error_path = NULL;
	hash_update_error = 1;
	assert(!sha256_of(from, sha, &size));
	hash_update_error = 0;
	hash_finish_short = 1;
	assert(!sha256_of(from, sha, &size));
	hash_finish_short = 0;
	assert(hash_aborts == aborts + 3);
}

static void installation(const char *root)
{
	char repository[PT_PATH_MAX], path[PT_PATH_MAX], source[PT_PATH_MAX];
	char saved_source[PT_PATH_MAX], binary[PT_PATH_MAX], temp_image[PT_PATH_MAX + 8];
	char temp_source[PT_PATH_MAX], db[PT_PATH_MAX], index[PT_PATH_MAX], line[256];
	struct entry e = { .name = "snake", .version = "2.0" };
	struct pkgs idx = { .e = &e, .n = 1 }, inst = { 0 }, fetched = { 0 };

	inst.e = pt_calloc(ENTRIES_MAX, sizeof(*inst.e));
	assert(inst.e);
	inst.e[0] = e; strcpy(inst.e[0].version, "1.0"); inst.n = 1;
	test_path(repository, sizeof(repository), root, "repository");
	assert(!pt_mkdir(repository));
	test_path(path, sizeof(path), repository, "packages"); assert(!pt_mkdir(path));
	test_path(path, sizeof(path), repository, "packages/snake"); assert(!pt_mkdir(path));
	test_path(source, sizeof(source), repository, "packages/snake/snake.pico");
	write_text(source, "println(\"new source\");\n");
	assert(sha256_of(source, e.sha, &e.size));
	assert(pkg_path("repo", path, sizeof(path))); write_text(path, repository);
	assert(pkg_path("src", path, sizeof(path))); assert(!pt_mkdir(path));
	test_path(saved_source, sizeof(saved_source), path, "snake.pico");
	test_path(temp_source, sizeof(temp_source), path, "snake.new.pico");
	write_text(saved_source, "old source");
	test_path(path, sizeof(path), test_home, "bin");
	assert(!pt_mkdir(path));
	test_path(binary, sizeof(binary), path, "snake");
	snprintf(temp_image, sizeof(temp_image), "%s.new", binary);
	write_text(binary, "old image");
	assert(pkg_path("installed", db, sizeof(db))); write_text(db, "snake 1.0\n");
	compiler_error = 1;
	assert(install(&idx, &inst, "snake"));
	compiler_error = 0;
	expect_text(saved_source, "old source"); expect_text(binary, "old image");
	expect_text(db, "snake 1.0\n"); absent(temp_image); absent(temp_source);
	assert(!install(&idx, &inst, "snake"));
	expect_text(saved_source, "println(\"new source\");\n"); expect_text(binary, "new image");
	expect_text(db, "snake 2.0\n"); absent(temp_image); absent(temp_source);
	unlink_error = EACCES;
	assert(uninstall(&inst, "snake") && inst.n == 1);
	unlink_error = 0;
	expect_text(binary, "new image"); expect_text(db, "snake 2.0\n");

	assert(pkg_path("index.txt", index, sizeof(index))); write_text(index, "old index\n");
	test_path(path, sizeof(path), repository, "index.txt");
	snprintf(line, sizeof(line), "snake\t2.0\t%ld\t%s\tabout\n", e.size, e.sha);
	write_text(path, line);
	close_error = ENOSPC;
	assert(update(&fetched));
	close_error = 0;
	expect_text(index, "old index\n");
	read_error_path = "index.txt.new";
	assert(update(&fetched));
	read_error_path = NULL;
	expect_text(index, "old index\n");
	assert(!update(&fetched) && fetched.n == 1);
	expect_text(index, line);
	pt_free(fetched.e); pt_free(inst.e);
}

/* The repository's images.txt (NULL: none), taken by pkg update. */
static void publish_images(const char *repository, const char *text)
{
	char path[PT_PATH_MAX];
	struct pkgs fetched = { 0 };

	test_path(path, sizeof(path), repository, "images.txt");
	if (text)
		write_text(path, text);
	else
		unlink(path);
	assert(!update(&fetched));
	pt_free(fetched.e);
}

/* snake installed: the program it leaves, how many picoc -t and -o, and no temporary files. */
static void installs(struct pkgs *idx, int status, const char *program, int new_checks,
		     int new_compiles)
{
	char binary[PT_PATH_MAX], temp_image[PT_PATH_MAX + 8], dir[PT_PATH_MAX];
	char temp_source[PT_PATH_MAX];
	struct pkgs inst = { 0 };
	int were_checks = checks, were_compiles = compiles;

	inst.e = pt_calloc(ENTRIES_MAX, sizeof(*inst.e));
	assert(inst.e);
	test_path(binary, sizeof(binary), test_home, "bin/snake");
	snprintf(temp_image, sizeof(temp_image), "%s.new", binary);
	assert(pkg_path("src", dir, sizeof(dir)));
	test_path(temp_source, sizeof(temp_source), dir, "snake.new.pico");
	write_text(binary, "old image");
	assert(install(idx, &inst, "snake") == status);
	expect_text(binary, program);
	assert(checks == were_checks + new_checks && compiles == were_compiles + new_compiles);
	absent(temp_image); absent(temp_source);
	pt_free(inst.e);
}

/* After installation(): its repository folder, now with programs compiled there. */
static void ready_made_programs(const char *root)
{
	char repository[PT_PATH_MAX], path[PT_PATH_MAX], kept[PT_PATH_MAX], aside[PT_PATH_MAX];
	char line[512], sha[65], wrong[65];
	struct entry e = { .name = "snake", .version = "3.0" };
	struct pkgs idx = { .e = &e, .n = 1 };
	long size;

	test_path(repository, sizeof(repository), root, "repository");
	test_path(path, sizeof(path), repository, "packages/snake/snake.pico");
	assert(sha256_of(path, e.sha, &e.size));
	test_path(path, sizeof(path), repository, "images"); assert(!pt_mkdir(path));
	test_path(path, sizeof(path), repository, "images/snake");
	write_text(path, "ready-made image");
	assert(sha256_of(path, sha, &size));
	strcpy(wrong, sha); wrong[0] = wrong[0] == '0' ? '1' : '0';
	assert(pkg_path("images.txt", kept, sizeof(kept)));

	/* none there: an older list goes, and the source is compiled */
	snprintf(line, sizeof(line), "snake\t%s\t%ld\t%s\n", e.sha, size, sha);
	write_text(kept, line);
	publish_images(repository, NULL);
	absent(kept);
	installs(&idx, 0, "new image", 0, 1);

	/* the program for this very source: checked by picoc -t, then installed as it came */
	snprintf(line, sizeof(line), "# name, source, size, sha256\nsnake\t%s\t%ld\t%s\n",
		 e.sha, size, sha);
	publish_images(repository, line);
	expect_text(kept, line);
	installs(&idx, 0, "ready-made image", 1, 0);
	assert(!strcmp(checked, "ready-made image"));

	/* only another source's program, or another package's for this source */
	snprintf(line, sizeof(line), "snake\t%s\t%ld\t%s\nguess\t%s\t%ld\t%s\n",
		 wrong, size, sha, e.sha, size, sha);
	publish_images(repository, line);
	installs(&idx, 0, "new image", 0, 1);

	/* a program that is not what the list says: another SHA-256, another size */
	snprintf(line, sizeof(line), "snake\t%s\t%ld\t%s\n", e.sha, size, wrong);
	publish_images(repository, line);
	installs(&idx, 0, "new image", 0, 1);
	snprintf(line, sizeof(line), "snake\t%s\t%ld\t%s\n", e.sha, size + 1, sha);
	publish_images(repository, line);
	installs(&idx, 0, "new image", 0, 1);

	/* listed, but it does not come */
	snprintf(line, sizeof(line), "snake\t%s\t%ld\t%s\n", e.sha, size, sha);
	publish_images(repository, line);
	test_path(path, sizeof(path), repository, "images/snake");
	test_path(aside, sizeof(aside), repository, "snake.aside");
	assert(!rename(path, aside));
	installs(&idx, 0, "new image", 0, 1);
	assert(!rename(aside, path));
	expect_text(kept, line);

	/* picoc -t refuses it: a newer compiler's, say */
	check_error = 1;
	installs(&idx, 0, "new image", 1, 1);
	check_error = 0;

	/* Ctrl-C while it is checked stops the install, rather than compiling instead */
	check_interrupted = true;
	check_error = 130;
	installs(&idx, 1, "old image", 1, 0);
	check_error = 0;
	installs(&idx, 0, "ready-made image", 1, 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	test_path(test_home, sizeof(test_home), argv[1], "home");
	assert(!pt_mkdir(test_home));
	char bin[PT_PATH_MAX];

	assert(!pkg_bin_dir(bin, sizeof(bin)) && strstr(bin, test_home) == bin);
	assert(pkg_bin_dir(bin, 2) == -ENAMETOOLONG);
	missing_home = true;
	assert(pkg_bin_dir(bin, sizeof(bin)) == -ENOENT);
	missing_home = false;
	assert(pkg_enter());
	current_pid = 2;
	assert(!pkg_enter());
	living_pid = 2;
	living_group = 1;
	assert(!pkg_enter());	/* killed owner still has a writing helper */
	living_group = 0;
	assert(pkg_enter());	/* previous owner was force-killed */
	current_pid = 1;
	pkg_leave();
	assert(atomic_load(&pkg_owner) == 2);
	current_pid = 2;
	pkg_leave();
	assert(!atomic_load(&pkg_owner));
	entries(); io_failures(); installation(argv[1]); ready_made_programs(argv[1]);
	puts("pkg: safe names, complete I/O, hash failures, saved DB, failed upgrades, orphan-helper leases, "
	     "ready-made programs and their fallbacks passed");
	return 0;
}
