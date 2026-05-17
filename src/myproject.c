/*
 * lsh — Little Shell
 * Based on the original by Stephen Brennan
 * https://brennan.io/2015/01/16/write-a-shell-in-c/
 *
 * Extended with builtins: pwd, echo, history, env
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────
   HISTORY
   ───────────────────────────────────────────── */
#define HISTORY_MAX 100
static char *history[HISTORY_MAX];
static int   history_count = 0;

void history_add(const char *line)
{
    if (!line || line[0] == '\0') return;
    if (history_count < HISTORY_MAX) {
        history[history_count++] = strdup(line);
    } else {
        /* drop oldest, shift everything down */
        free(history[0]);
        memmove(history, history + 1,
                (HISTORY_MAX - 1) * sizeof(char *));
        history[HISTORY_MAX - 1] = strdup(line);
    }
}

void history_free(void)
{
    for (int i = 0; i < history_count; i++) {
        free(history[i]);
        history[i] = NULL;
    }
    history_count = 0;
}

/* ─────────────────────────────────────────────
   BUILTIN DECLARATIONS
   ───────────────────────────────────────────── */
int lsh_cd(char **args);
int lsh_pwd(char **args);
int lsh_echo(char **args);
int lsh_history(char **args);
int lsh_env(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);

/* Parallel arrays: name → function pointer */
char *builtin_str[] = {
    "cd",
    "pwd",
    "echo",
    "history",
    "env",
    "help",
    "exit"
};

int (*builtin_func[])(char **) = {
    &lsh_cd,
    &lsh_pwd,
    &lsh_echo,
    &lsh_history,
    &lsh_env,
    &lsh_help,
    &lsh_exit
};

int lsh_num_builtins(void)
{
    return sizeof(builtin_str) / sizeof(char *);
}

/* ─────────────────────────────────────────────
   BUILTIN IMPLEMENTATIONS
   ───────────────────────────────────────────── */

/* cd — change directory */
int lsh_cd(char **args)
{
    if (args[1] == NULL) {
        fprintf(stderr, "lsh: cd: expected argument\n");
    } else if (chdir(args[1]) != 0) {
        perror("lsh: cd");
    }
    return 1;
}

/* pwd — print working directory */
int lsh_pwd(char **args)
{
    (void)args;                     /* unused */
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        perror("lsh: pwd");
    } else {
        printf("%s\n", buf);
    }
    return 1;
}

/* echo — print arguments */
int lsh_echo(char **args)
{
    if (args[1] == NULL) {
        printf("\n");               /* echo with no args → blank line */
        return 1;
    }
    for (int i = 1; args[i] != NULL; i++) {
        printf("%s", args[i]);
        if (args[i + 1] != NULL) printf(" ");
    }
    printf("\n");
    return 1;
}

/* history — print numbered command history */
int lsh_history(char **args)
{
    (void)args;
    if (history_count == 0) {
        printf("No commands in history.\n");
    } else {
        for (int i = 0; i < history_count; i++) {
            printf("%4d  %s\n", i + 1, history[i]);
        }
    }
    return 1;
}

/* env — print all environment variables */
int lsh_env(char **args)
{
    (void)args;
    extern char **environ;
    for (char **ep = environ; *ep != NULL; ep++) {
        printf("%s\n", *ep);
    }
    return 1;
}

/* help — list builtins */
int lsh_help(char **args)
{
    (void)args;
    printf("LSH — Little Shell\n");
    printf("Built-in commands:\n");
    for (int i = 0; i < lsh_num_builtins(); i++) {
        printf("  %s\n", builtin_str[i]);
    }
    printf("Use `man <command>` for external programs.\n");
    return 1;
}

/* exit — quit the shell */
int lsh_exit(char **args)
{
    (void)args;
    history_free();
    return 0;   /* returning 0 signals the main loop to stop */
}

/* ─────────────────────────────────────────────
   LAUNCH (fork + exec for external programs)
   ───────────────────────────────────────────── */
int lsh_launch(char **args)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* child */
        if (execvp(args[0], args) == -1) {
            perror("lsh");
        }
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("lsh: fork");
    } else {
        /* parent: wait for child */
        int status;
        do {
            waitpid(pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
    return 1;
}

/* ─────────────────────────────────────────────
   EXECUTE — builtin or external?
   ───────────────────────────────────────────── */
int lsh_execute(char **args)
{
    if (args[0] == NULL) return 1;  /* empty command */

    for (int i = 0; i < lsh_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }
    return lsh_launch(args);
}

/* ─────────────────────────────────────────────
   READ LINE
   ───────────────────────────────────────────── */
#define LSH_RL_BUFSIZE 1024

char *lsh_read_line(void)
{
    char *line = NULL;
    size_t bufsize = 0;
    if (getline(&line, &bufsize, stdin) == -1) {
        if (feof(stdin)) {
            free(line);
            printf("\n");
            exit(EXIT_SUCCESS);
        } else {
            perror("lsh: getline");
            exit(EXIT_FAILURE);
        }
    }
    /* strip trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    return line;
}

/* ─────────────────────────────────────────────
   TOKENIZE (split into args)
   ───────────────────────────────────────────── */
#define LSH_TOK_BUFSIZE 64
#define LSH_TOK_DELIM   " \t\r\n\a"

char **lsh_split_line(char *line)
{
    int    bufsize = LSH_TOK_BUFSIZE;
    int    pos     = 0;
    char **tokens  = malloc(bufsize * sizeof(char *));
    char  *token;

    if (!tokens) { perror("lsh: malloc"); exit(EXIT_FAILURE); }

    token = strtok(line, LSH_TOK_DELIM);
    while (token != NULL) {
        tokens[pos++] = token;
        if (pos >= bufsize) {
            bufsize += LSH_TOK_BUFSIZE;
            tokens = realloc(tokens, bufsize * sizeof(char *));
            if (!tokens) { perror("lsh: realloc"); exit(EXIT_FAILURE); }
        }
        token = strtok(NULL, LSH_TOK_DELIM);
    }
    tokens[pos] = NULL;
    return tokens;
}

/* ─────────────────────────────────────────────
   MAIN LOOP
   ───────────────────────────────────────────── */
void lsh_loop(void)
{
    char  *line;
    char **args;
    int    status;

    do {
        printf("lsh> ");
        fflush(stdout);

        line = lsh_read_line();

        /* save to history (skip empty lines) */
        if (line && line[0] != '\0') {
            history_add(line);
        }

        args   = lsh_split_line(line);
        status = lsh_execute(args);

        free(line);
        free(args);
    } while (status);
}

int main(void)
{
    printf("LSH — Little Shell  (type 'help' for commands, 'exit' to quit)\n");
    lsh_loop();
    return EXIT_SUCCESS;
}