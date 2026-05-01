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
#include <arpa/inet.h>
#include <pthread.h>
#include <semaphore.h>

#define MAXPIPE 3
#define MAXLEN 4096
#define PORT 8080
#define END_MARKER "SERVER_OUT_DONE\n"
#define COMMAND_SEM_NAME "/phase3server_command_sem"

sem_t command_sem;
int running = 1;

typedef struct {
	int sockfd;
	int cwd_fd;
	char cwd_path[MAXLEN];
} session_t;

int copy_fd(int in, int out) {
	char buf[MAXLEN];

	while (1) {
		int n = read(in, buf, sizeof(buf));
		if (n < 0) return -1;
		if (n == 0) return 0;
		int total_written = 0;
		while (n > 0) {
			int m = write(out, buf + total_written, n);
			if (m < 0) return -1;
			n -= m;
			total_written += m;
		}
	}
	return 0;
}

void builtin_pwd(session_t *sess, char **args) {
	if (write(sess->sockfd, sess->cwd_path, strlen(sess->cwd_path)) < 0) {
		perror("pwd");
		return;
	}
	if (write(sess->sockfd, "\n", 1) < 0) perror("pwd");
}

void builtin_cd(session_t *sess, char** args) {
    char *path = args[1];
    int newfd;
    char proc_path[64];
    char resolved[PATH_MAX];
    ssize_t len;

    if (path == NULL) {
        path = getenv("HOME");
        if (path == NULL) path = "/";
    }

    if (path[0] == '/')
        newfd = open(path, O_RDONLY | O_DIRECTORY);
    else
        newfd = openat(sess->cwd_fd, path, O_RDONLY | O_DIRECTORY);

    if (newfd < 0) {
        dprintf(sess->sockfd, "cd: %s\n", strerror(errno));
        return;
    }

    // Resolve the real absolute path of the opened directory fd
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", newfd);
    len = readlink(proc_path, resolved, sizeof(resolved) - 1);
    if (len < 0) {
        dprintf(sess->sockfd, "cd: %s\n", strerror(errno));
        close(newfd);
        return;
    }
    resolved[len] = '\0';

    close(sess->cwd_fd);
    sess->cwd_fd = newfd;

    strncpy(sess->cwd_path, resolved, sizeof(sess->cwd_path) - 1);
    sess->cwd_path[sizeof(sess->cwd_path) - 1] = '\0';
}

int open_session_path(session_t *sess, const char *path, int flags, mode_t mode) {
    if (path[0] == '/') return open(path, flags, mode);
    return openat(sess->cwd_fd, path, flags, mode);
}

int mkdir_session_path(session_t *sess, const char *path, mode_t mode) {
    if (path[0] == '/') return mkdir(path, mode);
    return mkdirat(sess->cwd_fd, path, mode);
}

int unlink_session_path(session_t *sess, const char *path) {
    if (path[0] == '/') return unlink(path);
    return unlinkat(sess->cwd_fd, path, 0);
}

int rmdir_session_path(session_t *sess, const char *path) {
    if (path[0] == '/') return rmdir(path);
    return unlinkat(sess->cwd_fd, path, AT_REMOVEDIR);
}

void builtin_echo(char **args) {
	for (int i=1; args[i] != NULL; i++) {
		if (write(STDOUT_FILENO, args[i], strlen(args[i])) < 0) {
			perror("echo");
			return;
		}
		if (args[i + 1] != NULL) {
			if (write(STDOUT_FILENO, " ", 1) < 0) {
				perror("echo");
				return;
			}
		}
	}
	if (write(STDOUT_FILENO, "\n", 1) < 0) perror("echo");
}

void builtin_mkdir(session_t *sess, char** args) {
    if (args[1] == NULL) {
        write(sess->sockfd, "mkdir: missing operand\n", 23);
        return;
    }

    for (int i = 1; args[i] != NULL; i++) {
        if (mkdir_session_path(sess, args[i], 0777) != 0) {
            dprintf(sess->sockfd, "mkdir: %s\n", strerror(errno));
            return;
        }
    }
}

void builtin_rmdir(session_t *sess, char** args) {
    if (args[1] == NULL) {
        dprintf(sess->sockfd, "rmdir: missing operand\n");
        return;
    }

    for (int i = 1; args[i] != NULL; i++) {
        int rc;

        if (args[i][0] == '/') {
            // absolute path
            rc = rmdir(args[i]);
        } else {
            // relative to session cwd
            rc = unlinkat(sess->cwd_fd, args[i], AT_REMOVEDIR);
        }

        if (rc != 0) {
            dprintf(sess->sockfd, "rmdir: %s\n", strerror(errno));
            return;
        }
    }
}

void builtin_rm(session_t *sess, char** args) {
    if (args[1] == NULL) {
        write(sess->sockfd, "rm: missing operand\n", 20);
        return;
    }

    for (int i = 1; args[i] != NULL; i++) {
        if (unlink_session_path(sess, args[i]) != 0) {
            dprintf(sess->sockfd, "rm: %s\n", strerror(errno));
            return;
        }
    }
}

void builtin_touch(session_t *sess, char** args) {
    if (args[1] == NULL) {
        write(sess->sockfd, "touch: missing file operand\n", 28);
        return;
    }

    for (int i = 1; args[i] != NULL; i++) {
        int fd = open_session_path(sess, args[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            dprintf(sess->sockfd, "touch: %s\n", strerror(errno));
            return;
        }
        close(fd);
    }
}

void builtin_mv(session_t *sess, char** args) {
    if (args[1] == NULL || args[2] == NULL) {
        dprintf(sess->sockfd, "mv: missing source or destination\n");
        return;
    }

    int rc;

    if (args[1][0] == '/' && args[2][0] == '/') {
        // both absolute paths
        rc = rename(args[1], args[2]);
    } else if (args[1][0] != '/' && args[2][0] != '/') {
        // both relative to session cwd
        rc = renameat(sess->cwd_fd, args[1], sess->cwd_fd, args[2]);
    } else if (args[1][0] == '/') {
        // source absolute, dest relative
        rc = renameat(AT_FDCWD, args[1], sess->cwd_fd, args[2]);
    } else {
        // source relative, dest absolute
        rc = renameat(sess->cwd_fd, args[1], AT_FDCWD, args[2]);
    }

    if (rc != 0) {
        dprintf(sess->sockfd, "mv: %s\n", strerror(errno));
    }
}

void builtin_cp(session_t *sess, char** args) {
    if (args[1] == NULL || args[2] == NULL) {
        dprintf(sess->sockfd, "cp: missing source or destination\n");
        return;
    }

    int src_fd, dest_fd;

    if (args[1][0] == '/')
        src_fd = open(args[1], O_RDONLY);
    else
        src_fd = openat(sess->cwd_fd, args[1], O_RDONLY);

    if (src_fd < 0) {
        dprintf(sess->sockfd, "cp: %s\n", strerror(errno));
        return;
    }

    if (args[2][0] == '/')
        dest_fd = open(args[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    else
        dest_fd = openat(sess->cwd_fd, args[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (dest_fd < 0) {
        dprintf(sess->sockfd, "cp: %s\n", strerror(errno));
        close(src_fd);
        return;
    }

    char buffer[1024];
    ssize_t n;

    while ((n = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(dest_fd, buffer + written, n - written);
            if (w < 0) {
                dprintf(sess->sockfd, "cp: %s\n", strerror(errno));
                close(src_fd);
                close(dest_fd);
                return;
            }
            written += w;
        }
    }

    if (n < 0) {
        dprintf(sess->sockfd, "cp: %s\n", strerror(errno));
    }

    close(src_fd);
    close(dest_fd);
}

void builtin_cat(session_t *sess, char** args) {
    if (args[1] == NULL) {
        dprintf(sess->sockfd, "cat: missing operand\n");
        return;
    }

    for (int i = 1; args[i] != NULL; i++) {
        int fd;

        // Resolve path relative to session
        if (args[i][0] == '/')
            fd = open(args[i], O_RDONLY);
        else
            fd = openat(sess->cwd_fd, args[i], O_RDONLY);

        if (fd < 0) {
            dprintf(sess->sockfd, "cat: %s\n", strerror(errno));
            continue;
        }

        char buffer[1024];
        ssize_t n;

        while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
            if (write(sess->sockfd, buffer, n) < 0) {
                perror("write");
                break;
            }
        }

        close(fd);
    }
}

void builtin_ls(session_t *sess, char** args) {
    char *path = (args[1] != NULL) ? args[1] : ".";
    int dfd;

    if (path[0] == '/')
        dfd = open(path, O_RDONLY | O_DIRECTORY);
    else
        dfd = openat(sess->cwd_fd, path, O_RDONLY | O_DIRECTORY);

    if (dfd < 0) {
        dprintf(sess->sockfd, "ls: %s\n", strerror(errno));
        return;
    }

    DIR *dir = fdopendir(dfd);
    if (!dir) {
        dprintf(sess->sockfd, "ls: %s\n", strerror(errno));
        close(dfd);
        return;
    }

    struct dirent *entry;
    int first = 1;
    while ((entry = readdir(dir)) != NULL) {
        if (!first) write(sess->sockfd, " ", 1);
        write(sess->sockfd, entry->d_name, strlen(entry->d_name));
        first = 0;
    }
    write(sess->sockfd, "\n", 1);
    closedir(dir);
}

void builtin_chmod(session_t *sess, char** args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		write(STDERR_FILENO, "chmod usage syntax: chmod MODE FILE\n", 37);
		return;
	}

	char *end = NULL;
	long mode = strtol(args[1], &end, 8);
	if (end == args[1] || *end != '\0') {
		write(STDERR_FILENO, "chmod: invalid mode\n", 20);
		return;
	}

	if (chmod(args[2], (mode_t)mode) != 0) perror("chmod");
}

void builtin_wc(session_t *sess, char** args) {
	int fd;
	if (args[1] == NULL) {
		fd = STDIN_FILENO;
	} else if (args[2] != NULL) {
		write(STDERR_FILENO, "wc: this simple version supports at most one file\n", 50);
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

void builtin_uname(session_t *sess, char** args) {
	char buffer[] = "COSC 354 Simple Command Line Interpreter\n";
	if (write(STDOUT_FILENO, buffer, strlen(buffer)) < 0) perror("uname");
}

void builtin_ln(session_t *sess, char** args) {
	if (args[1] == NULL || args[2] == NULL || args[3] != NULL) {
		write(STDERR_FILENO, "ln: usage: ln TARGET LINKNAME\n", 31);
		return;
	}
	if (link(args[1], args[2]) != 0) perror("ln");
}

void builtin_whoami(session_t *sess, char** args) {
	struct passwd *pw = getpwuid(getuid());
	if (!pw) {
		perror("whoami");
		return;
	}
	write(STDOUT_FILENO, pw->pw_name, strlen(pw->pw_name));
	write(STDOUT_FILENO, "\n", 1);
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

void runCmdsRemote(session_t *sess, char *args[], int cmdIndex[], int cmdCount) {
	if (cmdCount == 0) return;
	int prev_fd = -1;
	pid_t pids[cmdCount];

	// Looping over the commands in order
	for (int i=0; i<cmdCount; i++) {
		// Creating a pipe
		int pipefd[2];

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
					exit(EXIT_FAILURE);
				}
			}

			if (i < cmdCount-1) {
				// Redirect output of current command to pipe
				close(pipefd[0]);
				if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
					perror("dup2");
					exit(EXIT_FAILURE);
				}
				close(pipefd[1]);
			} else {
				if (dup2(sess->sockfd, STDOUT_FILENO) == -1) {
					perror("dup2");
					exit(EXIT_FAILURE);
				}
				if (dup2(sess->sockfd, STDERR_FILENO) == -1) {
					perror("dup2");
					exit(EXIT_FAILURE);
				}
			}

			// prev_fd has been redirected so it can be closed
			if (prev_fd != -1) close(prev_fd);

			if (fchdir(sess->cwd_fd) != 0) {
				dprintf(STDERR_FILENO, "fchdir: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}

			// Check for commands which can't be piped
			if (strcmp(currentArgs[0], "cd") == 0 || strcmp(currentArgs[0], "exit") == 0) {
				write(STDERR_FILENO, currentArgs[0], strlen(currentArgs[0]));
				write(STDERR_FILENO, ": must be run as a single command\n", 35);
				exit(EXIT_FAILURE);
			}

			if (strcmp(currentArgs[0], "pwd") == 0) builtin_pwd(sess, currentArgs);
			else if (strcmp(currentArgs[0], "echo") == 0) builtin_echo(currentArgs);
			else if (strcmp(currentArgs[0], "mkdir") == 0) builtin_mkdir(sess, currentArgs);
			else if (strcmp(currentArgs[0], "rmdir") == 0) builtin_rmdir(sess, currentArgs);
			else if (strcmp(currentArgs[0], "rm") == 0) builtin_rm(sess, currentArgs);
			else if (strcmp(currentArgs[0], "touch") == 0) builtin_touch(sess, currentArgs);
			else if (strcmp(currentArgs[0], "mv") == 0) builtin_mv(sess, currentArgs);
			else if (strcmp(currentArgs[0], "cp") == 0) builtin_cp(sess, currentArgs);
			else if (strcmp(currentArgs[0], "cat") == 0) builtin_cat(sess, currentArgs);
			else if (strcmp(currentArgs[0], "ls") == 0) builtin_ls(sess, currentArgs);
			else if (strcmp(currentArgs[0], "chmod") == 0) builtin_chmod(sess, currentArgs);
			else if (strcmp(currentArgs[0], "wc") == 0) builtin_wc(sess, currentArgs);
			else if (strcmp(currentArgs[0], "uname") == 0) builtin_uname(sess, currentArgs);
			else if (strcmp(currentArgs[0], "ln") == 0) builtin_ln(sess, currentArgs);
			else if (strcmp(currentArgs[0], "whoami") == 0) builtin_whoami(sess, currentArgs);
			else {
				// Not a built-in command
				execvp(currentArgs[0], currentArgs);
				perror(currentArgs[0]);

				// execvp call failed
				write(STDOUT_FILENO, currentArgs[0], strlen(currentArgs[0]));
				write(STDOUT_FILENO, ": command not found or failed\n", 30);
				exit(EXIT_FAILURE);
			}
			exit(EXIT_SUCCESS);
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

void shell(session_t *sess) {
	char buffer[MAXLEN];

	while (running) {
		int n = read(sess->sockfd, buffer, sizeof(buffer) - 1);
		if (n < 0) {
			perror("read");
			break;
		}
		if (n == 0) {
			// Client closed the connection
			printf("Client closed the connection\n");
			break;
		}

		buffer[strcspn(buffer, "\n")] = '\0';

		if (buffer[0] == '\0') {
			write(sess->sockfd, END_MARKER, strlen(END_MARKER));
			continue;
		}

		int length = strlen(buffer);
		char *args[length];
		int cmdIndex[MAXPIPE+1];
		int cmdCount = parseCmds(buffer, args, cmdIndex);
		if (cmdCount < 0) {
			write(sess->sockfd, "parse error\n", 12);
			write(sess->sockfd, END_MARKER, strlen(END_MARKER));
			continue;
		}

		char **arg0 = &args[cmdIndex[0]];

		sem_wait(&command_sem);
		if (cmdCount == 1) {
			if (strcmp(arg0[0], "exit") == 0) {
				write(sess->sockfd, END_MARKER, strlen(END_MARKER));
				sem_post(&command_sem);
				break;
			}
			if (strcmp(arg0[0], "shutdown") == 0) {
				running = 0;
				write(sess->sockfd, END_MARKER, strlen(END_MARKER));
				sem_post(&command_sem);
				break;
			}
			if (strcmp(arg0[0], "cd") == 0) {
				builtin_cd(sess, arg0);
				write(sess->sockfd, END_MARKER, strlen(END_MARKER));
				sem_post(&command_sem);
				continue;
			}
		}

		runCmdsRemote(sess, args, cmdIndex, cmdCount);
		sem_post(&command_sem);
		write(sess->sockfd, END_MARKER, strlen(END_MARKER));
	}
}

void *client_thread(void *arg) {
	session_t *client_sess = (session_t *)arg;

	// Start the shell
	shell(client_sess);

	// Close the socket
	close(client_sess->cwd_fd);
	close(client_sess->sockfd);
	free(client_sess);
	return NULL;
}

int main() {
	// Create a socket
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return 1;
	}

	// Define server address
	struct sockaddr_in server_addr;
	socklen_t addrlen = sizeof(server_addr);
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	// Bind the socket to the address
	if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind");
		close(sockfd);
		return 1;
	}

	// Listen for incoming connections
	if (listen(sockfd, 3) < 0) {
		perror("listen");
		close(sockfd);
		return 1;
	}

	// Semaphore to allow only 1 command to execute at a time
	sem_init(&command_sem, 1, 1);

	while (running) {
		// Accept incoming connections
		int client_sockfd;
		if ((client_sockfd = accept(sockfd, (struct sockaddr *)&server_addr, &addrlen)) < 0) {
			perror("accept");
			close(sockfd);
			return 1;
		}

		session_t *client_arg = malloc(sizeof(session_t));
		if (client_arg == NULL) {
			perror("malloc");
			close(client_sockfd);
			continue;
		}

		// Defining the current session struct
		client_arg->sockfd = client_sockfd;
		client_arg->cwd_fd = open(".", O_RDONLY | O_DIRECTORY);
		if (client_arg->cwd_fd < 0) {
			perror("open");
			close(client_sockfd);
			free(client_arg);
			continue;
		}

		if (getcwd(client_arg->cwd_path, sizeof(client_arg->cwd_path)) == NULL) {
			perror("getcwd");
			close(client_sockfd);
			close(client_arg->cwd_fd);
			free(client_arg);
			continue;
		}

		pthread_t thread;
		if (pthread_create(&thread, NULL, client_thread, client_arg) != 0) {
			perror("pthread_create");
			close(client_sockfd);
			free(client_arg);
			continue;
		}

		// For resource cleanup upon after thread close
		pthread_detach(thread);
	}

	sem_close(&command_sem);
	close(sockfd);
	return 0;
}
