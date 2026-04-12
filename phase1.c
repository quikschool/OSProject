#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <pwd.h>

#define MAXPIPE 3
#define MAXLEN 4096

int write_fd(int fd, char *str, int len) {
	while (len > 0) {
		int n = write(fd, str, len);
		if (n < 0) return -1;
		str += n;
		len -= n;
	}
	return 0;
}

int copy_fd(int in, int out) {
	char buf[MAXLEN];
	while (1) {
		int n = read(in, buf, sizeof(buf));
		if (n < 0) return -1;
		if (n == 0) break;
		if (write_fd(out, buf, n) != 0) return 1;
	}
	return 0;
}

void builtin_pwd(char **args) {
	char buffer[PATH_MAX];
	if (getcwd(buffer, sizeof(buffer)) == NULL) {
		perror("pwd");
		return;
	}
	puts(buffer);
}

void builtin_cd(char **args) {
	char *path = args[1];

	if (path == NULL) {
		path = getenv("HOME");
		if (path == NULL) path = "/";
	}

	if (chdir(path) != 0) perror("cd");
}

void builtin_echo(char **args) {
	for (int i=1; args[i] != NULL; i++) {
		if (write_fd(STDOUT_FILENO, args[i], strlen(args[i])) != 0) {
			perror("echo");
			return;
		}
		if (args[i + 1] != NULL) {
			if (write_fd(STDOUT_FILENO, " ", 1) != 0) {
				perror("echo");
				return;
			}
		}
	}
	if (write_fd(STDOUT_FILENO, "\n", 1) != 0) perror("echo");
}

void builtin_mkdir(char **args) {
	if (args[1] == NULL) {
		fprintf(stderr, "mkdir: missing operand\n");
		return;
	}

	for (int i=1; args[i] != NULL; i++) {
		if (mkdir(args[i], 0777) != 0) {
			perror("mkdir");
			return;
		}
	}
}

void builtin_rmdir(char **args) {
	if (args[1] == NULL) {
		fprintf(stderr, "rmdir: missing operand\n");
		return;
	}

	for (int i=1; args[i] != NULL; i++) {
		if (rmdir(args[i]) != 0) {
			perror("rmdir");
			return;
		}
	}
}

void builtin_rm(char **args) {
	if (args[1] == NULL) {
		fprintf(stderr, "rm: missing operand\n");
		return;
	}

	for (int i=1; args[i] != NULL; i++) {
		if (unlink(args[i]) != 0) {
			perror("rm");
			return;
		}
	}
}

void builtin_touch(char **args) {
	if (args[1] == NULL) {
		fprintf(stderr, "touch: missing file operand\n");
		return;
	}

	for (int i=1; args[i] != NULL; i++) {
		int fd = open(args[i], O_WRONLY | O_CREAT, 0666);
		if (fd < 0) {
			perror("touch");
			return;
		}
		close(fd);
	}
}

void builtin_mv(char **args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		fprintf(stderr, "mv usage syntax: mv SOURCE DESTINATION\n");
		return;
	}

	if (rename(args[1], args[2]) != 0) perror("mv");
}

void builtin_cp(char **args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		fprintf(stderr, "cp usage syntax: cp SOURCE DESTINATION\n");
		return;
	}

	int in = open(args[1], O_RDONLY);
	if (in < 0) {
		perror("cp");
		return;
	}

	int out = open(args[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0) {
		perror("cp");
		close(in);
		return;
	}

	int rc = copy_fd(in, out);
	if (rc != 0) perror("cp");

	close(in);
	close(out);
}

void builtin_cat(char **args) {
	if (args[1] == NULL) {
		if (copy_fd(STDIN_FILENO, STDOUT_FILENO) != 0) perror("cat");
		return;
	}

	for (int i=1; args[i] != NULL; i++) {
		int fd = open(args[i], O_RDONLY);
		if (fd < 0) {
			perror(args[i]);
			continue;
		}
		if (copy_fd(fd, STDOUT_FILENO) != 0) perror("cat");
		close(fd);
	}
}

void builtin_ls(char **args) {
	char *path = (args[1] != NULL) ? args[1] : ".";
	DIR *dir = opendir(path);
	if (!dir) {
		perror("ls");
		return;
	}

	struct dirent *entry;
	int first = 1;
	while ((entry = readdir(dir)) != NULL) {
		if (!first) printf("  ");
		printf("%s", entry->d_name);
		first = 0;
	}
	printf("\n");
	closedir(dir);
}

void builtin_chmod(char **args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		fprintf(stderr, "chmod usage syntax: chmod MODE FILE\n");
		return;
	}

	char *end = NULL;
	long mode = strtol(args[1], &end, 8);
	if (end == args[1] || *end != '\0') {
		fprintf(stderr, "chmod: invalid mode\n");
		return;
	}

	if (chmod(args[2], (mode_t)mode) != 0) perror("chmod");
}

void builtin_wc(char **args) {
	int fd;
	if (args[1] == NULL) {
		fd = STDIN_FILENO;
	} else if (args[2] != NULL) {
		fprintf(stderr, "wc: this simple version supports at most one file\n");
		return;
	} else {
		fd = open(args[1], O_RDONLY);
		if (fd < 0) {
			perror("wc");
			return;
		}
	}

	char buf[MAXLEN];
	long lines = 0, words = 0, bytes = 0;
	int in_word = 0;

	while (1) {
		int n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			perror("wc");
			if (fd != STDIN_FILENO) close(fd);
			return;
		}
		if (n == 0) break;

		bytes += n;
		for (int i=0; i<n; i++) {
			char c = buf[i];
			if (c == '\n') lines++;
			if (isspace(c)) {
				in_word = 0;
			} else if (!in_word) {
				in_word = 1;
				words++;
			}
		}
	}

	if (fd != STDIN_FILENO) close(fd);
	printf("%ld %ld %ld\n", lines, words, bytes);
}

void builtin_uname(char **args) {
	char buffer[] = "COSC 354 Simple Command Line Interpreter\n";
	if (write_fd(STDOUT_FILENO, buffer, strlen(buffer)) != 0) perror("uname");
}

void builtin_ln(char **args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		fprintf(stderr, "ln: usage: ln TARGET LINKNAME\n");
		return;
	}
	if (link(args[1], args[2]) != 0) perror("ln");
}

void builtin_whoami(char **args) {
	struct passwd *pw = getpwuid(getuid());
	if (!pw) {
		perror("whoami");
		return;
	}
	printf("%s\n", pw->pw_name);
}

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
		// Creating a pipe
		int pipefd[2];

		// Looping over the commands in order
		if (i < cmdCount-1) {
			if (pipe(pipefd) == -1) {
				perror("pipe");
				if (prev_fd != -1) close(prev_fd);
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
			char **currentArgs = &args[cmdIndex[i]];
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

			// Check for commands which can't be piped
			if (strcmp(currentArgs[0], "cd") == 0 || strcmp(currentArgs[0], "exit") == 0) {
				fprintf(stderr, "%s: must be run as a single command\n", currentArgs[0]);
				exit(1);
			}
			
			if (strcmp(currentArgs[0], "pwd") == 0) builtin_pwd(currentArgs);
			else if (strcmp(currentArgs[0], "echo") == 0) builtin_echo(currentArgs);
			else if (strcmp(currentArgs[0], "mkdir") == 0) builtin_mkdir(currentArgs);
			else if (strcmp(currentArgs[0], "rmdir") == 0) builtin_rmdir(currentArgs);
			else if (strcmp(currentArgs[0], "rm") == 0) builtin_rm(currentArgs);
			else if (strcmp(currentArgs[0], "touch") == 0) builtin_touch(currentArgs);
			else if (strcmp(currentArgs[0], "mv") == 0) builtin_mv(currentArgs);
			else if (strcmp(currentArgs[0], "cp") == 0) builtin_cp(currentArgs);
			else if (strcmp(currentArgs[0], "cat") == 0) builtin_cat(currentArgs);
			else if (strcmp(currentArgs[0], "ls") == 0) builtin_ls(currentArgs);
			else if (strcmp(currentArgs[0], "chmod") == 0) builtin_chmod(currentArgs);
			else if (strcmp(currentArgs[0], "wc") == 0) builtin_wc(currentArgs);
			else if (strcmp(currentArgs[0], "uname") == 0) builtin_uname(currentArgs);
			else if (strcmp(currentArgs[0], "ln") == 0) builtin_ln(currentArgs);
			else if (strcmp(currentArgs[0], "whoami") == 0) builtin_whoami(currentArgs);
			else {
				// Not a built-in command
				execvp(currentArgs[0], currentArgs);
				perror(currentArgs[0]);
			}
			exit(1);
		}
		
		// Closing the fd from the previous command as it is no longer needed
		if (prev_fd != -1) close(prev_fd);
		
		// Storing the read pipe fd to be used in the next command
		if (i < cmdCount-1) {
			close(pipefd[1]);
			prev_fd = pipefd[0];
		}
	}
	
	// Ensuring the last carryover fd is closed
	if (prev_fd != -1) close(prev_fd);
	
	// Waiting for all the child processes to finish running
	for (int i=0; i<cmdCount; i++) {
		waitpid(pids[i], NULL, 0);
	}
}

int main() {
	char buffer[MAXLEN];

	while (1) {
		printf("shell> ");
		if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
			perror("fgets");
			break;
		}

		buffer[strcspn(buffer, "\n")] = '\0';

		if (buffer[0] == '\0') continue;

		int length = strlen(buffer);
		char *args[length];
		int cmdIndex[MAXPIPE+1];
		int cmdCount = parseCmds(buffer, args, cmdIndex);
		if (cmdCount < 0) {
			fprintf(stderr, "parse error\n");
			continue;
		}

		char **arg0 = &args[cmdIndex[0]];

		if (cmdCount == 1) {
			if (strcmp(arg0[0], "exit") == 0) break;
			if (strcmp(arg0[0], "cd") == 0) {
				builtin_cd(arg0);
				continue;
			}
		}
		
		runCmds(args, cmdIndex, cmdCount);
	}
	return 0;
}
	