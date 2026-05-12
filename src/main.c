#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dirent.h>

static const char *builtins[] = { "echo", "exit", "type", "pwd", "cd", NULL};

static int tab_press_count = 0;

static char *builtins_generator(const char *text, int state){
    static int idx;
    static int len;
    static char **matches;
    static int match_count;

    if(state == 0){
        idx = 0;
        len = strlen(text);
        match_count = 0;
        matches = NULL;

        for(int i = 0; builtins[i] != NULL; i++){
            if(strncmp(builtins[i], text, len) == 0){
                matches = realloc(matches, sizeof(char*) * (match_count + 1));
                matches[match_count++] = strdup(builtins[i]);
            }
        }

        char *path_env = getenv("PATH");
        if(path_env){
            char path_copy[4096];
            strncpy(path_copy, path_env, sizeof(path_copy));
            path_copy[sizeof(path_copy) - 1] = '\0';

            char *dir = strtok(path_copy, ":");
            while(dir){
                DIR *dp = opendir(dir);
                if(dp){
                    struct dirent *entry;
                    while((entry = readdir(dp)) != NULL){
                        if(strncmp(entry->d_name, text, len) == 0){
                            char full_path[4096];
                            snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
                            if(access(full_path, X_OK) == 0){
                                matches = realloc(matches, sizeof(char*) * (match_count + 1));
                                matches[match_count++] = strdup(entry->d_name);
                            }
                        }
                    }
                    closedir(dp);
                }
                dir = strtok(NULL, ":");
            }
        }
    }

    if(idx < match_count)
        return matches[idx++];

    free(matches);
    matches = NULL;
    return NULL;
}

static char **shell_completion(const char *text, int start, int end){
    (void)end;
    if(start != 0) return NULL;

    char **matches = rl_completion_matches(text, builtins_generator);

    int count = 0;
    if(matches) while(matches[count]) count++;

    if(count == 0){
        tab_press_count = 0;
        rl_attempted_completion_over = 1;
        return NULL;
    }

    if(count == 1){
        tab_press_count = 0;
        rl_attempted_completion_over = 1;
        return matches;
    }

    char lcp[1024];
    strncpy(lcp, matches[0], sizeof(lcp));
    lcp[sizeof(lcp)-1] = '\0';
    for(int i = 1; i < count; i++){
        int j = 0;
        while(lcp[j] && matches[i][j] && lcp[j] == matches[i][j]) j++;
        lcp[j] = '\0';
    }

    if(strlen(lcp) > strlen(text)){
        tab_press_count = 0;
        rl_insert_text(lcp + strlen(text));
        rl_redisplay();
        for(int i = 0; i < count; i++) free(matches[i]);
        free(matches);
        rl_attempted_completion_over = 1;
        return NULL;
    }

    tab_press_count++;

    if(tab_press_count == 1){
        write(STDOUT_FILENO, "\x07", 1);
        for(int i = 0; i < count; i++) free(matches[i]);
        free(matches);
        rl_attempted_completion_over = 1;
        return NULL;
    }

    tab_press_count = 0;

    for(int i = 0; i < count - 1; i++)
        for(int j = i + 1; j < count; j++)
            if(strcmp(matches[i], matches[j]) > 0){
                char *tmp = matches[i];
                matches[i] = matches[j];
                matches[j] = tmp;
            }

    printf("\n");
    for(int i = 0; i < count; i++){
        if(i > 0) printf("  ");
        printf("%s", matches[i]);
    }
    printf("\n");
    printf("$ %s", rl_line_buffer);
    fflush(stdout);

    for(int i = 0; i < count; i++) free(matches[i]);
    free(matches);
    rl_attempted_completion_over = 1;
    return NULL;
}


int parseArgs(char *input, char **args, int max_args) {
    int argc = 0;
    int i = 0;

    while(input[i] != '\0' && argc < max_args - 1) {
        while(input[i] == ' ') i++;
        if(input[i] == '\0') break;

        char buf[1024];
        int buf_i = 0;

        while(input[i] != '\0') {
            if(input[i] == '\\') {
                i++;
                if(input[i] != '\0')
                    buf[buf_i++] = input[i++];
            } else if(input[i] == '\'') {
                i++;
                while(input[i] != '\0' && input[i] != '\'')
                    buf[buf_i++] = input[i++];
                if(input[i] == '\'') i++;
            } else if(input[i] == '"') {
                i++;
                while(input[i] != '\0' && input[i] != '"'){
                    if(input[i] == '\\' && (input[i+1] == '\\' || input[i+1] == '"')){
                        i++;
                        buf[buf_i++] = input[i++];
                    } else {
                        buf[buf_i++] = input[i++];
                    }
                }
                if(input[i] == '"') i++;
            } else if(input[i] == ' ') {
                break;
            } else {
                buf[buf_i++] = input[i++];
            }
        }

        buf[buf_i] = '\0';
        args[argc++] = strdup(buf);
    }

    args[argc] = NULL;
    return argc;
}

char *extractRedirect(char **args, int *n, int *target_fd, int *append) {
    for(int i = 0; i < *n; i++){
        int fd = -1;
        if(strcmp(args[i], ">") == 0 || strcmp(args[i], "1>") == 0)
            fd = STDOUT_FILENO;
        else if(strcmp(args[i], "2>") == 0)
            fd = STDERR_FILENO;
        else if(strcmp(args[i], ">>") == 0 || strcmp(args[i], "1>>") == 0){
            fd = STDOUT_FILENO;
            *append = 1;
        }
        else if(strcmp(args[i], "2>>") == 0){
            fd = STDERR_FILENO;
            *append = 1;
        }
        if(fd != -1 && i + 1 < *n){
            char *file = args[i+1];
            free(args[i]);
            for(int j = i; j < *n - 2; j++)
                args[j] = args[j+2];
            *n -= 2;
            args[*n] = NULL;
            *target_fd = fd;
            return file;
        }
    }
    *target_fd = -1;
    return NULL;
}

int isBuiltIn(const char *temp){
    return strcmp(temp, "echo") == 0 ||
           strcmp(temp, "exit") == 0 ||
           strcmp(temp, "type") == 0 ||
           strcmp(temp, "pwd") == 0 ||
           strcmp(temp, "cd") == 0;
}

int findInPath(const char *cmd, char *full_path, size_t size){
    char *path_env = getenv("PATH");
    if(!path_env) return 0;
    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy));
    path_copy[sizeof(path_copy)-1] = '\0';
    char *dir = strtok(path_copy, ":");
    while(dir){
        snprintf(full_path, size, "%s/%s", dir, cmd);
        if(access(full_path, X_OK) == 0) return 1;
        dir = strtok(NULL, ":");
    }
    return 0;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    rl_attempted_completion_function = shell_completion;

    char *command;

    while(1){
        command = readline("$ ");
        if(!command) break;
        if(command[0] == '\0'){
            free(command); continue;
        }

        char *args[100];
        int n = parseArgs(command, args, 100);
        if(n == 0) continue;

        char *cmd = args[0];

        if(strcmp(cmd, "exit") == 0){
            for(int j = 0; j < n; j++) free(args[j]);
            break;
        }
        else if(strcmp(cmd, "echo") == 0){
            int target_fd;
            int append = 0;
            char *outfile = extractRedirect(args, &n, &target_fd, &append);
            int save_fd = -1;
            int fd;
            if(outfile){
                save_fd = dup(target_fd);
                if(append == 1) {
                    fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                }
                else {
                    fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                }
               
                dup2(fd, target_fd);
                close(fd);
                free(outfile);
            }

            for(int j = 1; j < n; j++){
                if(j > 1) printf(" ");
                printf("%s", args[j]);
            }
            printf("\n");
            fflush(stdout);

            if(save_fd != -1){
                dup2(save_fd, target_fd);
                close(save_fd);
            }
        }
        else if(strcmp(cmd, "type") == 0){
            if(n < 2){ for(int j=0;j<n;j++) free(args[j]); continue; }
            char *arg = args[1];

            if(isBuiltIn(arg)){
                printf("%s is a shell builtin\n", arg);
            } else {
                char full_path[4096];
                if(findInPath(arg, full_path, sizeof(full_path)))
                    printf("%s is %s\n", arg, full_path);
                else
                    printf("%s: not found\n", arg);
            }
        }
        else if(strcmp(cmd, "pwd") == 0){
            char cwd[4096];
            if(getcwd(cwd, sizeof(cwd)) != NULL) printf("%s\n", cwd);
        }
        else if(strcmp(cmd, "cd") == 0){
            char *target = (n < 2 || strcmp(args[1], "~") == 0) ? getenv("HOME") : args[1];
            if(target == NULL || chdir(target) != 0)
                printf("cd: %s: No such file or directory\n", target ? target : "");
        }
        else {
            int target_fd;
            int append = 0;
            int fd;
            char *outfile = extractRedirect(args, &n, &target_fd, &append);

            char full_path[4096];
            if(!findInPath(cmd, full_path, sizeof(full_path))){
                fprintf(stderr, "%s: not found\n", cmd);
            } else {
                pid_t pid = fork();
                if(pid == 0){
                    if(outfile){
                        if(append == 1) {
                            fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                        }
                        else {
                            fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        }
                        if(fd < 0){ perror("open"); exit(1); }
                        dup2(fd, target_fd);
                        close(fd);
                    }
                    execv(full_path, args);
                    perror("execv");
                    exit(1);
                } else {
                    waitpid(pid, NULL, 0);
                }
            }

            if(outfile) free(outfile);
        }

        for(int j = 0; j < n; j++) free(args[j]);
            free(command);
    }

    return 0;
}