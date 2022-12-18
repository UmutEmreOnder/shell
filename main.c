#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_LINE 80 /* 80 chars per line, per command, should be enough. */
#define HISTORY_SIZE 10
#define MAX_BACKGROUND_PROCESSES 100
#define PATH_MAX 100

char *history[HISTORY_SIZE];
char inputBuffer[MAX_LINE]; /* buffer to hold command entered */
pid_t foreground_pid = 0;


// global array to store the PIDs of all background processes
pid_t background_pids[MAX_BACKGROUND_PROCESSES];

// global counter for the number of background processes
int background_count = 0;

void fg(char *const *args);

void
ioRedirection(int input_redirect, int output_redirect, int append_redirect, int error_redirect, const char *input_file,
              const char *output_file, const char *error_file);

void exitCall();

//This function takes a program name and check it if it is executable or not.
int checkExecutable(const char *filename)
{
    int result;
    struct stat statinfo;

    result = stat(filename, &statinfo);
    if (result < 0) return 0;
    if (!S_ISREG(statinfo.st_mode)) return 0;

    if (statinfo.st_uid == geteuid()) return statinfo.st_mode & S_IXUSR;
    if (statinfo.st_gid == getegid()) return statinfo.st_mode & S_IXGRP;
    return statinfo.st_mode & S_IXOTH;
}

int findPath(char *pth, const char *exe)
{
    char *searchpath;
    char *beg, *end;
    int stop, found;
    int len;

    if (strchr(exe, '/') != NULL) {
        if (realpath(exe, pth) == NULL) return 0;
        return  checkExecutable(pth);
    }

    searchpath = getenv("PATH");
    if (searchpath == NULL) return 0;
    if (strlen(searchpath) <= 0) return 0;

    beg = searchpath;
    stop = 0; found = 0;
    do {
        end = strchr(beg, ':');
        if (end == NULL) {
            stop = 1;
            strncpy(pth, beg, PATH_MAX);
            len = strlen(pth);
        } else {
            strncpy(pth, beg, end - beg);
            pth[end - beg] = '\0';
            len = end - beg;
        }
        if (pth[len - 1] != '/') strncat(pth, "/", 2);
        strncat(pth, exe, PATH_MAX - len);
        found = checkExecutable(pth);
        if (!stop) beg = end + 1;
    } while (!stop && !found);

    return found;
}


void sigtstp_handler(int sig) {
    printf("Received SIGTSTP signal\n");

    if (foreground_pid > 0) {
        printf("Killing the process %d\n", foreground_pid);
        kill(foreground_pid, sig);
        foreground_pid = 0;
    }
}

void add_to_history(char *args[]) {
    /* shift history down one position */
    for (int i = HISTORY_SIZE - 1; i > 0; i--) {
        history[i] = history[i - 1];
    }

    /* concatenate args into a single string */
    history[0] = malloc(MAX_LINE);
    strcpy(history[0], args[0]);
    for (int i = 1; args[i] != NULL; i++) {
        strcat(history[0], " ");
        strcat(history[0], args[i]);
    }
}

void print_history() {
    for (int i = 0; i < HISTORY_SIZE; i++) {
        if (history[i]) {
            printf("%d %s\n", i, history[i]);
        }
    }
}

void list_background_processes() {
    for (int i = 0; i < background_count; i++) {
        pid_t pid = background_pids[i];
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            printf("[%d] %d Running\n", i + 1, pid);
        } else if (result == pid) {
            printf("[%d] %d Done\n", i + 1, pid);
        } else {
            printf("[%d] %d Unknown\n", i + 1, pid);
        }
    }
}

/* The setup function below will not return any value, but it will just: read
in the next command line; separate it into distinct arguments (using blanks as
delimiters), and set the args array entries to point to the beginning of what
will become null-terminated, C-style strings. */

void setup(char inputBuffer[], char *args[], int *background) {
    int length, /* # of characters in the command line */
    i,      /* loop index for accessing inputBuffer array */
    start,  /* index where beginning of next command parameter is */
    ct;     /* index of where to place the next parameter into args[] */

    ct = 0;

    /* read what the user enters on the command line */
    length = read(STDIN_FILENO, inputBuffer, MAX_LINE);

    /* 0 is the system predefined file descriptor for stdin (standard input),
       which is the user's screen in this case. inputBuffer by itself is the
       same as &inputBuffer[0], i.e. the starting address of where to store
       the command that is read, and length holds the number of characters
       read in. inputBuffer is not a null terminated C-string. */

    start = -1;
    if (length == 0)
        exit(0);            /* ^d was entered, end of user command stream */

/* the signal interrupted the read system call */
/* if the process is in the read() system call, read returns -1
  However, if this occurs, errno is set to EINTR. We can check this  value
  and disregard the -1 value */
    if ((length < 0) && (errno != EINTR)) {
        perror("error reading the command");
        exit(-1);           /* terminate with error code of -1 */
    }

    for (i = 0; i < length; i++) { /* examine every character in the inputBuffer */

        switch (inputBuffer[i]) {
            case ' ':
            case '\t' :               /* argument separators */
                if (start != -1) {
                    args[ct] = &inputBuffer[start];    /* set up pointer */
                    ct++;
                }
                inputBuffer[i] = '\0'; /* add a null char; make a C string */
                start = -1;
                break;

            case '\n':                 /* should be the final char examined */
                if (start != -1) {
                    args[ct] = &inputBuffer[start];
                    ct++;
                }
                inputBuffer[i] = '\0';
                args[ct] = NULL; /* no more arguments to this command */
                break;

            case '&':                  /* background indicator */
                *background = 1;
                inputBuffer[i] = '\0';
                break;

            default :             /* some other character */
                if (start == -1)
                    start = i;
        } /* end of switch */
    }    /* end of for */
    args[ct] = NULL; /* just in case the input line was > 80 */
} /* end of setup routine */

void run_history(int index) {
    char path[PATH_MAX+1];
    char *exe;
    char *progpath;

    if (index < 0 || index >= HISTORY_SIZE || !history[index]) {
        fprintf(stderr, "Invalid history index\n");
        return;
    }

    char *args[MAX_LINE / 2 + 1];
    int ct = 0;
    char *token = strtok(history[index], " \t");
    while (token) {
        args[ct] = token;
        ct++;
        token = strtok(NULL, " \t");
    }
    args[ct] = NULL;

    progpath = strdup(args[0]);
    exe=args[0];

    int input_redirect = 0, output_redirect = 0, append_redirect = 0, error_redirect = 0;
    char *input_file = NULL, *output_file = NULL, *error_file = NULL;
    int i;
    for (i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            input_redirect = 1;
            input_file = args[i + 1];

            for (int j = i; args[j] != NULL; j++) {
                args[j] = args[j + 2];
            }
        } else if (strcmp(args[i], ">") == 0) {
            output_redirect = 1;
            output_file = args[i + 1];
            args[i] = NULL;
        } else if (strcmp(args[i], ">>") == 0) {
            append_redirect = 1;
            output_file = args[i + 1];
            args[i] = NULL;
        } else if (strcmp(args[i], "2>") == 0) {
            error_redirect = 1;
            error_file = args[i + 1];
            args[i] = NULL;
        }
    }

    if(!findPath(path, exe)){ /*Checks the existence of program*/
        fprintf(stderr, "No executable \"%s\" found\n", exe);
        free(progpath);
    } else {
        int pid = fork();
        if (pid == 0) {
            ioRedirection(input_redirect, output_redirect, append_redirect, error_redirect, input_file, output_file, error_file);

            execv(path, args);
            perror("execvp");
            exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }
}

int main(void) {
    int background;
    char *args[MAX_LINE/2 + 1];
    int pid;
    signal(SIGTSTP, sigtstp_handler);

    char path[PATH_MAX+1];
    char *exe;
    char *progpath;

    while (1) {
        background = 0;
        printf("%s", "myshell: ");
        fflush(0);
        setup(inputBuffer, args, &background);

        if(strcmp(args[0], "history") == 0) {
            if (args[1] && strcmp(args[1], "-i") == 0) {
                int index = atoi(args[2]);
                run_history(index);
            } else {
                print_history();
            }

            continue;
        }

        if (strcmp(args[0], "exit") == 0) {
            exitCall();
            add_to_history(args);
        }

        if (strcmp(args[0], "fg") == 0) {
            add_to_history(args);
            fg(args);
            continue;
        }

        if (strcmp(args[0], "jobs") == 0) {
            add_to_history(args);
            list_background_processes();
            continue;
        }

        progpath = strdup(args[0]);
        exe=args[0];

        if(!findPath(path, exe)){ /*Checks the existence of program*/
            fprintf(stderr, "No executable \"%s\" found\n", exe);
            free(progpath);
        } else {
            add_to_history(args);

            int input_redirect = 0, output_redirect = 0, append_redirect = 0, error_redirect = 0;

            char *input_file = NULL, *output_file = NULL, *error_file = NULL;
            int i;
            for (i = 0; args[i] != NULL; i++) {
                if (strcmp(args[i], "<") == 0) {
                    input_redirect = 1;
                    input_file = args[i + 1];

                    for (int j = i; args[j] != NULL; j++) {
                        args[j] = args[j + 2];
                    }
                } else if (strcmp(args[i], ">") == 0) {
                    output_redirect = 1;
                    output_file = args[i + 1];
                    args[i] = NULL;
                } else if (strcmp(args[i], ">>") == 0) {
                    append_redirect = 1;
                    output_file = args[i + 1];
                    args[i] = NULL;
                } else if (strcmp(args[i], "2>") == 0) {
                    error_redirect = 1;
                    error_file = args[i + 1];
                    args[i] = NULL;
                }
            }

            /* fork a child process to execute the command */
            pid = fork();
            if (pid == 0) {
                ioRedirection(input_redirect, output_redirect, append_redirect, error_redirect, input_file, output_file,
                              error_file);
                /* child process */
                /* execute the command */
                execv(path, args);
                perror("execvp");
                exit(1);
            } else if (pid > 0) {
                /* parent process */
                if (background == 0) {
                    foreground_pid = pid;
                    /* foreground process, wait for it to finish */
                    int status;
                    waitpid(pid, &status, WUNTRACED);
                } else {
                    /* background process, don't wait for it to finish */
                    printf("Background process %d started\n", pid);
                    background_pids[background_count++] = pid;
                }
            } else {
                /* error occurred while forking */
                perror("fork");
            }
        }

        path[0] = '\0';
    }
}

void exitCall() {// check if there are any background processes still running
    int running_count = 0;
    for (int i = 0; i < background_count; i++) {
        int status;
        pid_t pid = waitpid(background_pids[i], &status, WNOHANG);
        if (pid == 0) {
            // background process is still running
            running_count++;
        }
    }

    if (running_count > 0) {
        // there are still background processes running
        printf("There are still %d background processes running.\n", running_count);
        printf("Please terminate all background processes before exiting.\n");
        return;
    } else {
        // no background processes are running, so exit the shell
        exit(0);
    }
}

void
ioRedirection(int input_redirect, int output_redirect, int append_redirect, int error_redirect, const char *input_file, const char *output_file, const char *error_file) {
    if (input_redirect) {
        int fd = open(input_file, O_RDONLY);
        if (fd == -1) {
            perror("open");
            exit(1);
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            exit(1);
        }

        if(close(fd) == -1){
            fprintf(stderr, "%s", "Failed to close the input file\n");
            exit(1);
        }
    }
    if (output_redirect) {
        int fd;
        if (append_redirect) {
            fd = open(output_file, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
        } else {
            fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        }
        if (fd == -1) {
            perror("open");
            exit(1);
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            exit(1);
        }

        if(close(fd) == -1){
            fprintf(stderr, "%s", "Failed to close the input file\n");
            exit(1);
        }
    }
    if (error_redirect) {
        int fd = open(error_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            perror("open");
            exit(1);
        }
        if (dup2(fd, STDERR_FILENO) == -1) {
            perror("dup2");
            exit(1);
        }

        if(close(fd) == -1){
            fprintf(stderr, "%s", "Failed to close the input file\n");
            exit(1);
        }
    }
}

void fg(char *const *args) {// check if a process ID was specified
    if (args[1] == NULL) {
        printf("Error: no process ID specified\n");
        return;
    }

    // convert the process ID to an integer
    int process_id = atoi(args[1]);

    // check if the process ID is valid
    int found = 0;
    for (int i = 0; i < background_count; i++) {
        if (background_pids[i] == process_id) {
            found = 1;
            break;
        }
    }
    if (!found) {
        printf("Error: invalid process ID\n");
        return;
    }

    // bring the specified background process to the foreground
    int status;
    kill(process_id, SIGCONT);
    foreground_pid = process_id;
    waitpid(process_id, &status, WUNTRACED);
}
