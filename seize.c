#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

#include "compiler.h"
#include "cr_options.h"
#include "cr-errno.h"
#include "pstree.h"
#include "ptrace.h"
#include "seize.h"
#include "stats.h"
#include "xmalloc.h"
#include "util.h"

#define NR_ATTEMPTS 5

static inline bool child_collected(struct pstree_item *i, pid_t pid)
{
	struct pstree_item *c;

	list_for_each_entry(c, &i->children, sibling)
		if (c->pid.real == pid)
			return true;

	return false;
}

static int collect_task(struct pstree_item *item);
static int collect_children(struct pstree_item *item)
{
	pid_t *ch;
	int ret, i, nr_children, nr_inprogress;

	ret = parse_children(item->pid.real, &ch, &nr_children);
	if (ret < 0)
		return ret;

	nr_inprogress = 0;
	for (i = 0; i < nr_children; i++) {
		struct pstree_item *c;
		pid_t pid = ch[i];

		/* Is it already frozen? */
		if (child_collected(item, pid))
			continue;

		nr_inprogress++;

		pr_info("Seized task %d, state %d\n", pid, ret);

		c = alloc_pstree_item();
		if (c == NULL) {
			ret = -1;
			goto free;
		}

		/* fails when meets a zombie */
		seize_catch_task(pid);

		ret = seize_wait_task(pid, item->pid.real, &dmpi(c)->pi_creds);
		if (ret < 0) {
			/*
			 * Here is a race window between parse_children() and seize(),
			 * so the task could die for these time.
			 * Don't worry, will try again on the next attempt. The number
			 * of attempts is restricted, so it will exit if something
			 * really wrong.
			 */
			ret = 0;
			xfree(c);
			continue;
		}

		c->pid.real = pid;
		c->parent = item;
		c->state = ret;
		list_add_tail(&c->sibling, &item->children);

		/* Here is a recursive call (Depth-first search) */
		ret = collect_task(c);
		if (ret < 0)
			goto free;
	}
free:
	xfree(ch);
	return ret < 0 ? ret : nr_inprogress;
}

static void unseize_task_and_threads(const struct pstree_item *item, int st)
{
	int i;

	if (item->state == TASK_DEAD)
		return;

	/*
	 * The st is the state we want to switch tasks into,
	 * the item->state is the state task was in when we seized one.
	 */

	unseize_task(item->pid.real, item->state, st);

	if (st == TASK_DEAD)
		return;

	for (i = 1; i < item->nr_threads; i++)
		if (ptrace(PTRACE_DETACH, item->threads[i].real, NULL, NULL))
			pr_perror("Unable to detach from %d", item->threads[i].real);
}

static void pstree_wait(struct pstree_item *root_item)
{
	struct pstree_item *item = root_item;
	int pid, status, i;

	for_each_pstree_item(item) {

		if (item->state == TASK_DEAD)
			continue;

		for (i = 0; i < item->nr_threads; i++) {
			pid = wait4(-1, &status, __WALL, NULL);
			if (pid < 0) {
				pr_perror("wait4 failed");
				break;
			} else {
				if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
					pr_err("Unexpected exit code %d of %d\n", status, pid);
					BUG();
				}
			}
		}
	}
	pid = wait4(-1, &status, __WALL, NULL);
	if (pid > 0) {
		pr_err("Unexpected child %d", pid);
		BUG();
	}
}

void pstree_switch_state(struct pstree_item *root_item, int st)
{
	struct pstree_item *item = root_item;

	pr_info("Unfreezing tasks into %d\n", st);
	for_each_pstree_item(item)
		unseize_task_and_threads(item, st);

	if (st == TASK_DEAD)
		pstree_wait(root_item);
}

static pid_t item_ppid(const struct pstree_item *item)
{
	item = item->parent;
	return item ? item->pid.real : -1;
}

static inline bool thread_collected(struct pstree_item *i, pid_t tid)
{
	int t;

	if (i->pid.real == tid) /* thread leader is collected as task */
		return true;

	for (t = 0; t < i->nr_threads; t++)
		if (tid == i->threads[t].real)
			return true;

	return false;
}

static int collect_threads(struct pstree_item *item)
{
	struct pid *threads = NULL;
	int nr_threads = 0, i = 0, ret, nr_inprogress, nr_stopped = 0;

	ret = parse_threads(item->pid.real, &threads, &nr_threads);
	if (ret < 0)
		goto err;

	if ((item->state == TASK_DEAD) && (nr_threads > 1)) {
		pr_err("Zombies with threads are not supported\n");
		goto err;
	}

	/* The number of threads can't be less than allready frozen */
	item->threads = xrealloc(item->threads, nr_threads * sizeof(struct pid));
	if (item->threads == NULL)
		return -1;

	if (item->nr_threads == 0) {
		item->threads[0].real = item->pid.real;
		item->nr_threads = 1;
	}

	nr_inprogress = 0;
	for (i = 0; i < nr_threads; i++) {
		pid_t pid = threads[i].real;

		if (thread_collected(item, pid))
			continue;

		nr_inprogress++;

		pr_info("\tSeizing %d's %d thread\n",
				item->pid.real, pid);

		if (seize_catch_task(pid))
			continue;

		ret = seize_wait_task(pid, item_ppid(item), &dmpi(item)->pi_creds);
		if (ret < 0) {
			/*
			 * Here is a race window between parse_threads() and seize(),
			 * so the task could die for these time.
			 * Don't worry, will try again on the next attempt. The number
			 * of attempts is restricted, so it will exit if something
			 * really wrong.
			 */
			continue;
		}

		BUG_ON(item->nr_threads + 1 > nr_threads);
		item->threads[item->nr_threads].real = pid;
		item->nr_threads++;

		if (ret == TASK_DEAD) {
			pr_err("Zombie thread not supported\n");
			goto err;
		}

		if (ret == TASK_STOPPED) {
			nr_stopped++;
		}
	}

	if (nr_stopped && nr_stopped != nr_inprogress) {
		pr_err("Individually stopped threads not supported\n");
		goto err;
	}

	xfree(threads);
	return nr_inprogress;

err:
	xfree(threads);
	return -1;
}

static int collect_loop(struct pstree_item *item,
		int (*collect)(struct pstree_item *))
{
	int attempts = NR_ATTEMPTS, nr_inprogress = 1;

	/*
	 * While we scan the proc and seize the children/threads
	 * new ones can appear (with clone(CLONE_PARENT) or with
	 * pthread_create). Thus, after one go, we need to repeat
	 * the scan-and-freeze again collecting new arrivals. As
	 * new guys may appear again we do NR_ATTEMPTS passes and
	 * fail to seize the item if new tasks/threads still
	 * appear.
	 */

	while (nr_inprogress > 0 && attempts) {
		attempts--;
		nr_inprogress = collect(item);
	}

	/*
	 * We may fail to collect items or run out of attempts.
	 * In the former case nr_inprogress will be negative, in
	 * the latter -- positive. Thus it's enough just to check
	 * for "no more new stuff" and say "we're OK" if so.
	 */

	return (nr_inprogress == 0) ? 0 : -1;
}

static int collect_task(struct pstree_item *item)
{
	int ret;

	ret = collect_loop(item, collect_threads);
	if (ret < 0)
		goto err_close;

	/* Depth-first search (DFS) is used for traversing a process tree. */
	ret = collect_loop(item, collect_children);
	if (ret < 0)
		goto err_close;

	if ((item->state == TASK_DEAD) && !list_empty(&item->children)) {
		pr_err("Zombie with children?! O_o Run, run, run!\n");
		goto err_close;
	}

	if (pstree_alloc_cores(item))
		goto err_close;

	pr_info("Collected %d in %d state\n", item->pid.real, item->state);
	return 0;

err_close:
	close_pid_proc();
	return -1;
}

int collect_pstree(pid_t pid)
{
	int ret;

	timing_start(TIME_FREEZING);

	root_item = alloc_pstree_item();
	if (root_item == NULL)
		return -1;

	root_item->pid.real = pid;

	if (seize_catch_task(pid)) {
		set_cr_errno(ESRCH);
		goto err;
	}

	ret = seize_wait_task(pid, -1, &dmpi(root_item)->pi_creds);
	if (ret < 0)
		goto err;
	pr_info("Seized task %d, state %d\n", pid, ret);
	root_item->state = ret;

	ret = collect_task(root_item);
	if (ret < 0)
		goto err;

	timing_stop(TIME_FREEZING);
	timing_start(TIME_FROZEN);

	return 0;
err:
	pstree_switch_state(root_item, TASK_ALIVE);
	return -1;
}

