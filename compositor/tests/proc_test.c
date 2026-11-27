/* The password helper answers only the desktop that started it. This checks the
 * rule on its own: run from a shell the parent is something else, and run from
 * a child of ourselves the parent is the same executable - which is the shape
 * the desktop makes when it forks and execs /proc/self/exe. */
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/proc.h"

int main(void) {
	int failures = 0;
	if (proc_parent_is_self()) {
		printf("FAIL: a parent that is not us was accepted\n");
		failures++;
	}
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0) {
		printf("FAIL: could not fork\n");
		return 1;
	}
	if (pid == 0) {
		_exit(proc_parent_is_self() ? 0 : 1);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL: a parent that is this same program was refused\n");
		failures++;
	}
	if (failures == 0) {
		printf("only our own parent is accepted\n");
	}
	return failures ? 1 : 0;
}
