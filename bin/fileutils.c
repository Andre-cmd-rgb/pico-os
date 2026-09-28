/*
 * File programs: ls cat cp mv rm mkdir rmdir touch pwd df mount umount
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "drivers/drivers.h"
#include "util.h"

/* ------------------------------------------------------------ ls */

struct ls {
	bool	 all, one, dirs, lng, recurse, reverse, by_size, by_time;
	bool	 color;
	bool	 headers;		/* name each directory before its listing */
	int	 shown;			/* listings so far, for the blank line between */
};

struct entry {
	char		*name;
	struct pt_stat	 st;
	const struct ls	*o;		/* how to sort: qsort passes nothing else */
};

static int compare_entries(const void *pa, const void *pb)
{
	const struct entry *a = pa, *b = pb;
	int c;

	if (a->o->by_time && a->st.mtime != b->st.mtime)
		c = a->st.mtime > b->st.mtime ? -1 : 1;		/* newest first */
	else if (a->o->by_size && a->st.size != b->st.size)
		c = a->st.size > b->st.size ? -1 : 1;		/* largest first */
	else if (!(c = strcasecmp(a->name, b->name)))
		c = strcmp(a->name, b->name);
	return a->o->reverse ? -c : c;
}

static void print_long(const struct entry *e, bool color)
{
	char size[16], when[24] = "                ";
	struct tm tm;

	human_size(e->st.size, size, sizeof(size));
	if (e->st.mtime > 315532800 && localtime_r(&e->st.mtime, &tm))	/* after 1980 */
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	pt_printf("%c %6s  %s  %s%s%s%s\n", e->st.is_dir ? 'd' : '-',
		  e->st.is_dir ? "-" : size, when,
		  color && e->st.is_dir ? "\x1b[1m" : "", e->name,
		  e->st.is_dir ? "/" : "", color && e->st.is_dir ? "\x1b[0m" : "");
}

static void print_columns(const struct entry *e, int n, bool color)
{
	int cols, rows;
	size_t width = 0;

	pt_tty_size(PT_STDOUT, &cols, &rows);
	for (int i = 0; i < n; i++) {
		size_t w = strlen(e[i].name) + e[i].st.is_dir;
		if (w > width)
			width = w;
	}
	width += 2;
	int per_row = cols / (int)width;
	if (per_row < 1)
		per_row = 1;
	int lines = (n + per_row - 1) / per_row;

	/* fill down the columns, like ls does */
	for (int r = 0; r < lines; r++) {
		for (int c = 0; c < per_row; c++) {
			int i = c * lines + r;
			if (i >= n)
				break;
			size_t w = strlen(e[i].name) + e[i].st.is_dir;
			pt_printf("%s%s%s%s", color && e[i].st.is_dir ? "\x1b[1m" : "", e[i].name,
				  e[i].st.is_dir ? "/" : "", color && e[i].st.is_dir ? "\x1b[0m" : "");
			if (c + 1 < per_row && (c + 1) * lines + r < n)
				pt_printf("%*s", (int)(width - w), "");
		}
		pt_puts("\n");
	}
}

/* Sorted, then printed: in columns on a terminal, a name a line elsewhere. */
static void print_entries(const struct ls *o, struct entry *v, int n)
{
	qsort(v, n, sizeof(*v), compare_entries);
	if (o->lng) {
		for (int i = 0; i < n; i++)
			print_long(&v[i], o->color);
	} else if (o->color && !o->one) {
		print_columns(v, n, o->color);
	} else {
		for (int i = 0; i < n; i++)
			pt_printf("%s\n", v[i].name);
	}
}

static void free_entries(struct entry *v, int n)
{
	for (int i = 0; i < n; i++)
		pt_free(v[i].name);
	pt_free(v);
}

/*
 * One directory's listing, and with -R the ones below it. The recursion
 * keeps nothing but pointers on the stack, a few dozen bytes a level:
 * the paths and the listings are on the heap.
 */
static int list_dir(struct ls *o, const char *path)
{
	pt_dir_t *d;
	struct pt_dirent *ent = pt_malloc(sizeof(*ent));
	char *full = pt_malloc(PT_PATH_MAX);
	struct entry *v = NULL;
	int n = 0, cap = 0, status = 0;
	int err = ent && full ? pt_opendir(path, &d) : -ENOMEM;

	if (err) {
		pt_free(ent);
		pt_free(full);
		return fail("ls", path, err);
	}
	while (pt_readdir(d, ent) == 1) {
		if (ent->name[0] == '.' && !o->all)
			continue;
		if (n == cap) {
			struct entry *grown = pt_realloc(v, (cap = cap ? cap * 2 : 32) * sizeof(*v));

			if (!grown) {
				status = fail("ls", path, -ENOMEM);
				break;
			}
			v = grown;
		}
		if (!(v[n].name = pt_strdup(ent->name))) {
			status = fail("ls", path, -ENOMEM);
			break;
		}
		memset(&v[n].st, 0, sizeof(v[n].st));
		v[n].st.is_dir = ent->is_dir;
		v[n].o = o;
		/* a size and a time cost a lookup each: only when they are wanted */
		if ((o->lng || o->by_time || o->by_size) && join_path(path, ent->name, full, PT_PATH_MAX))
			pt_stat(full, &v[n].st);
		n++;
	}
	pt_closedir(d);
	pt_free(ent);

	if (o->headers)
		pt_printf("%s%s:\n", o->shown ? "\n" : "", path);
	o->shown++;
	print_entries(o, v, n);
	for (int i = 0; o->recurse && i < n && !pt_interrupted(); i++)
		if (v[i].st.is_dir && join_path(path, v[i].name, full, PT_PATH_MAX))
			status |= list_dir(o, full);
	free_entries(v, n);
	pt_free(full);
	return status;
}

PT_PROGRAM(ls, "list directory contents\n"
	   "usage: ls [-1aAdlhrRSt] [path...]\n"
	   "  -l  long: size and date   -a  hidden too (so -A)\n"
	   "  -t  newest first   -S  largest first\n"
	   "  -r  the other way round   -1  a name a line\n"
	   "  -R  folders inside folders too\n"
	   "  -d  folders as themselves, not what is in them\n"
	   "Sizes are always in K, M and G: -h changes nothing.")
{
	struct ls o = { .color = pt_isatty(PT_STDOUT) };
	struct opt g = { .ind = 1 };
	struct entry *files = NULL;
	int c, nfiles = 0, status = 0;

	while ((c = getopt_pt(&g, "ls", argc, argv, "1aAdhlRrSt")) != -1) {
		switch (c) {
		case '1': o.one = true; break;
		case 'a': case 'A': o.all = true; break;
		case 'd': o.dirs = true; break;
		case 'h': break;
		case 'l': o.lng = true; break;
		case 'R': o.recurse = true; break;
		case 'r': o.reverse = true; break;
		case 'S': o.by_size = true; break;
		case 't': o.by_time = true; break;
		default: return 2;
		}
	}
	o.headers = o.recurse || argc - g.ind > 1;
	if (g.ind == argc && !o.dirs)
		return list_dir(&o, ".");
	/* files first, all together, then each directory: as ls does it */
	if (!(files = pt_calloc(argc - g.ind + 1, sizeof(*files))))
		return fail("ls", NULL, -ENOMEM);
	for (int i = g.ind; i < argc || (i == g.ind && g.ind == argc); i++) {
		const char *name = i < argc ? argv[i] : ".";
		struct pt_stat st;
		int err = pt_stat(name, &st);

		if (err) {
			fail("ls", name, err);
			status = 2;
		} else if (!st.is_dir || o.dirs) {
			files[nfiles++] = (struct entry){ .name = (char *)name, .st = st, .o = &o };
		}
	}
	if (nfiles) {
		print_entries(&o, files, nfiles);
		o.shown++;
	}
	for (int i = g.ind; i < argc && !o.dirs; i++) {
		struct pt_stat st;

		if (!pt_stat(argv[i], &st) && st.is_dir && list_dir(&o, argv[i]))
			status |= 1;
	}
	pt_free(files);
	return status;
}

/* ------------------------------------------------------------ cat */

PT_PROGRAM(cat, "print files one after another\nusage: cat [file...]\nWith no file, copies standard input.")
{
	int status = 0;

	if (argc == 1)
		return copy_fd(PT_STDIN, PT_STDOUT) < 0;
	for (int i = 1; i < argc; i++) {
		int fd = pt_open(argv[i], O_RDONLY);
		if (fd < 0) {
			status = fail("cat", argv[i], fd);
			continue;
		}
		ssize_t r = copy_fd(fd, PT_STDOUT);
		pt_close(fd);
		if (r < 0)
			status = fail("cat", argv[i], r);
	}
	return status;
}

/* ------------------------------------------------------------ cp, mv, rm */

/* FAT ignores case and LittleFS does not: can two spellings name one file? */
static bool folds_case(const char *abs)
{
	char vfs[PT_PATH_MAX + 16];
	const struct pt_mount *m = mount_resolve(abs, vfs, sizeof(vfs));

	return m && strcmp(m->type, "littlefs");
}

static bool same_file(const char *a, const char *b)
{
	char x[PT_PATH_MAX], y[PT_PATH_MAX];

	if (pt_abspath(a, x, sizeof(x)) || pt_abspath(b, y, sizeof(y)))
		return false;
	return !strcmp(x, y) || (!strcasecmp(x, y) && folds_case(x) && folds_case(y));
}

/* The same file spelled with different case, on a filesystem that ignores it. */
static bool case_change(const char *a, const char *b)
{
	char x[PT_PATH_MAX], y[PT_PATH_MAX];

	return !pt_abspath(a, x, sizeof(x)) && !pt_abspath(b, y, sizeof(y)) && strcmp(x, y) &&
	       same_file(x, y);
}

/* Is `path` the directory `dir` or somewhere below it? */
static bool inside(const char *path, const char *dir)
{
	char p[PT_PATH_MAX], d[PT_PATH_MAX];

	if (pt_abspath(path, p, sizeof(p)) || pt_abspath(dir, d, sizeof(d)))
		return false;
	size_t n = strlen(d);
	int differ = folds_case(d) ? strncasecmp(p, d, n) : strncmp(p, d, n);
	return !differ && (p[n] == '/' || !p[n] || n == 1);
}

static int copy_file(const char *prog, const char *from, const char *to)
{
	if (same_file(from, to)) {
		pt_dprintf(PT_STDERR, "%s: %s and %s are the same file\n", prog, from, to);
		return 1;
	}
	int in = pt_open(from, O_RDONLY);

	if (in < 0)
		return fail(prog, from, in);
	int out = pt_open(to, O_WRONLY | O_CREAT | O_TRUNC);
	if (out < 0) {
		pt_close(in);
		return fail(prog, to, out);
	}
	ssize_t r = copy_fd(in, out);
	pt_close(in);
	int err = pt_close(out);		/* the last block is written here */
	if (r >= 0 && err)
		r = err;
	return r < 0 ? fail(prog, to, r) : 0;
}

/*
 * Recursion keeps its path buffers on the heap: a process has an 8 KB stack,
 * and a few hundred bytes per directory level would run out a few levels down.
 */
struct walk {
	char		  path[PT_PATH_MAX];
	char		  other[PT_PATH_MAX];
	struct pt_dirent  ent;
};

static int copy_tree(const char *prog, const char *from, const char *to)
{
	struct pt_stat st;
	pt_dir_t *d;
	int err = pt_stat(from, &st), status = 0;

	if (err)
		return fail(prog, from, err);
	if (!st.is_dir)
		return copy_file(prog, from, to);
	err = pt_mkdir(to);
	if (err && err != -EEXIST)
		return fail(prog, to, err);
	struct walk *w = pt_malloc(sizeof(*w));
	if (!w)
		return fail(prog, from, -ENOMEM);
	if ((err = pt_opendir(from, &d))) {
		pt_free(w);
		return fail(prog, from, err);
	}
	while (pt_readdir(d, &w->ent) == 1 && !pt_interrupted()) {
		if (!join_path(from, w->ent.name, w->path, sizeof(w->path)) ||
		    !join_path(to, w->ent.name, w->other, sizeof(w->other))) {
			status = fail(prog, w->ent.name, -ENAMETOOLONG);
			continue;
		}
		status |= copy_tree(prog, w->path, w->other);
	}
	pt_closedir(d);
	pt_free(w);
	return status;
}

static bool is_mount_point(const char *path)
{
	char abs[PT_PATH_MAX];

	if (pt_abspath(path, abs, sizeof(abs)))
		return false;
	if (!strcmp(abs, "/") || !strcmp(abs, "/proc") || !strcmp(abs, "/dev"))
		return true;
	for (int i = 0; i < mount_count(); i++)
		if (!strcmp(mount_path(mount_get(i)), abs))
			return true;
	return false;
}

static int remove_tree(const char *prog, const char *path, bool recursive, bool force)
{
	struct pt_stat st;
	int err = pt_stat(path, &st);

	if (is_mount_point(path)) {
		pt_dprintf(PT_STDERR, "%s: refusing to remove %s: it is a mount point\n", prog, path);
		return 1;
	}
	if (err)
		return force && err == -ENOENT ? 0 : fail(prog, path, err);
	if (!st.is_dir) {
		err = pt_unlink(path);
		return err ? fail(prog, path, err) : 0;
	}
	if (!recursive) {
		pt_dprintf(PT_STDERR, "%s: %s: is a directory (use -r)\n", prog, path);
		return 1;
	}

	pt_dir_t *d;
	struct walk *w = pt_malloc(sizeof(*w));
	int status = 0;

	if (!w)
		return fail(prog, path, -ENOMEM);
	if ((err = pt_opendir(path, &d))) {
		pt_free(w);
		return fail(prog, path, err);
	}
	/* collect first: removing while iterating confuses FAT readdir */
	char **names = NULL;
	int n = 0;
	while (pt_readdir(d, &w->ent) == 1) {
		char **grown = pt_realloc(names, (n + 1) * sizeof(*names));
		if (!grown) {
			status = fail(prog, path, -ENOMEM);
			break;
		}
		names = grown;
		names[n++] = pt_strdup(w->ent.name);
	}
	pt_closedir(d);
	for (int i = 0; i < n && !pt_interrupted(); i++) {
		if (!names[i])
			status = fail(prog, path, -ENOMEM);
		else if (!join_path(path, names[i], w->path, sizeof(w->path)))
			status = fail(prog, names[i], -ENAMETOOLONG);
		else
			status |= remove_tree(prog, w->path, true, force);
	}
	for (int i = 0; i < n; i++)
		pt_free(names[i]);
	pt_free(names);
	pt_free(w);
	if (!status && (err = pt_rmdir(path)))
		status = fail(prog, path, err);
	return status;
}

/* Where `src` goes when the last operand is `dst`; NULL if the path is too long. */
static const char *target_path(const char *src, const char *dst, bool dst_is_dir, char *out, size_t size)
{
	if (!dst_is_dir)
		return dst;
	size_t len = strlen(src);
	while (len > 1 && src[len - 1] == '/')
		len--;				/* "dir/" names dir */
	const char *base = src + len;
	while (base > src && base[-1] != '/')
		base--;
	char name[PT_NAME_MAX];
	snprintf(name, sizeof(name), "%.*s", (int)(src + len - base), base);
	return join_path(dst, name, out, size);
}

PT_PROGRAM(cp, "copy files\nusage: cp [-r] source... target\n  -r  copy directories recursively")
{
	uint32_t flags;
	int i = parse_flags("cp", argc, argv, "r", &flags);
	struct pt_stat st;
	char to[PT_PATH_MAX];
	int status = 0;

	if (i < 0 || argc - i < 2) {
		pt_dprintf(PT_STDERR, "usage: cp [-r] source... target\n");
		return 2;
	}
	const char *dst = argv[argc - 1];
	bool dst_dir = !pt_stat(dst, &st) && st.is_dir;
	if (argc - i > 2 && !dst_dir) {
		pt_dprintf(PT_STDERR, "cp: %s is not a directory\n", dst);
		return 1;
	}
	for (; i < argc - 1; i++) {
		struct pt_stat src;
		int err = pt_stat(argv[i], &src);
		if (err) {
			status = fail("cp", argv[i], err);
			continue;
		}
		if (src.is_dir && !FLAG(flags, 'r')) {
			pt_dprintf(PT_STDERR, "cp: %s: is a directory (use -r)\n", argv[i]);
			status = 1;
			continue;
		}
		const char *target = target_path(argv[i], dst, dst_dir, to, sizeof(to));
		if (!target) {
			status = fail("cp", argv[i], -ENAMETOOLONG);
			continue;
		}
		if (src.is_dir && inside(target, argv[i])) {
			pt_dprintf(PT_STDERR, "cp: cannot copy %s into itself\n", argv[i]);
			status = 1;
			continue;
		}
		status |= copy_tree("cp", argv[i], target);
	}
	return status;
}

PT_PROGRAM(mv, "move or rename files\nusage: mv source... target")
{
	struct pt_stat st;
	char to[PT_PATH_MAX];
	int status = 0;

	if (argc < 3) {
		pt_dprintf(PT_STDERR, "usage: mv source... target\n");
		return 2;
	}
	const char *dst = argv[argc - 1];
	bool dst_dir = !pt_stat(dst, &st) && st.is_dir;
	if (argc > 3 && !dst_dir) {
		pt_dprintf(PT_STDERR, "mv: %s is not a directory\n", dst);
		return 1;
	}
	for (int i = 1; i < argc - 1; i++) {
		struct pt_stat src;
		int err = pt_stat(argv[i], &src);
		if (err) {
			status = fail("mv", argv[i], err);
			continue;
		}
		/* `mv notes Notes` into the directory `notes` itself on FAT: a rename */
		bool recase = case_change(argv[i], dst);
		const char *target = recase ? dst : target_path(argv[i], dst, dst_dir, to, sizeof(to));
		if (!target) {
			status = fail("mv", argv[i], -ENAMETOOLONG);
			continue;
		}
		recase = recase || case_change(argv[i], target);
		if (!recase && same_file(argv[i], target)) {
			pt_dprintf(PT_STDERR, "mv: %s and %s are the same file\n", argv[i], target);
			status = 1;
			continue;
		}
		if (src.is_dir && !recase && inside(target, argv[i])) {
			pt_dprintf(PT_STDERR, "mv: cannot move %s into itself\n", argv[i]);
			status = 1;
			continue;
		}
		err = pt_rename(argv[i], target);	/* replaces a file already there */
		if (err == -EXDEV) {
			/* different filesystems: copy, then remove */
			err = copy_tree("mv", argv[i], target) ? -EIO : 0;
			if (!err)
				err = remove_tree("mv", argv[i], true, false) ? -EIO : 0;
			if (err)
				status = 1;		/* copy_tree and remove_tree said why */
			continue;
		}
		if (err)
			status = fail("mv", argv[i], err);
	}
	return status;
}

PT_PROGRAM(rm, "remove files\nusage: rm [-rf] path...\n  -r  remove directories and their contents\n  -f  ignore missing files")
{
	uint32_t flags;
	int i = parse_flags("rm", argc, argv, "rf", &flags);
	int status = 0;

	if (i < 0 || i == argc) {
		pt_dprintf(PT_STDERR, "usage: rm [-rf] path...\n");
		return 2;
	}
	for (; i < argc; i++)
		status |= remove_tree("rm", argv[i], FLAG(flags, 'r'), FLAG(flags, 'f'));
	return status;
}

/* ------------------------------------------------------------ directories */

static int mkdir_parents(const char *path)
{
	char buf[PT_PATH_MAX];
	int err = pt_abspath(path, buf, sizeof(buf));

	if (err)
		return err;
	for (char *p = buf + 1; ; p++) {
		if (*p == '/' || !*p) {
			char saved = *p;
			*p = '\0';
			err = pt_mkdir(buf);
			if (err && err != -EEXIST && err != -EBUSY && err != -EROFS)
				return err;
			*p = saved;
			if (!saved)
				return 0;
		}
	}
}

PT_PROGRAM(mkdir, "make directories\nusage: mkdir [-p] dir...\n  -p  create parents too, no error if it exists")
{
	uint32_t flags;
	int i = parse_flags("mkdir", argc, argv, "p", &flags);
	int status = 0;

	if (i < 0 || i == argc) {
		pt_dprintf(PT_STDERR, "usage: mkdir [-p] dir...\n");
		return 2;
	}
	for (; i < argc; i++) {
		int err = FLAG(flags, 'p') ? mkdir_parents(argv[i]) : pt_mkdir(argv[i]);
		if (err)
			status = fail("mkdir", argv[i], err);
	}
	return status;
}

PT_PROGRAM(rmdir, "remove empty directories\nusage: rmdir dir...")
{
	int status = 0;

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: rmdir dir...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		int err = pt_rmdir(argv[i]);
		if (err)
			status = fail("rmdir", argv[i], err);
	}
	return status;
}

PT_PROGRAM(touch, "make files' times now, creating any that are missing\n"
	   "usage: touch file...")
{
	int status = 0;

	if (argc < 2) {
		pt_dprintf(PT_STDERR, "usage: touch file...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		int fd = pt_open(argv[i], O_WRONLY | O_CREAT | O_APPEND), err;

		if (fd < 0) {
			status = fail("touch", argv[i], fd);
			continue;
		}
		err = pt_close(fd);
		if (!err)
			err = pt_utime(argv[i], 0);
		if (err)
			status = fail("touch", argv[i], err);
	}
	return status;
}

PT_PROGRAM(pwd, "print the working directory")
{
	pt_printf("%s\n", pt_getcwd());
	return 0;
}

/* ------------------------------------------------------------ filesystems */

PT_PROGRAM(sync, "write out everything still in file buffers")
{
	int err = vfs_sync_all();

	return err ? fail("sync", NULL, err) : 0;
}

/* One filesystem's line, the header first if it is the first; false if it has none. */
static bool df_line(const struct pt_mount *m, bool *header)
{
	uint64_t total, free;
	char a[16], b[16], c[16];
	const char *src = strchr(m->source, ':') ? strchr(m->source, ':') + 1 : m->source;

	if ((m->present && !m->present()) || !m->info || m->info(&total, &free))
		return false;
	if (!*header) {
		pt_printf("%-8s %7s %7s %7s %4s %s\n", "Source", "Size", "Used", "Free", "Use%", "Mount");
		*header = true;
	}
	human_size(total, a, sizeof(a));
	human_size(total - free, b, sizeof(b));
	human_size(free, c, sizeof(c));
	pt_printf("%-8.8s %7s %7s %7s %3d%% %s\n", src, a, b, c,
		  total ? (int)((total - free) * 100 / total) : 0, mount_path(m));
	return true;
}

PT_PROGRAM(df, "show free space on mounted filesystems\n"
	   "usage: df [file...]\n"
	   "With files, only the filesystems they are on.")
{
	bool header = false;
	int status = 0;

	if (argc == 1) {
		for (int i = 0; i < mount_count(); i++)
			if (!mount_get(i)->bind)	/* the card again, as ~ */
				df_line(mount_get(i), &header);
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		char abs[PT_PATH_MAX], vfs[PT_PATH_MAX + 16];
		const struct pt_mount *m = NULL;
		struct pt_stat st;
		int err = pt_stat(argv[i], &st);

		if (!err)
			err = pt_abspath(argv[i], abs, sizeof(abs));
		if (!err && !(m = mount_resolve(abs, vfs, sizeof(vfs))))
			err = -ENOENT;
		if (!err && !df_line(m, &header))
			err = -EIO;
		if (err)
			status = fail("df", argv[i], err);
	}
	return status;
}

PT_COMPLETE(mount, ": /mnt/sd\n")

PT_PROGRAM(mount, "list mounts, or mount the SD card\nusage: mount [/mnt/sd]")
{
	if (argc == 1) {
		int fd = pt_open("/proc/mounts", O_RDONLY);
		if (fd >= 0) {
			copy_fd(fd, PT_STDOUT);
			pt_close(fd);
		}
		return 0;
	}
	if (strcmp(argv[1], "/mnt/sd")) {
		pt_dprintf(PT_STDERR, "mount: only /mnt/sd can be mounted\n");
		return 2;
	}
	int err = sd_mount();
	if (err)
		return fail("mount", "/mnt/sd", err == -ENODEV ? -ENXIO : err);
	char desc[80];
	sd_describe(desc, sizeof(desc));
	pt_printf("/mnt/sd: %s\n", desc);
	return 0;
}

PT_PROGRAM(umount, "unmount the SD card so it can be removed\nusage: umount /mnt/sd")
{
	if (argc != 2 || strcmp(argv[1], "/mnt/sd")) {
		pt_dprintf(PT_STDERR, "usage: umount /mnt/sd\n");
		return 2;
	}
	int err = sd_unmount();
	if (err == -EBUSY) {
		pt_dprintf(PT_STDERR, "umount: /mnt/sd: files are open on it; close them first\n");
		return 1;
	}
	return err ? fail("umount", "/mnt/sd", err == -EINVAL ? -ENOENT : err) : 0;
}
