#include <stdbool.h>
#include <sys/syscall.h>
#include "header.h"
#include "lib_perf.h"
#include "lib_thp.h"

#define max(a, b) (((a) < (b)) ? (b) : (a))

const char *USAGE = "Usage: %s -p 'regex of prog names' "
		    "-f <family; see main() for supported list>"
		    "[-i interval]\n";

unsigned long current_timestamp = 0;

/*
 * All fields must be updated inside this function.
 * return value indicates if the process should be added to
 * the list of candidate processes for further consideration.
 */
static bool update_process_stats(int pid, struct process *proc)
{
	int ret;

	proc->pid = pid;
	proc->timestamp = current_timestamp;
	ret = update_thp_usage(proc);
	if (ret != -1 && proc->anon_size > ELIGIBILITY_THRESHOLD) {
		if (update_translation_overhead(proc) == 0) {
			proc->skip = false;
			return true;
		}
	}
	proc->skip = true;
	return false;
}

/*
 * This function also profiles the current applications to measure
 * relevant memory informatation and translation overhead. A processes
 * is added to the list only if it should be considered in subsequent
 * iterations.
 */
void add_pid_to_list(int pid, struct process **head) {
	struct process *proc = *head, *new = NULL;

	if (!proc)
		goto allocate_process;

	while (proc) {
		if (proc->pid == pid)
			break;
		proc = proc->next;
	}

	/*
	 * If the process already exists in the list, no need to check
	 * the return value. Update its stats and return.
	 */
	if (proc) {
		update_process_stats(pid, proc);
		syscall(325, pid, max(1, (int)proc->overhead));
		return;
	}

allocate_process:
	/* pid not present in the list, allocate and add 1 to the head.
	 * Note that the order of processes does not matter for us.
	 * If we fail to add the process in this iteration, no need
	 * to panic as it will be given a chance in the next iteration.
	 */
	new = (struct process *) malloc(sizeof(struct process));
	if (!new)
		return;

	if (!update_process_stats(pid, new))
		return;

	new->next = *head;
	*head = new;
	syscall(325, pid, 1000);
	syscall(325, pid, max(1, (int)new->overhead));
}

/*
 * Some processes may no longer exist now. It is neither required
 * nor safe to keep such processes around. Hence, remove them at
 * the first possible opportunity.
 */
static void remove_expired_processes(struct process **head)
{
	struct process *prev, *curr, *del;

	prev = NULL;
	curr = *head;
	if (curr == NULL)
		return;

	while(curr) {
		if (curr->timestamp < current_timestamp) {
			del = curr;
			if (prev)
				prev->next = curr->next;
			else
				*head = curr->next;
			curr = curr->next;
			free(del);
		} else {
			prev = curr;
			curr = curr->next;
		}
	}
}

/*
 * This can be used, for example, to dump all process related
 * information for further analys. Currently, this is being
 * written to the terminal.
 * Currently, this is being used to check if the list implementation
 * is correct or not.
 */
static void log_process_info(struct process *head)
{
	struct process *proc = head;

	while (proc) {
		printf("PID: %6d THP_Required: %8d THP: %8d Overhead: %4d\n",
			proc->pid, proc->anon_size/2048, proc->anon_thp/2048,
			(int)proc->overhead);
		proc = proc->next;
	}
	/*
	 * Blank line to mark the end of current iteration.
	 * This is just to ease log analysis.
	 */
	//printf("\n");
}

static inline double get_process_weight(struct process *proc)
{
	/*
	 * Convert rss to MB to get noticeable value for the weight.
	 */
	double overhead = proc->overhead;
	double rss = (proc->anon_size - proc->anon_thp)/1024;

	if (rss <= 0)
		return -1;
	else
		return (overhead/rss)*1024;
}

static void update_candidate_process(struct process *head)
{
	struct process *best = NULL, *curr = head;
	double best_weight = 0, curr_weight;

	if (curr == NULL)
		return;
	/*
	 * For a process to be eligible in this iteration, it should
	 * not have been marked to skip recently. Its translation overhead
	 * should also be above the minimum threshold.
	 */
	while(curr) {
		if (curr->skip || curr->overhead < IS_CONSIDERABLE) {
			curr = curr->next;
			continue;
		}
		curr_weight = get_process_weight(curr);
		if (curr_weight > best_weight) {
			best = curr;
			best_weight = curr_weight;
		}
		curr = curr->next;
	}
	if (!best)
		return;
#if 0
	printf("Candidate PID: %d\tWeight: %lf \n\n", best->pid, best_weight);
#endif
}

static void profile_forever(char *prog_regex, int interval)
{
	FILE *fp;
	struct process *head = NULL;
	char line[LINELENGTH], command[LINELENGTH], tmp[LINELENGTH];

	sprintf(command, "ps aux | grep '%s'", prog_regex);
	while(true) {
		/* get all processes of the current user*/
		fp = popen(command, "r");
		if (fp == NULL) {
			printf("Could not get process list\n");
			exit(EXIT_FAILURE);
		}
		//fgets(line, 1900, fp);
		while (fgets(line, 1900, fp) != NULL) {
			int pid;

			/* Ignore background deamons */
			if (strstr(line, "sshd")
				|| strstr(line, "bash")
				|| strstr(line, "global_profile"))
				continue;

			sscanf(line, "%s %d", tmp, &pid);
			//printf("%s %d\n", tmp, pid);
			add_pid_to_list(pid, &head);
		}
		remove_expired_processes(&head);
		log_process_info(head);
		pclose(fp);
		sleep(interval);
		current_timestamp += 1;
	}
}

int make_daemon(void) {
    int fd;

    switch (fork()) {
    case -1:
        return (-1);
    case 0:
        break;
    default:
        _exit(EXIT_SUCCESS);
    }

    if (setsid() == -1)
        return (-1);

    return (0);
}

int main(int argc, char **argv)
{
	int c, interval = 10;
	bool is_skylake = false;
	char *prog_regex = NULL;
	char *family_str = NULL;
	enum ProcessorFamily family;
	bool daemonize = false;

	while ((c = getopt(argc, argv, "i:f:p:d")) != -1) {
		switch(c) {
			case 'i':
				interval = atoi(optarg);
				break;
			case 'f':
				family_str = optarg;
				break;
			case 'p':
				prog_regex = optarg;
				break;
			case 'd':
				daemonize = true;
				break;
			default:
				printf(USAGE, argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	if (prog_regex == NULL || family_str == NULL) {
		printf(USAGE, argv[0]);
		exit(EXIT_FAILURE);
	}

	/* Supported CPU microarchs. To add more, update lib_perf.h */
	if (strstr(family_str, "skylakesp")) {
		family = SkylakeScalable;
	} else if (strstr(family_str, "haswell")) {
		family = Haswell;
	} else {
		printf("Unknown machine type");
		exit(EXIT_FAILURE);
	}

	if (init_perf_event_masks(family)) {
		printf("Unknown machine type\n");
		exit(EXIT_FAILURE);
	}

	if (daemonize && make_daemon()) {
		printf("Unable to daemonize\n");
		exit(EXIT_FAILURE);
	}

	profile_forever(prog_regex, interval);
}
