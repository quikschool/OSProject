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
	
}

int main() {
	