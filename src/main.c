#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);
  char command[1024];
  fget(command, sizeof(command), stdin);
  command[strcspn(command, "\n")] = '\0';
  if(command != " "){
    printf("%s: command not found", command)
  }
  printf("$ ");

  return 0;
}
