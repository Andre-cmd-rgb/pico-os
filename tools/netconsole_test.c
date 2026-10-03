/* The login timeout must be cleared when the network shell starts reading. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define IAC 255
#define SB 250
#define SE 240
#define PT_SIGTERM 15
struct pt_file { int unused; };
static int session = -1, read_timeout_ms = -1, shell_pid = 42, signals;
static volatile bool peer_gone;
static void proc_signal_group(int pid, int sig)
{
	assert(pid == shell_pid && sig == PT_SIGTERM);
	signals++;
}
#include "net_read_under_test.h"

static void *delayed_output(void *arg)
{
	int peer = *(int *)arg;

	usleep(50000);
	assert(send(peer, "abc", 3, 0) == 3);
	return NULL;
}

int main(void)
{
	struct timeval login_limit = { .tv_usec = 5000 }, timeout;
	socklen_t size = sizeof(timeout);
	int pair[2];
	pthread_t writer;
	char buf[16];

	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
	session = pair[0];
	assert(!setsockopt(session, SOL_SOCKET, SO_RCVTIMEO, &login_limit, sizeof(login_limit)));
	assert(!pthread_create(&writer, NULL, delayed_output, &pair[1]));
	assert(net_read(NULL, buf, sizeof(buf)) == 3 && !memcmp(buf, "abc", 3));
	assert(!pthread_join(writer, NULL));
	assert(!getsockopt(session, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size));
	assert(!timeout.tv_sec && !timeout.tv_usec);
	read_timeout_ms = 10;
	assert(net_read(NULL, buf, sizeof(buf)) == -EAGAIN && !peer_gone);
	read_timeout_ms = 0;
	assert(net_read(NULL, buf, sizeof(buf)) == -EAGAIN && !peer_gone);
	assert(net_read(NULL, buf, 0) == 0 && !peer_gone && !signals);
	assert(send(pair[1], "hello", 5, 0) == 5);
	assert(net_read(NULL, buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5));
	assert(!close(pair[1]));
	assert(net_read(NULL, buf, sizeof(buf)) == 0 && peer_gone && signals == 1);
	assert(!close(pair[0]));
	puts("netconsole: login deadline, timed reads, polling and EOF passed");
	return 0;
}
