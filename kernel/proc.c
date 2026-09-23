/*
 * Processes.
 *
 * A process is a FreeRTOS task pinned to core 1 plus the Unix bookkeeping
 * around it: pid and parent, process group, fd table, working directory,
 * environment, argv, tracked allocations and pending signals. Drivers and
 * the display run on core 0, so a program stuck in a loop cannot freeze the
 * screen or the keyboard.
 *
 * Signals are delivered at safe points, when the process enters a system
 * call. SIGKILL gets half a second to be noticed; after that the reaper
 * deletes the task outright, which can leak whatever it was holding.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "sdkconfig.h"

#include "pt/kernel.h"
#include "pt/program.h"

#define PROC_PRIORITY		2
#define PROC_CORE		1
#define KILL_GRACE_US		500000
#define LOADER_STACK_KB		12
#define DEFAULT_PATH		"/bin:/home/" CONFIG_PT_USERNAME "/bin"
#define SIGMASK(sig)		(1u << (sig))
#define STOPS			(SIGMASK(PT_SIGSTOP) | SIGMASK(PT_SIGTSTP))
#define ENDS			(SIGMASK(PT_SIGINT) | SIGMASK(PT_SIGTERM) | SIGMASK(PT_SIGKILL))

static struct proc	 procs[CONFIG_PT_MAX_PROCS];
static SemaphoreHandle_t table_lock;
static int		 next_pid = 1;
static struct pt_program *programs;
static struct pt_loader	 *loaders;

#define LOCK()		xSemaphoreTake(table_lock, portMAX_DELAY)
#define UNLOCK()	xSemaphoreGive(table_lock)

/* ------------------------------------------------------------ registries */

void program_register(struct pt_program *prog)
{
	struct pt_program **pp = &programs;

	while (*pp && strcmp((*pp)->name, prog->name) < 0)
		pp = &(*pp)->next;
	prog->next = *pp;
	*pp = prog;
}

const struct pt_program *program_find(const char *name)
{
	for (struct pt_program *p = programs; p; p = p->next)
		if (!strcmp(p->name, name))
			return p;
	return NULL;
}

const struct pt_program *program_first(void)
{
	return programs;
}

void loader_register(struct pt_loader *loader)
{
	loader->next = loaders;
	loaders = loader;
}

const struct pt_loader *loader_find(const char *path)
{
	uint8_t head[64];
	char vfs[PT_PATH_MAX + 16];

	if (!mount_resolve(path, vfs, sizeof(vfs)))
		return NULL;
	int fd = open(vfs, O_RDONLY);

	if (fd < 0)
		return NULL;
	ssize_t n = read(fd, head, sizeof(head));
	close(fd);
	for (struct pt_loader *l = loaders; l; l = l->next)
		if (l->probe(path, head, n > 0 ? n : 0))
			return l;
	return NULL;
}

/* ------------------------------------------------------------ environment */

static const char *env_find(const char *env, const char *name)
{
	size_t len = strlen(name);

	for (const char *e = env; e && *e; e += strlen(e) + 1)
		if (!strncmp(e, name, len) && e[len] == '=')
			return e + len + 1;
	return NULL;
}

/* Rebuild the block without `name`, then append name=value if given. */
static int env_update(struct proc *p, const char *name, const char *value)
{
	size_t nlen = strlen(name);
	size_t vlen = value ? strlen(value) : 0;
	char *env = malloc(p->env_len + nlen + vlen + 3);
	char *o = env;

	if (!env)
		return -ENOMEM;
	for (const char *e = p->env; e && *e; e += strlen(e) + 1) {
		size_t len = strlen(e) + 1;
		if (!strncmp(e, name, nlen) && e[nlen] == '=')
			continue;
		memcpy(o, e, len);
		o += len;
	}
	if (value) {
		memcpy(o, name, nlen);
		o += nlen;
		*o++ = '=';
		memcpy(o, value, vlen + 1);
		o += vlen + 1;
	}
	*o++ = '\0';
	free(p->env);
	p->env = env;
	p->env_len = o - env;
	return 0;
}

const char *pt_getenv(const char *name)
{
	struct proc *p = proc_current();

	return p ? env_find(p->env, name) : NULL;
}

int pt_setenv(const char *name, const char *value)
{
	struct proc *p = proc_current();

	if (!name || !*name || strchr(name, '=') || !value)
		return -EINVAL;
	return p ? env_update(p, name, value) : -EPERM;
}

int pt_unsetenv(const char *name)
{
	struct proc *p = proc_current();

	if (!name || !*name)
		return -EINVAL;
	return p ? env_update(p, name, NULL) : -EPERM;
}

int pt_environ(int index, const char **entry)
{
	struct proc *p = proc_current();
	const char *e = p ? p->env : NULL;

	for (int i = 0; e && *e; e += strlen(e) + 1, i++) {
		if (i == index) {
			*entry = e;
			return 1;
		}
	}
	return 0;
}

static char *env_default(size_t *len)
{
	char buf[512];
	int n = snprintf(buf, sizeof(buf),
			 "HOME=/home/%s%cUSER=%s%cHOSTNAME=%s%cPATH=%s%c"
			 "SHELL=sh%cTERM=vt100%cTZ=%s%c",
			 CONFIG_PT_USERNAME, 0, CONFIG_PT_USERNAME, 0, CONFIG_PT_HOSTNAME, 0,
			 DEFAULT_PATH, 0, 0, 0, CONFIG_PT_TZ, 0);
	char *env = malloc(n + 1);

	if (env) {
		memcpy(env, buf, n);
		env[n] = '\0';
		*len = n + 1;
	}
	return env;
}

/* ------------------------------------------------------------ lookup */

struct proc *proc_current(void)
{
	TaskHandle_t self = xTaskGetCurrentTaskHandle();

	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++)
		if (procs[i].state == PROC_RUNNING && procs[i].task == self)
			return &procs[i];
	return NULL;
}

int pt_getpid(void)
{
	struct proc *p = proc_current();

	return p ? p->pid : 0;
}

int proc_count(void)
{
	int n = 0;

	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++)
		n += procs[i].state == PROC_RUNNING;
	return n;
}

int proc_list(struct pt_procinfo *out, int max)
{
	int n = 0;

	LOCK();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS && n < max; i++) {
		struct proc *p = &procs[i];
		TaskStatus_t task = { 0 };

		if (p->state == PROC_FREE)
			continue;
		if (p->task)
			vTaskGetInfo(p->task, &task, pdFALSE, eInvalid);
		out[n] = (struct pt_procinfo) {
			.pid = p->pid, .ppid = p->ppid, .pgid = p->pgid,
			.state = p->state != PROC_RUNNING ? 'Z' : p->stopped ? 'T' : 'R',
			.stack_kb = p->stack_kb, .start_us = p->start_us,
			.stack_free = p->task ? uxTaskGetStackHighWaterMark(p->task) : 0,
			.cpu_us = task.ulRunTimeCounter,
		};
		strlcpy(out[n].name, p->name, sizeof(out[n].name));
		n++;
	}
	UNLOCK();
	return n;
}

bool proc_alive(int pid)
{
	bool alive = false;

	LOCK();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS && !alive; i++)
		alive = procs[i].state == PROC_RUNNING && procs[i].pid == pid;
	UNLOCK();
	return alive;
}

/* ------------------------------------------------------------ exit */

static void deliver(struct proc *p, int sig);

static void teardown(struct proc *p, int status)
{
	LOCK();
	atomic_store(&p->exiting, true);
	UNLOCK();

	for (int i = 0; i < PT_MAX_FDS; i++) {
		file_put(p->fd[i]);
		p->fd[i] = NULL;
	}
	dir_release_all(p);
	mem_release_all(p);
	free(p->env);
	free(p->args);
	p->env = p->args = NULL;

	LOCK();
	struct proc *parent = NULL;
	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++) {
		struct proc *q = &procs[i];
		if (q->state == PROC_FREE || q == p)
			continue;
		if (q->ppid == p->pid) {
			q->ppid = 0;			/* orphans are reaped on exit */
			if (q->state == PROC_ZOMBIE)
				q->state = PROC_FREE;
			/* nobody is left to continue a stopped one: it ends */
			if (q->state == PROC_RUNNING && q->stopped)
				deliver(q, PT_SIGTERM);
		}
		if (q->pid == p->ppid && q->state == PROC_RUNNING)
			parent = q;
	}
	p->status = status;
	p->task = NULL;
	p->kill_deadline_us = 0;
	xSemaphoreGive(p->exited);
	p->state = parent ? PROC_ZOMBIE : PROC_FREE;
	UNLOCK();
}

void pt_exit(int status)
{
	struct proc *p = proc_current();

	if (p)
		teardown(p, status);
	vTaskDelete(NULL);
	for (;;)
		vTaskDelay(portMAX_DELAY);
}

int pt_wait(int pid, int *status, int flags)
{
	proc_check_signals();
	struct proc *self = proc_current();

	if (!self)
		return -EPERM;
	for (;;) {
		struct proc *child = NULL;

		LOCK();
		for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++) {
			struct proc *q = &procs[i];
			if (q->state == PROC_FREE || q->ppid != self->pid || (pid > 0 && q->pid != pid))
				continue;
			/* one with news first: finished, or stopped if that is asked about */
			bool news = q->state == PROC_ZOMBIE ||
				    ((flags & PT_WUNTRACED) && q->stopped && !q->stop_reported);

			if (!child || news)
				child = q;
			if (news)
				break;
		}
		if (!child) {
			UNLOCK();
			return -ECHILD;
		}
		if (child->state == PROC_ZOMBIE) {
			int got = child->pid;
			if (status)
				*status = child->status;
			child->state = PROC_FREE;
			UNLOCK();
			return got;
		}
		if ((flags & PT_WUNTRACED) && child->stopped && !child->stop_reported) {
			int got = child->pid;

			child->stop_reported = true;
			if (status)
				*status = PT_WSTOPPED | child->stop_sig;
			UNLOCK();
			return got;
		}
		SemaphoreHandle_t exited = child->exited;
		UNLOCK();

		if (flags & PT_WNOHANG)
			return 0;
		if (pid > 0)
			xSemaphoreTake(exited, pdMS_TO_TICKS(50));
		else
			vTaskDelay(pdMS_TO_TICKS(20));
		proc_stop_point();		/* the one waiting may be stopped itself */
		if (pt_interrupted())
			return -EINTR;
	}
}

/* ------------------------------------------------------------ spawn */

static void trampoline(void *arg)
{
	struct proc *p = arg;
	int status;

	p->task = xTaskGetCurrentTaskHandle();
	if (p->prog)
		status = p->prog->main(p->argc, p->argv);
	else
		status = p->loader->exec(p->exec_path, p->argc, p->argv);
	pt_exit(status);
}

static int try_exec(const char *path, const struct pt_loader **loader)
{
	struct pt_stat st;

	if (pt_stat(path, &st))
		return -ENOENT;
	if (st.is_dir)
		return -EISDIR;
	*loader = loader_find(path);
	return *loader ? 0 : -ENOEXEC;
}

static int resolve(const char *cmd, const char *cwd, const char *env,
		   const struct pt_program **prog, const struct pt_loader **loader, char *path)
{
	*prog = NULL;
	*loader = NULL;
	path[0] = '\0';	/* a built-in has none, and spawn copies it all the same */

	if (strchr(cmd, '/')) {
		int err = path_normalize(cwd, cmd, path, PT_PATH_MAX);
		return err ? err : try_exec(path, loader);
	}
	*prog = program_find(cmd);
	if (*prog)
		return 0;

	const char *search = env_find(env, "PATH");
	int err = -ENOENT;

	for (const char *dir = search ? search : DEFAULT_PATH; *dir;) {
		size_t len = strcspn(dir, ":");
		if (len && (size_t)snprintf(path, PT_PATH_MAX, "%.*s/%s", (int)len, dir, cmd) < PT_PATH_MAX) {
			int e = try_exec(path, loader);
			if (!e)
				return 0;
			if (e != -ENOENT)
				err = e;
		}
		dir += len + (dir[len] == ':');
	}
	return err;
}

static int spawn(struct proc *parent, const char *cmd, int argc, char *const *argv,
		 struct pt_file *files[3], int pgid, const char *cwd,
		 const char *env, size_t env_len)
{
	const struct pt_program *prog;
	const struct pt_loader *loader;
	char path[PT_PATH_MAX];
	int err = resolve(cmd, cwd, env, &prog, &loader, path);

	if (err)
		return err;

	/* argv pointers, their strings and the executable path in one block */
	size_t size = (argc + 1) * sizeof(char *) + strlen(path) + 1;
	for (int i = 0; i < argc; i++)
		size += strlen(argv[i]) + 1;
	char *args = malloc(size);
	char *envcopy = malloc(env_len);
	if (!args || !envcopy) {
		free(args);
		free(envcopy);
		return -ENOMEM;
	}
	memcpy(envcopy, env, env_len);

	LOCK();
	struct proc *p = NULL;
	for (int i = 0; i < CONFIG_PT_MAX_PROCS && !p; i++)
		if (procs[i].state == PROC_FREE)
			p = &procs[i];
	if (!p) {
		UNLOCK();
		free(args);
		free(envcopy);
		return -EAGAIN;
	}
	SemaphoreHandle_t exited = p->exited, cont = p->cont;
	memset(p, 0, sizeof(*p));
	p->exited = exited;
	p->cont = cont;
	xSemaphoreTake(p->exited, 0);
	xSemaphoreTake(p->cont, 0);
	p->pid = next_pid++;
	p->ppid = parent ? parent->pid : 0;
	p->pgid = pgid > 0 ? pgid : p->pid;
	p->state = PROC_RUNNING;
	UNLOCK();

	char **av = (char **)args;
	char *s = args + (argc + 1) * sizeof(char *);
	for (int i = 0; i < argc; i++) {
		av[i] = s;
		s = stpcpy(s, argv[i]) + 1;
	}
	av[argc] = NULL;
	strcpy(s, path);

	p->args = args;
	p->argc = argc;
	p->argv = av;
	p->exec_path = s;
	p->env = envcopy;
	p->env_len = env_len;
	p->prog = prog;
	p->loader = loader;
	p->start_us = esp_timer_get_time();
	strlcpy(p->name, prog ? prog->name : path_basename(path), sizeof(p->name));
	strlcpy(p->cwd, cwd, sizeof(p->cwd));
	for (int i = 0; i < 3; i++)
		p->fd[i] = file_get(files[i]);
	p->stack_kb = prog && prog->stack_kb ? prog->stack_kb :
		      loader ? (loader->stack_kb ? loader->stack_kb : LOADER_STACK_KB) :
		      CONFIG_PT_PROC_STACK_KB;

	int pid = p->pid;
	TaskHandle_t task;
	BaseType_t made = pdFAIL;

	/*
	 * A process that has exited leaves its stack to be freed by the
	 * idle task, so a program that spawns in a tight loop can run the
	 * heap down before that happens. Give the idle task a moment and
	 * try again rather than failing something that is only briefly
	 * out of memory.
	 */
	for (int attempt = 0; attempt < 4 && made != pdPASS; attempt++) {
		if (attempt)
			vTaskDelay(pdMS_TO_TICKS(2));
		made = xTaskCreatePinnedToCore(trampoline, p->name, p->stack_kb * 1024, p,
					       PROC_PRIORITY, &task, PROC_CORE);
	}
	if (made != pdPASS) {
		teardown(p, -ENOMEM);
		LOCK();
		p->state = PROC_FREE;
		UNLOCK();
		return -ENOMEM;
	}
	return pid;
}

int pt_spawn(const struct pt_spawn *req)
{
	proc_check_signals();
	struct proc *self = proc_current();
	struct pt_file *files[3];

	if (!self)
		return -EPERM;
	if (!req || !req->cmd || req->argc < 1 || !req->argv)
		return -EINVAL;
	for (int i = 0; i < 3; i++) {
		int fd = req->fd[i] < 0 ? i : req->fd[i];
		files[i] = fd < PT_MAX_FDS ? self->fd[fd] : NULL;
	}
	return spawn(self, req->cmd, req->argc, req->argv, files, req->pgid,
		     self->cwd, self->env, self->env_len);
}

int proc_spawn_console(int argc, char **argv, struct pt_file *console)
{
	struct pt_file *files[3] = { console, console, console };
	size_t env_len = 0;
	char *env = env_default(&env_len);
	struct pt_stat st;
	char home[PT_PATH_MAX];

	if (!env)
		return -ENOMEM;
	snprintf(home, sizeof(home), "/home/%s", CONFIG_PT_USERNAME);
	const char *cwd = pt_stat(home, &st) == 0 && st.is_dir ? home : "/";
	int pid = spawn(NULL, argv[0], argc, argv, files, 0, cwd, env, env_len);

	free(env);
	return pid;
}

int proc_wait_orphan(int pid)
{
	SemaphoreHandle_t exited = NULL;
	struct proc *p = NULL;

	LOCK();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS && !p; i++)
		if (procs[i].pid == pid)
			p = &procs[i];
	if (p)
		exited = p->exited;
	UNLOCK();
	if (!p)
		return -ECHILD;
	xSemaphoreTake(exited, portMAX_DELAY);
	return p->status;
}

/* ------------------------------------------------------------ signals */

/* Called with the table locked, which is what orders it against a stop. */
static void deliver(struct proc *p, int sig)
{
	if (sig == PT_SIGCONT) {
		atomic_fetch_and(&p->sigpending, ~STOPS);
		if (p->stopped)
			xSemaphoreGive(p->cont);
		return;
	}
	atomic_fetch_or(&p->sigpending, SIGMASK(sig));
	if (sig == PT_SIGKILL && !p->kill_deadline_us)
		p->kill_deadline_us = esp_timer_get_time() + KILL_GRACE_US;
	/* a stopped process wakes to be ended */
	if ((SIGMASK(sig) & ENDS) && p->stopped)
		xSemaphoreGive(p->cont);
}

/*
 * A stop: the process sleeps here until SIGCONT, or until a signal that
 * ends it wakes it to act on that. Only ever at a point holding no locks
 * -- a system call's entry, or proc_stop_point() -- so nothing waits on a
 * lock a stopped process has.
 */
static void stop(struct proc *p)
{
	LOCK();
	if (!(atomic_load(&p->sigpending) & STOPS)) {
		UNLOCK();			/* a SIGCONT got here first */
		return;
	}
	p->stop_sig = atomic_load(&p->sigpending) & SIGMASK(PT_SIGSTOP) ? PT_SIGSTOP : PT_SIGTSTP;
	atomic_fetch_and(&p->sigpending, ~STOPS);
	xSemaphoreTake(p->cont, 0);
	p->stopped = true;
	p->stop_reported = false;
	UNLOCK();
	while (xSemaphoreTake(p->cont, pdMS_TO_TICKS(1000)) != pdTRUE &&
	       !(atomic_load(&p->sigpending) & ENDS))
		;
	LOCK();
	p->stopped = false;
	UNLOCK();
}

bool proc_stop_pending(void)
{
	struct proc *p = proc_current();

	return p && (atomic_load(&p->sigpending) & STOPS);
}

void proc_stop_point(void)
{
	struct proc *p = proc_current();

	if (p && !atomic_load(&p->exiting) && (atomic_load(&p->sigpending) & STOPS))
		stop(p);
}

int pt_kill(int pid, int sig)
{
	proc_check_signals();
	if (sig != PT_SIGINT && sig != PT_SIGTERM && sig != PT_SIGKILL && sig != PT_SIGCONT &&
	    sig != PT_SIGSTOP && sig != PT_SIGTSTP)
		return -EINVAL;

	int err = -ESRCH;
	LOCK();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++) {
		if (procs[i].state == PROC_RUNNING && procs[i].pid == pid) {
			deliver(&procs[i], sig);
			err = 0;
		}
	}
	UNLOCK();
	return err;
}

void proc_signal_group(int pgid, int sig)
{
	LOCK();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++)
		if (procs[i].state == PROC_RUNNING && procs[i].pgid == pgid)
			deliver(&procs[i], sig);
	UNLOCK();
}

void proc_check_signals(void)
{
	struct proc *p = proc_current();

	if (!p || atomic_load(&p->exiting))
		return;
	unsigned pending = atomic_load(&p->sigpending);
	if (pending & SIGMASK(PT_SIGKILL))
		pt_exit(128 + PT_SIGKILL);
	if (!p->sigcatch && (pending & SIGMASK(PT_SIGINT)))
		pt_exit(128 + PT_SIGINT);
	if (!p->sigcatch && (pending & SIGMASK(PT_SIGTERM)))
		pt_exit(128 + PT_SIGTERM);
	if (pending & STOPS) {
		stop(p);
		proc_check_signals();		/* what woke it may be what ends it */
	}
}

unsigned pt_sigpending(void)
{
	struct proc *p = proc_current();

	return p ? atomic_load(&p->sigpending) : 0;
}

bool pt_interrupted(void)
{
	struct proc *p = proc_current();
	const unsigned mask = SIGMASK(PT_SIGINT) | SIGMASK(PT_SIGTERM) | SIGMASK(PT_SIGKILL);

	return p && (atomic_load(&p->sigpending) & mask);
}

void pt_sigcatch(bool on)
{
	struct proc *p = proc_current();

	if (!p)
		return;
	p->sigcatch = on;
	if (on)
		atomic_fetch_and(&p->sigpending, ~(SIGMASK(PT_SIGINT) | SIGMASK(PT_SIGTERM)));
	else
		proc_check_signals();
}

/* Deletes processes that ignored SIGKILL past their grace period. */
static void reaper(void *arg)
{
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(100));
		int64_t now = esp_timer_get_time();

		for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++) {
			struct proc *p = &procs[i];
			bool forced = false;

			LOCK();
			if (p->state == PROC_RUNNING && p->kill_deadline_us && now > p->kill_deadline_us &&
			    !atomic_load(&p->exiting) && p->task) {
				vTaskDelete(p->task);
				p->task = NULL;
				forced = true;
			}
			UNLOCK();
			if (forced) {
				klog("kill: pid %d (%s) ignored SIGKILL, task deleted", p->pid, p->name);
				teardown(p, 128 + PT_SIGKILL);
			}
		}
	}
}

void proc_init(void)
{
	table_lock = xSemaphoreCreateMutex();
	for (int i = 0; i < CONFIG_PT_MAX_PROCS; i++) {
		procs[i].exited = xSemaphoreCreateBinary();
		procs[i].cont = xSemaphoreCreateBinary();
	}
	/* 4 KB: after a forced kill the reaper closes the program's files
	 * itself, and one with unwritten data is a write down through FAT
	 * and the card driver. */
	xTaskCreatePinnedToCore(reaper, "kreaper", 4096, NULL, 5, NULL, 0);
	klog("proc: %d process slots, programs on core %d", CONFIG_PT_MAX_PROCS, PROC_CORE);
}
