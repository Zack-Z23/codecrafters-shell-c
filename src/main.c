#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int isBuiltIn(const char *temp){
    return strcmp(cmd, "echo") == 0 ||
           strcmp(cmd, "exit") == 0 ||
           strcmp(cmd, "type") == 0;
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  char command[1024];
    while(1){
    printf("$ ")
    fgets(input, sizeof(input), stdin);

    command[strcspn(command, "\n")] = '\0';

    char *cmd = strtok(command, " ");

    if(cmd == NULL) continue;

    if(strcmp(cmd, "exit") == 0){
        break;
    }
    else if (strcmp(cmd, "echo") == 0){
        char *arg = strtok(NULL, " ");
        if(arg) printf("%s\n". arg);
        else printf("\n");
    }
    else if(strcmp(cmd, "type") == 0){
      char *arg  = strtok(NULL, " ");
        if(isBuiltIn(arg)){
            printf("%s is shell buitlin\n", arg);
        }
        else{
            printf("%s: not found\n", arg);
        }
    }
    else{
        printf("%s: not found\n", cmd);
    }

    }
  return 0;
}
