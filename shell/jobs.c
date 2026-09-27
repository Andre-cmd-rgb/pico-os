/*
 * sh: job control.
 *
 * A job is what one command line started: a pipeline put in the
 * background with &, or one that Ctrl-Z stopped. The shell keeps them in
 * a table and lets %1 name one to kill or wait for; an interactive shell
 * also says when one finishes or stops, and lets fg and bg work on them.
 *
 * Ctrl-Z makes the terminal send SIGTSTP to the job in front, whose
 * processes stop at their next system call; waiting with PT_WUNTRACED is
 * how the shell hears of it. fg gives the job the terminal again and sends
 * SIGCONT, bg sends SIGCONT and leaves it behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh.h"

static bool in_table(struct sh *sh, const struct job *j)
{
	return j >= sh->jobs && j < sh->jobs + MAX_JOBS;
}

/* The lowest number no job has, as every shell numbers them. */
static int free_id(struct sh *sh)
{
	for (int id = 1;; id++) {
		bool taken = false;

		for (int i = 0; i < MAX_JOBS; i++)
			taken |= sh->jobs[i].id == id;
		if (!taken)
			return id;
	}
}

struct job *job_add(struct sh *sh, int pgid, const int *pids, int n, const char *cmd, size_t len)
{
	struct job *j = NULL;

	for (int i = 0; i < MAX_JOBS && !j; i++)
		if (!sh->jobs[i].id)
			j = &sh->jobs[i];
	if (!j)
		return NULL;			/* too many: it runs, untracked */
	memset(j, 0, sizeof(*j));
	/* the command as typed, less the & and spaces that end it */
	while (len && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t' || cmd[len - 1] == '&' ||
		       cmd[len - 1] == '\n'))
		len--;
	if (!(j->cmd = pt_malloc(len + 1)))
		return NULL;
	memcpy(j->cmd, cmd, len);
	j->cmd[len] = '\0';
	j->id = free_id(sh);
	j->pgid = pgid;
	j->n = n < MAX_STAGES ? n : MAX_STAGES;
	memcpy(j->pid, pids, j->n * sizeof(*pids));
	sh->current_job = j->id;
	return j;
}

void job_free(struct job *j)
{
	pt_free(j->cmd);
	memset(j, 0, sizeof(*j));
}

static struct job *job_of_pid(struct sh *sh, int pid)
{
	for (int i = 0; i < MAX_JOBS; i++)
		for (int k = 0; sh->jobs[i].id && k < sh->jobs[i].n; k++)
			if (sh->jobs[i].pid[k] == pid)
				return &sh->jobs[i];
	return NULL;
}

static struct job *job_by_id(struct sh *sh, int id)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (id && sh->jobs[i].id == id)
			return &sh->jobs[i];
	return NULL;
}

/*
 * "%2" (or "2"), "%%", "%+" or nothing for the current job -- the one
 * last stopped or put in the background. Says so and returns NULL when
 * there is no such job.
 */
struct job *job_find(struct sh *sh, const char *spec, const char *who)
{
	struct job *j = NULL;

	if (!spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+")) {
		if (!(j = job_by_id(sh, sh->current_job)))
			for (int i = 0; i < MAX_JOBS; i++)	/* the newest, then */
				if (sh->jobs[i].id && (!j || sh->jobs[i].id > j->id))
					j = &sh->jobs[i];
		if (!j)
			pt_dprintf(PT_STDERR, "%s: no current job\n", who);
		return j;
	}
	char *end;
	long id = strtol(spec + (*spec == '%'), &end, 10);

	if (*end || end == spec + (*spec == '%') || !(j = job_by_id(sh, (int)id)))
		pt_dprintf(PT_STDERR, "%s: %s: no such job\n", who, spec);
	return *end ? NULL : j;
}

/* "[1]+  Stopped      vi notes" */
void job_report(struct job *j, const char *what)
{
	pt_printf("[%d]  %-12s %s\n", j->id, what, j->cmd);
}

static const char *how_it_ended(int status, char *buf, size_t size)
{
	switch (status) {
	case 0: return "Done";
	case 128 + PT_SIGINT: return "Interrupted";
	case 128 + PT_SIGKILL: return "Killed";
	case 128 + PT_SIGTERM: return "Terminated";
	}
	snprintf(buf, size, "Exit %d", status);
	return buf;
}

/*
 * Process pid has ended with status st: its job, if it is in one, knows,
 * and is forgotten once all of it has ended -- said so first if `report`.
 */
void job_ended(struct sh *sh, int pid, int st, bool report)
{
	struct job *j = job_of_pid(sh, pid);
	bool all = true;
	char buf[16];

	if (!j)
		return;			/* not one it started as a job */
	for (int k = 0; k < j->n; k++) {
		if (j->pid[k] == pid) {
			j->done[k] = true;
			if (k == j->n - 1)
				j->status = st;
		}
		all &= j->done[k];
	}
	if (all) {
		if (report)
			job_report(j, how_it_ended(j->status, buf, sizeof(buf)));
		job_free(j);
	}
}

/* Before a prompt: what finished or stopped in the background, said. */
void jobs_reap(struct sh *sh)
{
	int st, pid;

	while ((pid = pt_wait(-1, &st, PT_WNOHANG | PT_WUNTRACED)) > 0) {
		struct job *j = job_of_pid(sh, pid);

		if (j && (st & PT_WSTOPPED)) {
			if (!j->stopped) {
				j->stopped = true;
				sh->current_job = j->id;
				job_report(j, "Stopped");
			}
		} else if (!(st & PT_WSTOPPED)) {
			job_ended(sh, pid, st, true);
		}
	}
}

bool jobs_stopped(struct sh *sh)
{
	for (int i = 0; i < MAX_JOBS; i++)
		if (sh->jobs[i].id && sh->jobs[i].stopped)
			return true;
	return false;
}

static int terminal(void)
{
	static const int fds[] = { PT_STDIN, PT_STDERR, PT_STDOUT };

	for (int i = 0; i < 3; i++)
		if (pt_isatty(fds[i]))
			return fds[i];
	return -1;
}

/*
 * A job in front: it gets the terminal, and is waited for until it ends
 * or stops. One that stops goes into the table (if it is not there yet)
 * and the shell takes the terminal back. `cont` sends SIGCONT first, for
 * fg. The status is the last process's, or with -o pipefail the last
 * that failed; 148 (128 + SIGTSTP) for a stop.
 */
int job_wait(struct sh *sh, struct job *j, bool cont)
{
	int tty = terminal(), failed = 0, sig = 0;

	if (tty >= 0)
		pt_ioctl(tty, PT_TTY_SETPGRP, &j->pgid);
	/*
	 * A game or a player runs its terminal raw; stopped, the shell's own
	 * line editing set it cooked again, and continued so it read keys a
	 * line at a time: q echoed instead of quitting. So it gets back the
	 * mode it had, as a shell restores a stopped job's terminal modes.
	 */
	if (cont && tty >= 0)
		pt_ioctl(tty, PT_TTY_SETRAW, &j->raw);
	for (int k = 0; cont && k < j->n; k++)
		if (!j->done[k])
			pt_kill(j->pid[k], PT_SIGCONT);
	j->stopped = false;
	for (int i = 0; i < j->n; i++) {
		int st = 0, r;

		if (j->done[i])
			continue;
		while ((r = pt_wait(j->pid[i], &st, PT_WUNTRACED)) == -EINTR) {
			/* a signal reached the shell: make sure the job has it too */
			sig = sh_take_signal();
			for (int k = i; k < j->n; k++)
				if (!j->done[k])
					pt_kill(j->pid[k], sig);
		}
		if (r > 0 && (st & PT_WSTOPPED)) {
			int raw = 0;

			if (tty >= 0) {
				pt_ioctl(tty, PT_TTY_GETRAW, &raw);
				pt_ioctl(tty, PT_TTY_SETPGRP, &sh->pgid);
			}
			if (!in_table(sh, j)) {
				struct job *kept = job_add(sh, j->pgid, j->pid, j->n, j->cmd,
							   strlen(j->cmd));

				if (kept)
					memcpy(kept->done, j->done, sizeof(kept->done));
				j = kept;
			}
			if (j) {
				j->stopped = true;
				j->raw = raw;
				sh->current_job = j->id;
				pt_puts("\n");
				job_report(j, "Stopped");
			}
			return 128 + PT_SIGTSTP;
		}
		j->done[i] = true;
		if (r > 0 && st)
			failed = st;
		if (i == j->n - 1)
			j->status = r > 0 ? st : 127;
	}
	if (tty >= 0)
		pt_ioctl(tty, PT_TTY_SETPGRP, &sh->pgid);
	int status = sh->pipefail && failed ? failed : j->status;

	if (in_table(sh, j))
		job_free(j);
	if (status == 128 + PT_SIGINT && !sig)
		sig = PT_SIGINT;		/* the job died of Ctrl-C: so does a script */
	if (sig)
		sh_signal(sh, sig);
	return status;
}

/*
 * kill %1 and wait %1: each %spec among the arguments becomes the pids of
 * that job. -1 after saying which spec named nothing.
 */
int jobs_expand(struct sh *sh, struct fields *args, const char *who)
{
	struct fields out = { 0 };

	for (int i = 0; i < args->n; i++) {
		if (!i || args->v[i][0] != '%') {
			fields_add(&out, args->v[i]);
			continue;
		}
		struct job *j = job_find(sh, args->v[i], who);

		if (!j) {
			fields_free(&out);
			return -1;
		}
		for (int k = 0; k < j->n; k++) {
			char pid[16];

			if (j->done[k])
				continue;
			snprintf(pid, sizeof(pid), "%d", j->pid[k]);
			fields_add(&out, pid);
		}
	}
	if (out.oom) {
		fields_free(&out);
		return -1;
	}
	fields_free(args);
	*args = out;
	return 0;
}

/* ------------------------------------------------------------ builtins */

int builtin_jobs(struct sh *sh, int argc, char **argv)
{
	jobs_reap(sh);
	for (int id = 1; id <= MAX_JOBS * 4; id++) {
		struct job *j = job_by_id(sh, id);

		if (j)
			pt_printf("[%d]%c %-12s %s\n", j->id, j->id == sh->current_job ? '+' : ' ',
				  j->stopped ? "Stopped" : "Running", j->cmd);
	}
	return 0;
}

int builtin_fg(struct sh *sh, int argc, char **argv)
{
	struct job *j;

	if (!sh->interactive) {
		pt_dprintf(PT_STDERR, "fg: no job control in a script\n");
		return 1;
	}
	if (!(j = job_find(sh, argc > 1 ? argv[1] : NULL, "fg")))
		return 1;
	pt_printf("%s\n", j->cmd);
	return job_wait(sh, j, j->stopped);
}

int builtin_bg(struct sh *sh, int argc, char **argv)
{
	struct job *j;

	if (!sh->interactive) {
		pt_dprintf(PT_STDERR, "bg: no job control in a script\n");
		return 1;
	}
	if (!(j = job_find(sh, argc > 1 ? argv[1] : NULL, "bg")))
		return 1;
	if (!j->stopped) {
		pt_dprintf(PT_STDERR, "bg: job %d is already running\n", j->id);
		return 0;
	}
	for (int k = 0; k < j->n; k++)
		if (!j->done[k])
			pt_kill(j->pid[k], PT_SIGCONT);
	j->stopped = false;
	pt_printf("[%d] %s &\n", j->id, j->cmd);
	return 0;
}
