#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>


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

    char *cmd = strtok(command, " ");

    if(cmd == NULL) continue;

    if(strcmp(cmd, "exit") == 0){
        break;
    }
    else if (strncmp(cmd, "echo", 5) == 0){
        printf("%s\n", cmd + 5);
    }
    else if(strcmp(cmd, "type") == 0){
      char *arg  = strtok(NULL, " ");
        if(!arg) continue;
        else if(isBuiltIn(arg)){
            printf("%s is a shell builtin\n", arg);
            continue;
        }

        char *path_env = getenv("PATH");
        if(!path_env){
            printf("%s: not found\n", arg);
            continue;
        }

        char path_copy[4096];
        strncpy(path_copy, path_env, sizeof(path_copy));
        char *dir = strtok(path_copy, ":");
        while(dir != NULL){
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, arg);

            if(access(full_path, X_OK) == 0){
                printf("%s is %s\n", arg, full_path);
                break;
            }

            dir = strtok(NULL, ":");
        }

        if(dir == NULL){
            printf("%s: not found\n", arg);
        }
    }
    else if (strcmp(cmd, "pwd") == 0){
        char cwd[4096];

        if(getcwd(cwd, sizeof(cwd)) != NULL){

            printf("%s\n", cwd);
        }        

    }
    else if(strcmp(cmd, "cd") == 0){
        char *arg = strtok(NULL, " ");

        if(arg == NULL){
            continue;
        }

        if(arg[0] == '/'){
            if(chdir(arg) != 0){
                printf("cd: %s: No such file or directory\n", arg);
            }

        }

    }
    else{
        char *arg[100];
        int i = 0;

        arg[i++] = cmd;
        char *token;

        while((token = strtok(NULL, " ")) != NULL){
            arg[i++] = token;
        }
        arg[i] = NULL;
        char *path_env = getenv("PATH");
        if(!path_env){
            printf("%s: not found\n", arg);
            continue;
        }

        char path_copy[4096];
        strncpy(path_copy, path_env, sizeof(path_copy));
        path_copy[sizeof(path_copy) - 1] = '\0';

        char *dir = strtok(path_copy, ":");
        char full_path[4096];
        int found = 0;

        while(dir != NULL){
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);

            if(access(full_path, X_OK) == 0){
                found = 1;
                break;
            }
            dir = strtok(NULL, ":");
        }
        if(!found){
            printf("%s: not found\n", cmd);
            continue;
        }

        pid_t pid = fork();

        if(pid == 0){
            execv(full_path, arg);

            perror("execv");
            exit(1);
        }
        else{
            waitpid(pid, NULL, 0);
        }



    }

    }
  return 0;
}
