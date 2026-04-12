#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#define MAXPIPE 3
#define MAXLEN 1024

int parseCmds(char *str, char *result[], int cmdIndex[]) {
	// result is structured as {arg1, arg2, ..., NULL, arg1, arg2, ..., NULL}
	int cmds=0, wordCount=0;
	int spacing=1, breaking=1, quoting1=0, quoting2=0;
	int str_size = strlen(str);
	if (str_size == 0) return -1;
	for (int i=0; i<str_size; i++) {
		if (quoting1) {
			if (str[i] == '"') {
				quoting1 = 0;
				str[i] = '\0';
				spacing = 1;
			}
		} else if (quoting2) {
			if (str[i] == '\'') {
				quoting2 = 0;
				str[i] = '\0';
				spacing = 1;
			}
		} else if (str[i] == ' ') {
			if (!spacing) {
				str[i] = '\0';
				spacing = 1;
			}
		} else if (str[i] == '|') {
			// 2 '|' symbols inputted without a command in between 
			if (breaking) return -1;

			// Adding string separator
			if (!spacing) {
				str[i] = '\0';
				spacing = 1;
			}

			// Adding a null terminator
			result[wordCount++] = NULL;
			breaking = 1;
		} else if (spacing || breaking) {
			if (breaking) {
				if (cmds == MAXPIPE + 1) return -1;
				cmdIndex[cmds++] = wordCount;
				breaking = 0;
			}
			if (str[i] == '"') {
				result[wordCount++] = &str[i+1];
				quoting1 = 1;
			} else if (str[i] == '\'') {
				result[wordCount++] = &str[i+1];
				quoting2 = 1;
			} else
				result[wordCount++] = &str[i];
			spacing = 0;
		}
	}
	if (breaking) return -1;
	if (quoting1 || quoting2) return -1;
	result[wordCount] = NULL;
	return cmds;
}

void runCmds(char *args[], int cmdIndex[], int cmdCount) {
	if (cmdCount == 0) return;
	int prev_fd = -1;
	pid_t pids[cmdCount];
	
	for (int i=0; i<cmdCount; i++) {
		// Looping over the commands in order
		if (i < cmdCount-1) {
			// Creating a pipe
			int pipefd[2];
			if (pipe(pipefd) == -1) {
				perror("pipe");
				if (prev_fd != -1) close(prev_read);
				return;
			}
		}

		// Creating a child process
		pids[i] = fork();
		if (pids[i] < 0) {
			perror("fork");
			if (prev_fd != -1) close(prev_fd);
			if (i < cmdCount-1) {
				close(pipefd[0]);
				close(pipefd[1]);
			}
			return;
		}

		if (pids[i] == 0) {
			// Child process
			if (prev_fd != -1) {
				// Read from previous command output
				if (dup2(prev_fd, STDIN_FILENO) == -1) {
					perror("dup2");
					exit(1);
				}
			}
			
			if (i < cmdCount-1) {
				// Redirect output of current command to pipe
				close(pipefd[0]);
				if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
					perror("dup2");
					exit(1);
				}
				close(pipefd[1]);
			}

			// prev_fd has been redirected so it can be closed
			if (prev_fd != -1) close(prev_fd);
}

int main() {
	