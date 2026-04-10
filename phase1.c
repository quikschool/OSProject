#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#define MAXPIPE 3
#define MAXLEN 1024

int parseCmds(char *str, char *result[], int cmdIndex[]) {
	// result is structured as {arg1, arg2, ..., NULL, arg1, arg2, ..., NULL}
	int foundCmd=0, cmds=0, wordCount=0, spacing=0, breaking=0;
	int str_size = strlen(str);
	if (str_size == 0) return -1;
	if (str[0] == ' ') spacing = 1;
	for (int i=0; i<str_size; i++) {
		if (str[i] == ' ') {
			if (!spacing) {
				str[i] = '\0';
				spacing = 1;
			}
		} else if (str[i] == '|') {
			// '|' found before any commands
			if (!foundCmd) return -1;
			
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
			if (breaking || !foundCmd) {
				if (cmds == MAXPIPE + 1) return -1;
				cmdIndex[cmds++] = wordCount;
				breaking = 0;
			}
			foundCmd = 1;
			result[wordCount++] = &str[i];
			spacing = 0;
		}
	}
	if (breaking) return -1;
	if (!foundCmd) return -1;
	result[wordCount] = NULL;
	return cmds;
}

void runCmds(char *args[], int cmdIndex[], int cmdCount) {
	
}

int main() {
	