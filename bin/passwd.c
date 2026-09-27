/*
 * passwd - set the password the network shell asks for.
 *
 * The screen and the USB console need none: whoever holds the machine has
 * it. The network is different, and until a password is set the network
 * shell does not let anyone in (kernel/auth.c keeps it, hashed).
 */
#include <string.h>

#include "pt/kernel.h"
#include "util.h"

#define MIN_LEN		6

/* A line typed with nothing shown: 0, or -ECANCELED for Ctrl-C or Ctrl-D. */
static int read_secret(const char *prompt, char *buf, size_t size)
{
	size_t n = 0;
	int ret = 0;

	pt_puts(prompt);
	pt_tty_raw(PT_STDIN, true);
	for (;;) {
		int c = pt_getc(PT_STDIN);

		if (c < 0 || c == PT_CTRL('c') || (c == PT_CTRL('d') && !n)) {
			ret = -ECANCELED;
			break;
		}
		if (c == '\r' || c == '\n')
			break;
		if (c == 0x7f || c == '\b') {
			if (n)
				n--;
		} else if (c >= ' ' && n < size - 1) {
			buf[n++] = (char)c;
		}
	}
	buf[n] = '\0';
	pt_tty_raw(PT_STDIN, false);
	pt_puts("\n");
	return ret;
}

PT_COMPLETE(passwd, ": -d\n")

PT_PROGRAM(passwd, "set the password the network shell asks for\n"
	   "usage: passwd [-d]\n"
	   "  -d  remove it, which closes the network shell again\n"
	   "Nobody can log in over the network until there is one.")
{
	char old[64], a[64], b[64];
	uint32_t flags;
	int err = 0, first = parse_flags("passwd", argc, argv, "d", &flags);

	if (first < 0)
		return 2;
	if (first != argc) {
		pt_dprintf(PT_STDERR, "usage: passwd [-d]\n");
		return 2;
	}
	if (auth_is_set()) {
		if (read_secret("Current password: ", old, sizeof(old)))
			return 1;
		err = auth_check(old);
		memset(old, 0, sizeof(old));
		if (err) {
			pt_dprintf(PT_STDERR, "passwd: %s\n",
				   err == -EACCES ? "that is not the password" : pt_strerror(err));
			return 1;
		}
	} else if (FLAG(flags, 'd')) {
		pt_puts("passwd: there is no password to remove\n");
		return 0;
	}
	if (FLAG(flags, 'd')) {
		if ((err = auth_set(NULL)))
			return fail("passwd", NULL, err);
		pt_puts("passwd: password removed; the network shell is closed\n");
		return 0;
	}

	if (read_secret("New password: ", a, sizeof(a)))
		return 1;
	if (strlen(a) < MIN_LEN) {
		pt_dprintf(PT_STDERR, "passwd: it has to be at least %d characters\n", MIN_LEN);
		err = 1;
	} else if (read_secret("Retype new password: ", b, sizeof(b))) {
		err = 1;
	} else if (strcmp(a, b)) {
		pt_dprintf(PT_STDERR, "passwd: the two did not match\n");
		err = 1;
	} else if ((err = auth_set(a))) {
		err = fail("passwd", NULL, err);
	} else {
		pt_puts("passwd: password updated\n");
	}
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	return err ? 1 : 0;
}
