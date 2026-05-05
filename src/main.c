#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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
                while(input[i] != '\0' && input[i] != '"')
                    buf[buf_i++] = input[i++];
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

int isBuiltIn(const char *temp){
    return strcmp(temp, "echo") == 0 ||
           strcmp(temp, "exit") == 0 ||
           strcmp(temp, "type") == 0 ||
           strcmp(temp, "pwd") == 0;
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    char command[1024];

    while(1){
        printf("$ ");
        fgets(command, sizeof(command), stdin);
        command[strcspn(command, "\n")] = '\0';

        char *args[100];
        int n = parseArgs(command, args, 100);
        if(n == 0) continue;

        char *cmd = args[0];

        if(strcmp(cmd, "exit") == 0){
            for(int j = 0; j < n; j++) free(args[j]);
            break;
        }
        else if(strcmp(cmd, "echo") == 0){
            for(int j = 1; j < n; j++){
                if(j > 1) printf(" ");
                printf("%s", args[j]);
            }
            printf("\n");
        }
        else if(strcmp(cmd, "type") == 0){
            if(n < 2){ for(int j=0;j<n;j++) free(args[j]); continue; }
            char *arg = args[1];

            if(isBuiltIn(arg)){
                printf("%s is a shell builtin\n", arg);
            } else {
                char *path_env = getenv("PATH");
                if(!path_env){ printf("%s: not found\n", arg); }
                else {
                    char path_copy[4096];
                    strncpy(path_copy, path_env, sizeof(path_copy));
                    path_copy[sizeof(path_copy)-1] = '\0';
                    char *dir = strtok(path_copy, ":");
                    int found = 0;
                    while(dir != NULL){
                        char full_path[4096];
                        snprintf(full_path, sizeof(full_path), "%s/%s", dir, arg);
                        if(access(full_path, X_OK) == 0){
                            printf("%s is %s\n", arg, full_path);
                            found = 1;
                            break;
                        }
                        dir = strtok(NULL, ":");
                    }
                    if(!found) printf("%s: not found\n", arg);
                }
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
            char *path_env = getenv("PATH");
            if(!path_env){ printf("%s: not found\n", cmd); }
            else {
                char path_copy[4096];
                strncpy(path_copy, path_env, sizeof(path_copy));
                path_copy[sizeof(path_copy)-1] = '\0';
                char *dir = strtok(path_copy, ":");
                char full_path[4096];
                int found = 0;
                while(dir != NULL){
                    snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
                    if(access(full_path, X_OK) == 0){ found = 1; break; }
                    dir = strtok(NULL, ":");
                }
                if(!found){
                    printf("%s: not found\n", cmd);
                } else {
                    pid_t pid = fork();
                    if(pid == 0){
                        execv(full_path, args);
                        perror("execv");
                        exit(1);
                    } else {
                        waitpid(pid, NULL, 0);
                    }
                }
            }
        }

        for(int j = 0; j < n; j++) free(args[j]);
    }

    return 0;
}