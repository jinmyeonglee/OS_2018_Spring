#include "types.h"
#include "stat.h"
#include "user.h"

int
main (int argc, char *argv[])
{
	int pid;
	pid = fork();
	int i;
	for(i = 0; i< 100; i++) {

		if(pid == 0) { // child process
			printf(1, "Child\n");
			yield();
		}
		else { // parent process
			printf(1, "Parent\n");
			yield();
		}

	}
	exit();
}
