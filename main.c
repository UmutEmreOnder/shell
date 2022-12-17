#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define MAX_LINE 80 /* 80 chars per line, per command, should be enough. */
#define HISTORY_SIZE 10
#define MAX_BACKGROUND_PROCESSES 100

char *history[HISTORY_SIZE];
int history_index = 0;
char inputBuffer[MAX_LINE]; /* buffer to hold command entered */
pid_t foreground_pid = 0;


// global array to store the PIDs of all background processes
pid_t background_pids[MAX_BACKGROUND_PROCESSES];

// global counter for the number of background processes
int background_count = 0;

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

    printf(">>%s<<", inputBuffer);
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

    for (i = 0; i <= ct; i++)
        printf("args %d = %s\n", i, args[i]);
} /* end of setup routine */

void run_history(int index) {
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

    int pid = fork();
    if (pid == 0) {
        execvp(args[0], args);
        perror("execvp");
        exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        perror("fork");
    }
}

int main(void) {
    int background;
    char *args[MAX_LINE/2 + 1];
    int pid;
    signal(SIGTSTP, sigtstp_handler);

    while (1) {
        background = 0;
        printf("myshell: ");
        setup(inputBuffer, args, &background);

        if (strcmp(args[0], "exit") == 0) {
            // check if there are any background processes still running
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
                continue;
            } else {
                // no background processes are running, so exit the shell
                exit(0);
            }
        }

        /* check for built-in commands */
        if (strcmp(args[0], "history") == 0 && args[1] && strcmp(args[1], "-i") == 0) {
            int index = atoi(args[2]);
            run_history(index);
            continue;
        } else if (strcmp(args[0], "history") == 0) {
            print_history();
            continue;
        }



        add_to_history(args);
        /* fork a child process to execute the command */
        pid = fork();
        if (pid == 0) {
            /* child process */
            /* execute the command */
            execvp(args[0], args);
            perror("execvp");
            exit(1);
        } else if (pid > 0) {
            /* parent process */
            if (background == 0) {
                foreground_pid = pid;
                /* foreground process, wait for it to finish */
                int status;
                waitpid(pid, &status, WNOHANG);
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
    return 0;
}