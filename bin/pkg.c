/*
 * pkg: programs from the pico-os-packages repository.
 *
 *	pkg update           what there is, from the repository's index.txt
 *	pkg list             it, * for what is installed
 *	pkg install snake    into ~/bin: then just `snake`
 *	pkg upgrade          the installed ones that have a new version
 *	pkg remove snake
 *
 * A package is pico source. It is downloaded, checked against the
 * SHA-256 the index gives, and compiled here by picoc, so that it always
 * matches the language this board speaks; the source is kept in
 * ~/.config/pkg/src. ~/.config/pkg holds the index and what is installed,
 * and `repo` there can name another repository -- an address, or a
 * folder, which is how a package is tried before it is published.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "psa/crypto.h"

#include "pt/program.h"
#include "util.h"

#define REPO_DEFAULT	"https://raw.githubusercontent.com/Andre-cmd-rgb/pico-os-packages/main"
#define ENTRIES_MAX	256
#define LINE_MAX_PKG	256

struct entry {
	char	name[24];
	char	version[16];
	long	size;
	char	sha[65];
	char	about[96];
};

struct pkgs {
	struct entry	*e;
	int		 n;
};

/* ~/.config/pkg/NAME, the folders made on the way. */
static bool pkg_path(const char *name, char *out, size_t size)
{
	char dir[PT_PATH_MAX];

	if (pt_home_file(".config", "pkg", NULL, dir, sizeof(dir)))
		return false;
	pt_mkdir(dir);
	return (size_t)snprintf(out, size, "%s/%s", dir, name) < size;
}

static void repo(char *out, size_t size)
{
	char path[PT_PATH_MAX];
	int fd;
	ssize_t n;

	strlcpy(out, REPO_DEFAULT, size);
	if (!pkg_path("repo", path, sizeof(path)) || (fd = pt_open(path, O_RDONLY)) < 0)
		return;
	n = pt_read(fd, out, size - 1);
	pt_close(fd);
	if (n <= 0) {
		strlcpy(out, REPO_DEFAULT, size);
		return;
	}
	out[n] = '\0';
	out[strcspn(out, "\r\n")] = '\0';
	for (size_t len = strlen(out); len > 1 && out[len - 1] == '/'; len--)
		out[len - 1] = '\0';
}

/* A command run to its end; its exit status, or <0. Ctrl-C reaches it. */
static int run(char *const argv[])
{
	struct pt_spawn req = { .cmd = argv[0], .fd = { -1, -1, -1 } };
	int pid, status = 0;

	while (argv[req.argc])
		req.argc++;
	req.argv = argv;
	pt_sigcatch(true);
	if ((pid = pt_spawn(&req)) < 0) {
		pt_sigcatch(false);
		return pid;
	}
	while (pt_wait(pid, &status, false) == -EINTR) {
		pt_kill(pid, PT_SIGINT);
		pt_sigcatch(true);
	}
	pt_sigcatch(false);
	return status;
}

static int copy(const char *from, const char *to)
{
	char buf[1024];
	int in = pt_open(from, O_RDONLY), out, err = 0;
	ssize_t n;

	if (in < 0)
		return in;
	if ((out = pt_open(to, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		pt_close(in);
		return out;
	}
	while ((n = pt_read(in, buf, sizeof(buf))) > 0)
		if (pt_write(out, buf, n) != n) {
			err = -EIO;
			break;
		}
	if (n < 0)
		err = (int)n;
	pt_close(in);
	pt_close(out);
	return err;
}

/* The repository's file `rel` into `dest`: over the web with wget, or copied from a folder. */
static int fetch(const char *rel, const char *dest)
{
	char base[PT_PATH_MAX], url[PT_PATH_MAX * 2];
	char *argv[] = { "wget", "-q", "-O", (char *)dest, url, NULL };
	int st;

	repo(base, sizeof(base));
	snprintf(url, sizeof(url), "%s/%s", base, rel);
	if (base[0] == '/')
		return copy(url, dest);
	st = run(argv);
	return st > 0 ? -EIO : st;
}

static bool sha256_of(const char *path, char hex[65], long *size)
{
	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	uint8_t buf[512], sum[32];
	size_t len = 0;
	ssize_t n;
	int fd = pt_open(path, O_RDONLY);

	*size = 0;
	if (fd < 0 || psa_crypto_init() != PSA_SUCCESS ||
	    psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		if (fd >= 0)
			pt_close(fd);
		return false;
	}
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0) {
		psa_hash_update(&op, buf, n);
		*size += n;
	}
	pt_close(fd);
	if (psa_hash_finish(&op, sum, sizeof(sum), &len) != PSA_SUCCESS)
		return false;
	for (int i = 0; i < 32; i++)
		snprintf(hex + 2 * i, 3, "%02x", sum[i]);
	return true;
}

/* The lines of a file, each handed to fn. */
static void each_line(const char *path, void (*fn)(char *line, void *ctx), void *ctx)
{
	char buf[512], line[LINE_MAX_PKG];
	size_t len = 0;
	ssize_t n;
	int fd = pt_open(path, O_RDONLY);

	if (fd < 0)
		return;
	while ((n = pt_read(fd, buf, sizeof(buf))) > 0)
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != '\n') {
				if (len < sizeof(line) - 1)
					line[len++] = buf[i];
				continue;
			}
			line[len] = '\0';
			fn(line, ctx);
			len = 0;
		}
	if (len) {
		line[len] = '\0';
		fn(line, ctx);
	}
	pt_close(fd);
}

static void index_line(char *line, void *ctx)
{
	struct pkgs *p = ctx;
	char *f[5];
	int k = 0;

	if (line[0] == '#' || !line[0] || p->n == ENTRIES_MAX)
		return;
	for (char *s = line; k < 5; k++) {
		f[k] = s;
		if (k < 4) {
			s = strchr(s, '\t');
			if (!s)
				return;
			*s++ = '\0';
		}
	}
	strlcpy(p->e[p->n].name, f[0], sizeof(p->e[0].name));
	strlcpy(p->e[p->n].version, f[1], sizeof(p->e[0].version));
	p->e[p->n].size = atol(f[2]);
	strlcpy(p->e[p->n].sha, f[3], sizeof(p->e[0].sha));
	strlcpy(p->e[p->n].about, f[4], sizeof(p->e[0].about));
	p->n++;
}

/* Installed: "name version" lines, into the same entries. */
static void installed_line(char *line, void *ctx)
{
	struct pkgs *p = ctx;
	char *sp = strchr(line, ' ');

	if (!sp || p->n == ENTRIES_MAX)
		return;
	*sp = '\0';
	memset(&p->e[p->n], 0, sizeof(p->e[0]));
	strlcpy(p->e[p->n].name, line, sizeof(p->e[0].name));
	strlcpy(p->e[p->n].version, sp + 1, sizeof(p->e[0].version));
	p->n++;
}

static bool load(struct pkgs *p, const char *file, void (*fn)(char *, void *))
{
	char path[PT_PATH_MAX];

	p->n = 0;
	if (!p->e && !(p->e = pt_calloc(ENTRIES_MAX, sizeof(*p->e))))
		return false;
	if (pkg_path(file, path, sizeof(path)))
		each_line(path, fn, p);
	return true;
}

static struct entry *find(struct pkgs *p, const char *name)
{
	for (int i = 0; i < p->n; i++)
		if (!strcmp(p->e[i].name, name))
			return &p->e[i];
	return NULL;
}

static int save_installed(const struct pkgs *inst)
{
	char path[PT_PATH_MAX], tmp[PT_PATH_MAX + 8];
	int fd;

	if (!pkg_path("installed", path, sizeof(path)))
		return -ENAMETOOLONG;
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if ((fd = pt_open(tmp, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return fd;
	for (int i = 0; i < inst->n; i++)
		pt_dprintf(fd, "%s %s\n", inst->e[i].name, inst->e[i].version);
	pt_close(fd);
	return pt_rename(tmp, path);
}

static int update(struct pkgs *idx)
{
	char path[PT_PATH_MAX], tmp[PT_PATH_MAX + 8], base[PT_PATH_MAX];
	int err;

	repo(base, sizeof(base));
	if (!pkg_path("index.txt", path, sizeof(path)))
		return fail("pkg", "~/.config/pkg", -ENAMETOOLONG);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	pt_printf("pkg: the index from %s\n", base);
	if ((err = fetch("index.txt", tmp)) || (err = pt_rename(tmp, path))) {
		pt_unlink(tmp);
		return fail("pkg", "the index", err);
	}
	load(idx, "index.txt", index_line);
	pt_printf("pkg: %d package%s\n", idx->n, idx->n == 1 ? "" : "s");
	return 0;
}

/* The index, fetched first if it has never been. */
static int ready(struct pkgs *idx)
{
	if (!load(idx, "index.txt", index_line))
		return fail("pkg", NULL, -ENOMEM);
	return idx->n ? 0 : update(idx);
}

static bool built_in(const char *name)
{
	for (const struct pt_program *p = program_first(); p; p = p->next)
		if (!strcmp(p->name, name))
			return true;
	return false;
}

static int install(struct pkgs *idx, struct pkgs *inst, const char *name)
{
	char src_dir[PT_PATH_MAX], src[PT_PATH_MAX + 32], tmp[PT_PATH_MAX + 40];
	char rel[96], bin[PT_PATH_MAX], out[PT_PATH_MAX + 32], sha[65];
	char *argv[] = { "picoc", "-o", out, src, NULL };
	const struct entry *e = find(idx, name);
	struct entry *have;
	long size;
	int err, st;

	if (!e) {
		pt_dprintf(PT_STDERR, "pkg: %s: no such package (pkg update, pkg list)\n", name);
		return 1;
	}
	if (built_in(name)) {
		pt_dprintf(PT_STDERR, "pkg: %s: the board has a command of that name already\n",
			   name);
		return 1;
	}
	if (!pkg_path("src", src_dir, sizeof(src_dir)))
		return fail("pkg", NULL, -ENAMETOOLONG);
	pt_mkdir(src_dir);
	snprintf(src, sizeof(src), "%s/%s.pico", src_dir, name);
	snprintf(tmp, sizeof(tmp), "%s.new", src);
	snprintf(rel, sizeof(rel), "packages/%s/%s.pico", name, name);
	pt_printf("pkg: %s %s: fetching\n", name, e->version);
	if ((err = fetch(rel, tmp))) {
		pt_unlink(tmp);
		return fail("pkg", name, err);
	}
	if (!sha256_of(tmp, sha, &size) || size != e->size || strcmp(sha, e->sha)) {
		pt_unlink(tmp);
		pt_dprintf(PT_STDERR, "pkg: %s: what came is not what the index says; "
				      "nothing installed (pkg update, and again)\n", name);
		return 1;
	}
	if ((err = pt_rename(tmp, src)))
		return fail("pkg", name, err);
	pt_mkdir(home_dir(bin, sizeof(bin), "bin"));
	snprintf(out, sizeof(out), "%s/%s", bin, name);
	pt_printf("pkg: %s: compiling\n", name);
	if ((st = run(argv))) {
		pt_dprintf(PT_STDERR, "pkg: %s: it did not compile here (%d)\n", name, st);
		return 1;
	}
	if ((have = find(inst, name)))
		strlcpy(have->version, e->version, sizeof(have->version));
	else if (inst->n < ENTRIES_MAX) {
		inst->e[inst->n] = *e;
		inst->n++;
	}
	if ((err = save_installed(inst)))
		return fail("pkg", "~/.config/pkg/installed", err);
	pt_printf("pkg: %s %s installed: run it as %s\n", name, e->version, name);
	return 0;
}

static int uninstall(struct pkgs *inst, const char *name)
{
	char path[PT_PATH_MAX + 32], dir[PT_PATH_MAX];
	struct entry *e = find(inst, name);
	int err;

	if (!e) {
		pt_dprintf(PT_STDERR, "pkg: %s: not installed\n", name);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/%s", home_dir(dir, sizeof(dir), "bin"), name);
	pt_unlink(path);
	if (pkg_path("src", dir, sizeof(dir))) {
		snprintf(path, sizeof(path), "%s/%s.pico", dir, name);
		pt_unlink(path);
	}
	*e = inst->e[--inst->n];
	if ((err = save_installed(inst)))
		return fail("pkg", "~/.config/pkg/installed", err);
	pt_printf("pkg: %s removed\n", name);
	return 0;
}

/* "* snake      1.0   the game: ..." -- the about cut to the line. */
static void show(const struct entry *e, const struct entry *have, int width)
{
	char head[80];
	int n = snprintf(head, sizeof(head), "%c %-10s %-5s ", have ? '*' : ' ', e->name, e->version);
	int room;

	if (have && strcmp(have->version, e->version))
		n += snprintf(head + n, sizeof(head) - n, "(have %s) ", have->version);
	room = width - n;
	if (utf8_width(e->about, strlen(e->about)) <= room)
		pt_printf("%s%s\n", head, e->about);
	else if (room > 1)			/* cut, and said to be */
		pt_printf("%s%.*s\xe2\x80\xa6\n", head,
			  (int)utf8_prefix(e->about, strlen(e->about), room - 1), e->about);
	else
		pt_printf("%s\n", head);
}

/* For Tab: the packages there are, or those installed after remove. */
static void pkg_more(const char *after, pt_complete_add add, void *ctx)
{
	struct pkgs p = { 0 };
	bool removing = !strcmp(after, "remove");

	if (!load(&p, removing ? "installed" : "index.txt", removing ? installed_line : index_line))
		return;
	for (int i = 0; i < p.n; i++)
		add(ctx, p.e[i].name);
	pt_free(p.e);
}

PT_COMPLETE_MORE(pkg, ": update list search info install remove upgrade\n"
		      "install: <more>\nremove: <more>\ninfo: <more>\n", pkg_more)

PT_PROGRAM(pkg, "install programs from the pico-os-packages repository\n"
	   "usage: pkg [update | list | search WORD | info NAME |\n"
	   "            install NAME... | remove NAME... | upgrade]\n"
	   "Packages are pico programs: fetched, checked, and compiled\n"
	   "here into ~/bin. ~/.config/pkg/repo can name another\n"
	   "repository, an address or a folder.")
{
	const char *cmd = argc > 1 ? argv[1] : "list";
	struct pkgs idx = { 0 }, inst = { 0 };
	struct pt_winsize ws = { 53, 23 };
	int status = 0;

	pt_ioctl(PT_STDOUT, PT_TTY_GETSIZE, &ws);
	if (!load(&inst, "installed", installed_line)) {
		status = fail("pkg", NULL, -ENOMEM);
		goto out;
	}
	if (!strcmp(cmd, "update") && argc == 2) {
		status = update(&idx) ? 1 : 0;
	} else if (!strcmp(cmd, "list") && argc == 2) {
		if ((status = ready(&idx)))
			goto out;
		for (int i = 0; i < idx.n; i++)
			show(&idx.e[i], find(&inst, idx.e[i].name), ws.cols);
		for (int i = 0; i < inst.n; i++)
			if (!find(&idx, inst.e[i].name))
				pt_printf("* %-10s %-5s (no longer in the index)\n", inst.e[i].name,
					  inst.e[i].version);
	} else if (!strcmp(cmd, "search") && argc == 3) {
		int found = 0;

		if ((status = ready(&idx)))
			goto out;
		for (int i = 0; i < idx.n; i++)
			if (strcasestr(idx.e[i].name, argv[2]) || strcasestr(idx.e[i].about, argv[2])) {
				show(&idx.e[i], find(&inst, idx.e[i].name), ws.cols);
				found++;
			}
		status = !found;
	} else if (!strcmp(cmd, "info") && argc == 3) {
		const struct entry *e, *have = find(&inst, argv[2]);

		if ((status = ready(&idx)))
			goto out;
		if (!(e = find(&idx, argv[2]))) {
			pt_dprintf(PT_STDERR, "pkg: %s: no such package\n", argv[2]);
			status = 1;
			goto out;
		}
		pt_printf("%s %s, %ld bytes of source\n%s\n", e->name, e->version, e->size, e->about);
		if (have)
			pt_printf("installed: %s\n", have->version);
	} else if (!strcmp(cmd, "install") && argc > 2) {
		if ((status = ready(&idx)))
			goto out;
		for (int i = 2; i < argc; i++)
			status |= install(&idx, &inst, argv[i]);
	} else if (!strcmp(cmd, "remove") && argc > 2) {
		for (int i = 2; i < argc; i++)
			status |= uninstall(&inst, argv[i]);
	} else if (!strcmp(cmd, "upgrade") && argc == 2) {
		int n = 0;

		if ((status = update(&idx) ? 1 : 0))
			goto out;
		for (int i = 0; i < inst.n; i++) {
			const struct entry *e = find(&idx, inst.e[i].name);

			if (e && strcmp(e->version, inst.e[i].version)) {
				status |= install(&idx, &inst, e->name);
				n++;
			}
		}
		if (!n)
			pt_printf("pkg: everything is up to date\n");
	} else {
		pt_dprintf(PT_STDERR, "usage: pkg [update | list | search WORD | info NAME |\n"
				      "            install NAME... | remove NAME... | upgrade]\n");
		status = 2;
	}
out:
	pt_free(idx.e);
	pt_free(inst.e);
	return status;
}
