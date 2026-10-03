/*
 * A shell over the network.
 *
 * The board has one USB socket and it is the console, which means the
 * port cannot be used for anything else while someone is at a terminal.
 * This gives the shell somewhere else to live:
 *
 *	$ telnet pockettype.local        (or the address `wifi` prints)
 *
 * One session at a time, the same shell as on the screen, and the USB
 * port is then free for a keyboard or a stick.
 *
 * The connection is a character device as far as the rest of the system
 * is concerned: it reads, writes, and answers the terminal ioctls, so
 * the line editor, `top`, the editor and the games all work through it
 * exactly as they do on the panel.
 */
#include <string.h>

#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "drivers/drivers.h"

#if CONFIG_PT_NETCONSOLE

#define PORT		CONFIG_PT_NETCONSOLE_PORT
#define BACKLOG		1

/* Telnet's own bytes, so a real telnet client behaves like a raw pipe. */
#define IAC		255
#define WILL		251
#define DO		253
#define DONT		254
#define SB		250
#define SE		240
#define OPT_ECHO	1
#define OPT_SGA		3

static int	 listener = -1;
static int	 session = -1;		/* the socket the shell is talking to */
static int	 read_timeout_ms = -1;
static int	 fg_pgid, shell_pid;
static bool	 busy;
static volatile bool peer_gone;

/* ------------------------------------------------------------ the device */

/*
 * Strips telnet's in-band commands out of what the client sent. They
 * arrive rarely (mostly at connection time) and never carry anything the
 * shell wants.
 */
static size_t strip_telnet(uint8_t *buf, size_t n)
{
	size_t out = 0;

	for (size_t i = 0; i < n; i++) {
		if (buf[i] != IAC) {
			buf[out++] = buf[i];
			continue;
		}
		if (i + 1 >= n)
			break;			/* split across reads: drop it */
		if (buf[i + 1] == IAC) {
			buf[out++] = IAC;	/* an escaped 255 */
			i++;
		} else if (buf[i + 1] == SB) {
			while (i + 1 < n && buf[i] != SE)
				i++;		/* skip to the end of the option */
		} else {
			i += 2;			/* a two-byte command */
		}
	}
	return out;
}

static ssize_t net_read(struct pt_file *f, void *buf, size_t n)
{
	int timeout = read_timeout_ms >= 0 ? read_timeout_ms : 0;
	struct timeval tv = {
		.tv_sec = timeout / 1000,
		.tv_usec = timeout % 1000 * 1000,
	};
	ssize_t got;

	if (!n)
		return 0;
	if (session < 0 || peer_gone)
		return 0;			/* the client hung up: end of file */
	/* Zero clears the login deadline; a zero-sized option is invalid. */
	if (setsockopt(session, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)))
		return -EIO;
	for (;;) {
		got = recv(session, buf, n, read_timeout_ms == 0 ? MSG_DONTWAIT : 0);
		if (got > 0) {
			size_t kept = strip_telnet(buf, got);

			if (kept)
				return kept;
			continue;		/* it was all negotiation */
		}
		if (got == 0) {
			/* The far end closed: that is a hangup, and a
			 * hangup takes the shell with it, exactly as
			 * unplugging a terminal does. */
			peer_gone = true;
			if (shell_pid > 0)
				proc_signal_group(shell_pid, PT_SIGTERM);
			return 0;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return read_timeout_ms >= 0 ? -EAGAIN : 0;
		return -EIO;
	}
}

static ssize_t net_write(struct pt_file *f, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t sent = 0;

	if (session < 0 || peer_gone)
		return -EPIPE;
	while (sent < n) {
		ssize_t k = send(session, p + sent, n - sent, 0);

		if (k <= 0)
			return sent ? (ssize_t)sent : -EPIPE;
		sent += k;
	}
	return sent;
}

static int net_ioctl(struct pt_file *f, int req, void *arg)
{
	switch (req) {
	case PT_TTY_SETRAW:
	case PT_TTY_TRYSETRAW:
		return 0;			/* the client is already raw */
	case PT_TTY_GETSIZE: {
		struct pt_winsize *ws = arg;

		ws->cols = 80;			/* telnet can say, but rarely does */
		ws->rows = 24;
		return 0;
	}
	case PT_TTY_SETPGRP:
		fg_pgid = *(int *)arg;
		return 0;
	case PT_TTY_SETTIMEOUT:
		read_timeout_ms = *(int *)arg;
		return 0;
	}
	return -ENOTTY;
}

static const struct pt_file_ops net_ops = {
	.read = net_read,
	.write = net_write,
	.ioctl = net_ioctl,
};

/* ------------------------------------------------------------ logging in */

/*
 * Nobody gets the shell without the password kept by kernel/auth.c, and
 * with none set nobody gets it at all: a shell open to anyone on the same
 * network -- a cafe's, a school's, a phone's hotspot -- is the whole card
 * and every saved Wi-Fi password handed out. Each wrong password makes the
 * next attempt, from anyone, wait twice as long, up to a minute, which
 * makes guessing slower than it is worth.
 */
#define MAX_WAIT_S	60

static int failures;

static int recv_byte(int sock)
{
	uint8_t c;

	return recv(sock, &c, 1, 0) == 1 ? c : -1;
}

static void say(int sock, const char *s)
{
	send(sock, s, strlen(s), 0);
}

/* A line from the client, telnet's own bytes left out and nothing echoed. */
static int recv_line(int sock, char *buf, int size)
{
	int n = 0;

	for (;;) {
		int c = recv_byte(sock);

		if (c < 0)
			return -1;
		if (c == IAC) {
			int cmd = recv_byte(sock);

			if (cmd == SB) {
				while ((c = recv_byte(sock)) >= 0 && c != SE)
					;
				if (c < 0)
					return -1;
				continue;
			}
			if (cmd < 0)
				return -1;
			if (cmd >= WILL && cmd <= DONT && recv_byte(sock) < 0)
				return -1;
			if (cmd != IAC)
				continue;	/* negotiation, NOP and the like */
			c = IAC;		/* an escaped 255 */
		}
		if (c == '\r' || c == '\n') {
			uint8_t next;

			/* telnet ends a line with CR LF or CR NUL: all of it */
			if (c == '\r' && recv(sock, &next, 1, MSG_PEEK | MSG_DONTWAIT) == 1 &&
			    (next == '\n' || next == 0))
				recv(sock, &next, 1, 0);
			break;
		}
		if (c == 0x7f || c == '\b') {
			if (n)
				n--;
		} else if (c >= ' ' && n < size - 1) {
			buf[n++] = (char)c;
		}
	}
	buf[n] = '\0';
	return n;
}

static bool login(int sock, const char *who)
{
	struct timeval tv = { .tv_sec = 60 };
	char pw[64], line[80];

	if (!auth_is_set()) {
		say(sock, "No password has been set, so the network shell is closed.\r\n"
			  "Set one on the board itself with: passwd\r\n");
		return false;
	}
	if (failures) {
		int wait = failures >= 6 ? MAX_WAIT_S : 1 << failures;

		snprintf(line, sizeof(line), "(a wrong password was given: waiting %d s)\r\n", wait);
		say(sock, line);
		vTaskDelay(pdMS_TO_TICKS(wait * 1000));
	}
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	for (int tries = 0; tries < 3; tries++) {
		bool ok;

		say(sock, "password: ");
		if (recv_line(sock, pw, sizeof(pw)) < 0)
			break;
		say(sock, "\r\n");
		ok = !auth_check(pw);
		memset(pw, 0, sizeof(pw));
		if (ok) {
			failures = 0;
			return true;
		}
		failures++;
		klog("netconsole: wrong password from %s", who);
		say(sock, "wrong password\r\n");
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	return false;
}

/* ------------------------------------------------------------ the server */

static void greet(int sock)
{
	static const uint8_t negotiate[] = {
		IAC, WILL, OPT_ECHO,	/* we echo, so the client must not */
		IAC, WILL, OPT_SGA,	/* and must send each key as it comes */
	};
	static const char hello[] = "\r\nPocketType on the network.\r\n";

	send(sock, negotiate, sizeof(negotiate), 0);
	send(sock, hello, sizeof(hello) - 1, 0);
}

static void serve(int sock)
{
	char *shell[] = { "sh", "-l", NULL };
	struct pt_file *console;
	int pid;

	session = sock;
	read_timeout_ms = -1;
	peer_gone = false;
	shell_pid = 0;
	console = file_alloc(&net_ops, NULL);
	if (!console) {
		session = -1;
		return;
	}
	console->is_tty = true;
	pid = proc_spawn_console(2, shell, console);
	if (pid >= 0) {
		shell_pid = pid;
		proc_wait_orphan(pid);
	}
	shell_pid = 0;
	file_put(console);
	session = -1;
}

static void netconsole_task(void *arg)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(PORT),
	};
	int yes = 1;

	for (;;) {			/* wait for an address before listening */
		if (wifi_up())
			break;
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener < 0) {
		klog("netconsole: no socket");
		vTaskDelete(NULL);
	}
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, BACKLOG)) {
		klog("netconsole: cannot listen on port %d", PORT);
		close(listener);
		listener = -1;
		vTaskDelete(NULL);
	}
	klog("netconsole: a shell is waiting on port %d", PORT);

	for (;;) {
		struct sockaddr_in from;
		socklen_t len = sizeof(from);
		int sock = accept(listener, (struct sockaddr *)&from, &len);

		if (sock < 0) {
			vTaskDelay(pdMS_TO_TICKS(500));
			continue;
		}
		if (busy) {
			static const char no[] = "\r\nsomebody else is already here\r\n";

			send(sock, no, sizeof(no) - 1, 0);
			close(sock);
			continue;
		}
		busy = true;
		greet(sock);
		if (login(sock, inet_ntoa(from.sin_addr))) {
			klog("netconsole: %s logged in", inet_ntoa(from.sin_addr));
			serve(sock);
			klog("netconsole: disconnected");
		}
		close(sock);
		busy = false;
	}
}

bool netconsole_busy(void)
{
	return busy;
}

int netconsole_port(void)
{
	return listener >= 0 ? PORT : -1;
}

int netconsole_init(void)
{
	if (!wifi_started())
		return -ENODEV;
	xTaskCreatePinnedToCore(netconsole_task, "knetcon", 4096, NULL, 4, NULL, 0);
	return 0;
}

#else

int  netconsole_init(void) { return -ENODEV; }
bool netconsole_busy(void) { return false; }
int  netconsole_port(void) { return -1; }

#endif
