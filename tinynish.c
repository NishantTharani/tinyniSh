#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

struct cmdInfo {
    char* command;
    char* args[513];  // Includes the command
    bool isIgnored;  // True for comments and blank lines
    bool isBackground;
    int argsCount;
    char* inputRedirect;
    char* outputRedirect;
};

struct pidNode {
    pid_t pid;
    struct pidNode* next;
};

struct shellState {
    char* status;  // The string to be printed out on 'status'
    char* currentDir;
    struct pidNode* head;  // keeps track of all running processes that need to be cleaned up on exit
    bool backgroundIgnored;
    struct cmdInfo* currentCmd;
};

void reapBackgroundProcesses(bool, bool);
void removeArgument(struct cmdInfo* info, int argidx, bool freestr);

// Keep track of shell state as a global variable
struct shellState* gState;

// Signal handling structs are global so that they can be easily referenced from both parent and child processes
struct sigaction gSIGINT_ignore = {
        .sa_handler = SIG_IGN,
        .sa_flags = 0
};

struct sigaction gSIGINT_default = {
        .sa_handler = SIG_DFL,
        .sa_flags = 0
};

struct sigaction gSIGTSTP_ignore = {
        .sa_handler = SIG_IGN,
        .sa_flags = 0
};

void handleSIGTSTP(int signo);

struct sigaction gSIGTSTP_handle = {
        .sa_handler = handleSIGTSTP,
//        .sa_flags = SA_RESTART
        .sa_flags = 0
};


/*
 * Checks to see if 'token' contains any instances of '$$' and if so,
 * expands each one by replacing them with the PID of the shell process.
 */
char* expandDoubleDollar(char* token) {
    char* needle = "$$";
    char* expanded = calloc(2049, 1);

    // Keep track of the current position inside the token and the expanded output
    char* tokenPos = token;
    char* expandedPos = expanded;

    // Fetch the PID as a string
    long rawPid = getpid();
    char* pid = calloc(10, 1);
    sprintf(pid, "%ld", rawPid);
    size_t lenPid = strlen(pid);

    char* location = strstr(tokenPos, needle);

    // There may be multiple occurrences of $$, keep going till we get them all
    while (location != NULL) {
        // Copy the characters up until '$$' into the output string
        long nBytes = location - tokenPos;
        strncpy(expandedPos, tokenPos, nBytes);
        expandedPos += nBytes;
        // Copy the PID in the place of '$$'
        strncpy(expandedPos, pid, lenPid);
        expandedPos += lenPid;
        // Begin looking again after the '$$'
        tokenPos = location + 2;
        location = strstr(tokenPos, needle);
    }

    // Copy the remaining characters and null terminate the destination string
    strcpy(expandedPos, tokenPos);
    free(pid);
    return expanded;
}

/*
 * Frees the memory taken up by the provided cmdInfo struct
 * and its fields.
 */
void freeCmd(struct cmdInfo *cmd) {
    // No need to free command separately - it is freed by freeing args
    free(cmd->outputRedirect);
    free(cmd->inputRedirect);

    for (int i = 0; i < cmd->argsCount; i++) {
        free(cmd->args[i]);
    }

    free(cmd);
}

/*
 * Frees the memory taken up by the provided shellState struct
 * and its fields. The memory taken by state->currentCmd is assumed to be
 * freed separately.
 */
void freeState(struct shellState *state) {
    free(state->status);
    free(state->currentDir);
    free(state);
}

/*
 * allocs a string that holds a line of input entered by the user
 * (the shell command + arguments), and returns a pointer to it
 */
char* getCmd() {
    char* cmdInput = NULL;
    size_t len = 0;
    ssize_t n;
    printf(": ");
    fflush(stdout);
    n = getline(&cmdInput, &len, stdin);
    // If getline exits with an error, for example if SIGTSTP is received, return an empty string
    if (n == -1) {
        clearerr(stdin);
        cmdInput = calloc(2, 1);
        sprintf(cmdInput, "");
    } else {
        // Trim the newline
        cmdInput[n - 1] = '\0';
    }
    fflush(stdout);
    return cmdInput;
}

/*
 * Initialises a new shellState struct,
 * and all of its fields too,
 * and returns a pointer to it
 */
struct shellState* getNewShellState() {
    // TODO free this up when exit is called; also free up the current 'cmd' which is pointed to
    struct shellState *state = malloc(sizeof(struct shellState));
    state->backgroundIgnored = false;
    state->head = NULL;
    state->currentCmd = NULL;
    state->currentDir = calloc(4097, 1);
    state->status = calloc(30, 1);
    sprintf(state->status, "%s", "exit value 0\n");
    return state;
}

/*
 * Executes the command described by the provided struct
 */
void handleCmd(struct cmdInfo* info) {
    if (info->isIgnored) {
        return;
    }

    /* Debug options
    printf("Your command is: %s\n", info->command);

    for (int i = 1; i < info->argsCount; i++) {
        printf("Argument number %d: %s\n", i, info->args[i]);
    }

    printf("Input redirected from: %s\n", info->inputRedirect);
    printf("Output redirected to: %s\n", info->outputRedirect);
    printf("Background: %d\n", info->isBackground);
     */

    // Implement 'cd' command
    if (strcmp(info->command, "cd") == 0) {
        // If there are no arguments, change to the home directory
        if (info->argsCount == 1) {
            const char* homePath = getenv("HOME");
            chdir(homePath);
            strcpy(gState->currentDir, homePath);
        } else {
            // Else change to the path
            chdir(info->args[1]);
            strcpy(gState->currentDir, info->args[1]);
        }
        setenv("PWD", gState->currentDir, 1);
    } else if (strcmp(info->command, "exit") == 0) {
        // printf("(DEBUG) Exiting...\n");
        reapBackgroundProcesses(true, false);
        freeCmd(gState->currentCmd);
        freeState(gState);
        exit(EXIT_SUCCESS);
    } else if (strcmp(info->command, "status") == 0) {
        // Prints the value found in the global shell state
        printf("%s", gState->status);
    } else {
        // A non built-in command is execvp'd using a forked child process
        // TODO make sure exit status is set to 1 and error message printed if the command cannot be found
        pid_t spawnPid = fork();
        bool background = info->isBackground && (!gState->backgroundIgnored);
        int infd = 0;
        int outfd = 1;

        switch(spawnPid) {
            case -1:
                printf("Error forking!\n");
                break;
            case 0:
                // Execute the new command from the child process
                // Redirect input/output if specified by user
                if (info->inputRedirect != NULL) {
                    infd = open(info->inputRedirect, O_RDONLY);
                    if (infd == -1) {
                        printf("cannot open %s for input\n", info->inputRedirect);
                        // TODO make sure that on this exit, the global status string is updated by parent to read 'exit value 1'
                        exit(EXIT_FAILURE);
                    }
                    dup2(infd, 0);
                }

                if (info->outputRedirect != NULL) {
                    outfd = open(info->outputRedirect, O_WRONLY | O_CREAT | O_TRUNC, 0640);
                    if (outfd == -1) {
                        printf("cannot open %s for output\n", info->outputRedirect);
                        // TODO make sure that on this exit, the global status string is updated by parent to read 'exit value 1'
                        exit(EXIT_FAILURE);
                    }
                    dup2(outfd, 1);
                }

                if (background) {
                    // Background child ignores SIGINT by default since the parent does too
                    sigaction(SIGTSTP, &gSIGTSTP_ignore, NULL);

                    // Redirect input and output to /dev/null if not specified by user
                    if (info->inputRedirect == NULL) {
                        infd = open("/dev/null", O_RDONLY);
                        dup2(infd, 0);
                    }
                    if (info->outputRedirect == NULL) {
                        outfd = open("/dev/null", O_WRONLY | O_TRUNC | O_CREAT);
                        dup2(outfd, 1);
                    }
                } else {
                    // Register signal handlers for foreground child
                    sigaction(SIGINT, &gSIGINT_default, NULL);
                    sigaction(SIGTSTP, &gSIGTSTP_ignore, NULL);
                }

                // info was initialised to zeros way back in parseCmd, so the final argument of
                // info->args should be NULL already?
                execvp(info->args[0], info->args);

                // print an error if the file/command could not be found
                if (errno == ENOENT) {
                    printf("%s: no such file or directory\n", info->args[0]);
                    exit(EXIT_FAILURE);
//                    sprintf(gState->status, "exit value 1");
                }

                // TODO how to notify the parent of this error, so that it does not do the below actions?
                // TODO do i need to make sure that the child process terminates in this case?
            default:
                // Parent process actions depends on whether our command was foreground or background
                if (background) {
                    // Store the PID in a global linked list, so we keep track of what processes are active
                    struct pidNode* newNode = malloc(sizeof(struct pidNode));
                    newNode->next = NULL;
                    newNode->pid = spawnPid;
                    struct pidNode* curr = gState->head;
                    if (curr == NULL) {
                        gState->head = newNode;
                    } else {
                        while (curr->next != NULL) {
                            curr = curr->next;
                        }
                        curr->next = newNode;
                    }

                    // Print the PID
                    printf("background pid is %i\n", spawnPid);
                } else {
                    // Blocking wait for child process termination. Make sure to block SIGTSTP until after the child has terminated
                    int childStatus;
                    sigset_t sigtstpSet;
                    sigaddset(&sigtstpSet, SIGTSTP);
                    sigprocmask(SIG_BLOCK, &sigtstpSet, NULL);
                    waitpid(spawnPid, &childStatus, 0);
                    sigprocmask(SIG_UNBLOCK, &sigtstpSet, NULL);

                    // Update the 'exit status' global string based on how the child terminated
                    if (WIFEXITED(childStatus)) {
                        int exitStatus = WEXITSTATUS(childStatus);
                        sprintf(gState->status, "exit value %i\n", exitStatus);
                    } else {
                        // In this case we know the child was signal terminated - also print the signal number here
                        int exitSignal = WTERMSIG(childStatus);
                        sprintf(gState->status, "terminated by signal %i\n", exitSignal);
                        printf("terminated by signal %i\n", exitSignal);
                    }
                }
        }
    }
}

/*
 * Handles the SIGTSTP signal by toggling foreground-only mode
 * (where '&' at the end of a command is ignored, and all commands are run in the foreground)
 * and informing the user of this
 */
void handleSIGTSTP(int signo) {
    if (gState->backgroundIgnored) {
        gState->backgroundIgnored = false;
        char* message = "Exiting foreground-only mode\n";
        write(STDOUT_FILENO, message, 29);
    } else {
        gState->backgroundIgnored = true;
        char* message = "Entering foreground-only mode (& is now ignored)\n";
        write(STDOUT_FILENO, message, 49);
    }
}

/*
 * Parses raw user-entered command string and sets the fields of the provided
 * cmdInfo struct accordingly.
 */
void parseCmd(char cmdInput[], struct cmdInfo* info) {
    char *savePtr;
    char *token;
    char *expandedToken;
    int argCount = 0;  // track number of arguments so we can loop through them easily later on
    int idxForIORedir = 0;

    token = strtok_r(cmdInput, " ", &savePtr);

    // Handle empty line
    if (token == NULL) {
        info->isIgnored = true;
        info->argsCount = argCount;
        return;
    }

    // First token is the command
    expandedToken = expandDoubleDollar(token);
    info->args[0] = calloc(strlen(expandedToken) + 1, sizeof(char));
    strcpy(info->args[0], expandedToken);
    free(expandedToken);
    info->command = info->args[0];
    argCount++;

    // Check if the command is a comment and return if so
    if (info->command[0] == '#') {
        info->isIgnored = true;
        info->argsCount = argCount;
        return;
    }

    // The rest, if any, are arguments
    token = strtok_r(NULL, " ", &savePtr);
    while (token != NULL) {
        expandedToken = expandDoubleDollar(token);
        info->args[argCount] = calloc(strlen(expandedToken) + 1, sizeof(char));
        strcpy(info->args[argCount], expandedToken);
        free(expandedToken);
        argCount++;
        token = strtok_r(NULL, " ", &savePtr);
    }

    info->argsCount = argCount;
    // Check if the command is to be processed in the background
    if (info->argsCount > 1 && strcmp(info->args[info->argsCount-1], "&") == 0) {
        info->isBackground = true;
        // Remove the '&' from the list of arguments so that it's not later passed to execvp
        removeArgument(info, info->argsCount - 1, true);
        idxForIORedir = argCount - 2;
    } else {
        idxForIORedir = argCount - 2;
    }

    // Check if there is any input/output redirection
    // Only check if we have enough arguments in the first place
    if (idxForIORedir > 0) {
        if (strcmp(info->args[idxForIORedir], "<") == 0) {
            info->inputRedirect = info->args[idxForIORedir + 1];

            // Remove the '<' and the filename from the list of arguments, so they aren't later passed to execvp
            removeArgument(info, idxForIORedir, true);
            removeArgument(info, idxForIORedir + 1, false);

            // If we found input redirection, check for output redirection earlier on
            // Again, only if there are enough arguments
            if (idxForIORedir - 2 > 0 && strcmp(info->args[idxForIORedir - 2], ">") == 0) {
                info->outputRedirect = info->args[idxForIORedir - 1];

                removeArgument(info, idxForIORedir - 2, true);
                removeArgument(info, idxForIORedir - 1, false);
            }

        } else if (strcmp(info->args[idxForIORedir], ">") == 0) {
            info->outputRedirect = info->args[idxForIORedir + 1];

            removeArgument(info, idxForIORedir, true);
            removeArgument(info, idxForIORedir + 1, false);

            if (idxForIORedir - 2 > 0 && strcmp(info->args[idxForIORedir - 2], "<") == 0) {
                info->inputRedirect = info->args[idxForIORedir - 1];

                removeArgument(info, idxForIORedir - 2, true);
                removeArgument(info, idxForIORedir - 1, false);
            }
        }
    }
    gState->currentCmd = info;
}

/*
 * This function:
 *      Checks whether any of the background PIDs stored in the global state linked list have terminated
 *      If so, removes them from the linked list (and frees the relevant memory of the removed node)
 *
 * If 'verbose' is true:
 *          also prints out a message indicating that they have terminated, and their exit status
 *          (this option is used when calling the function before each prompt)
 *          (but not when the exit command is given)
 *
 * If 'killall' is true:
 *          sends SIGTERM to all background PIDs that have not terminated yet
 *          (this is used when the exit command is given)
 *
 * // TODO replace the 'verbose' part of this functionality with a handler for SIGCHLD, leaving just the killall
 * // part to be done on exit
 */
void reapBackgroundProcesses(bool verbose, bool killall) {
    struct pidNode* curr = gState->head;
    struct pidNode* prev = NULL;
    int pidToCheck;
    int pidAfterCheck;
    int pidStatus;
    if (curr == NULL) {
        return;
    }
    while (curr != NULL) {
        pidToCheck = curr->pid;
        pidAfterCheck = waitpid(pidToCheck, &pidStatus, WNOHANG);
        if (pidAfterCheck != 0 && verbose) {
            // If the process has terminated and verbose is true, print out an informational message
            if (WIFEXITED(pidStatus)) {
                printf("background pid %i is done: exit value %i\n", pidToCheck, WEXITSTATUS(pidStatus));
            } else {
                printf("background pid %i is done: terminated by signal %i\n", pidToCheck, WTERMSIG(pidStatus));
            }
            // Remove the node from the linked list and free its memory
            if (prev == NULL) {
                gState->head = curr->next;
            } else {
                prev->next = curr->next;
            }
            free(curr);
        } else if (pidAfterCheck == 0 && killall) {
            // If the process has not yet terminated and killall is true, terminate it
            kill(pidToCheck, SIGTERM);
            waitpid(pidToCheck, &pidStatus, WNOHANG);
            // No need to prune the linked list here since we're about to exit the program anyway
        }
        curr = curr->next;
    }
}

/*
 * Removes the argument at index 'argidx' from the list of arguments stored at
 * info->args
 * Optionally frees the memory allocated to that argument
 */
void removeArgument(struct cmdInfo* info, int argidx, bool freestr) {
    if (freestr) {
        free(info->args[argidx]);
    }
    info->args[argidx] = NULL;
    info->argsCount--;
}

int main() {
    char *cmdInput;
    struct cmdInfo *cmd;
    gState = getNewShellState();

    // The parent process (and children by default) will ignore SIGINT
    sigaction(SIGINT, &gSIGINT_ignore, NULL);

    // Parent process will handle SIGTSTP by toggling foreground-only mode
    // TODO make sure that this waits for the current foreground process to finish
    sigaction(SIGTSTP, &gSIGTSTP_handle, NULL);


    while (true) {
        cmd = malloc(sizeof(struct cmdInfo));
        memset(cmd, 0, sizeof(*cmd));
        reapBackgroundProcesses(true, false);
        cmdInput = getCmd();
        parseCmd(cmdInput, cmd);
        handleCmd(cmd);
        free(cmdInput);
        freeCmd(cmd);  // TODO: the command is freed, but gState->cmd still points to it until the next one is parsed
    }
}


