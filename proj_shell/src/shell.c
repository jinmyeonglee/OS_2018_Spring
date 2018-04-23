/**
  * this file is for assignment about making simple linux shell.
  *
  * @author   Hanyang Univ. JinMyeong Lee (jinious1111@naver.com)
  * @since    2018-03-15
  * @deadlne  2019-03-29
*/

#include "shell.h"

// functions in this program
int tokenizer(char *input_cmds, char*** tokenized_cmds);
void remove_whitespace(char* source);
char * trim(char* s);
int exec_cmd(int count_, char ***cmds_);
int exec_batch(FILE * batch, char ***cmds_);

// global variable in this program
sigset_t g_sigset; // used to signal mask the shell process

int main (int argc, char **argv) {
  FILE *batchfile;
  char buffer[MAX_ARRAY_SIZE]; // store the input string
  char ***cmds = (char***)malloc(sizeof(char**) * MAX_CMD_COUNT);
  // cmds is pointing the matrix of strings, so It must be triple pointer
  int cmds_num;
  char *isEnd;

  // make a empty set of signal and mask the shell process
  sigemptyset(&g_sigset);
  sigaddset(&g_sigset, SIGINT);
  sigaddset(&g_sigset, SIGALRM);
  sigaddset(&g_sigset, SIGTSTP);
  sigprocmask(SIG_BLOCK, &g_sigset, NULL);

  if(argc > 1) { // if the batchfile is input
    batchfile = fopen(argv[1], "r");
    exec_batch(batchfile, cmds);
    sigprocmask(SIG_UNBLOCK, &g_sigset, NULL); // unblock signal mask
    return 0;
  }

  while(TRUE) { // strat the shell, when interactive mode
    printf("prompt> "); //print the shell
    isEnd = fgets(buffer, MAX_ARRAY_SIZE, stdin);

    if(isEnd == NULL) { // when ctrl + D is entered
      printf("\n");
      break;
    }

    cmds_num = tokenizer(buffer, cmds);

    if(strlen(buffer) == 0) { // if the input is empyt
      continue;
    }
    if(exec_cmd(cmds_num, cmds) != 0) { // if "quit" command is in input string
      break;
    }
  }

  free(cmds);
  sigprocmask(SIG_UNBLOCK, &g_sigset, NULL); // unblock signal mask
  return 0;
}

/**
  * this function is used to execute commands.
  * @param[in]  count_  the number of commands
  * @param[in]  cmds_   stroring the tokenized commands
  * @return     general case return 0. when quit is in commands, return -1
*/
int exec_cmd(int count_, char ***cmds_) {
  int pid, status;

  for(int i = 0; i < count_; i++) {
    if(strcmp(cmds_[i][0], "quit") == 0) {
      sigprocmask(SIG_UNBLOCK, &g_sigset, NULL);
      return -1;
    }
    if(strlen(cmds_[i][0]) == 0) {
      continue;
    }

    pid = fork();

    if(pid == 0) { // child process
      sigprocmask(SIG_UNBLOCK, &g_sigset, NULL);
      if(execvp(cmds_[i][0], cmds_[i]) == -1) {
        fprintf(stderr, "ERROR: %s\n", strerror(errno));
        // if the child process finishes before the parent process calling
        // waitpid(), the child process becames zombie process and can be
        // scheduled again. So MUST KILL THE CHILD PROCESS.
        kill(getpid(), SIGKILL);
        return -1;
      }
    }
    else if(pid > 0) {
      if(waitpid(pid, NULL, 0) < 0) {
        perror("Failed to collect child process.");
      }
    }
    else { // fork failed
      printf("ERROR: fork failed\n");
      return -1;
    }
  }
  return 0;
}

/**
  * this function is used to tokenize commands in one input string.
  * @param[in]  input_cmds      input string which contains commands
  * @param[out] tokenized_cmds  store the tokenized commands
  * @return     return the number of commands
*/
int tokenizer(char *input_cmds, char*** tokenized_cmds) {
  char *ptr;
  int i = 0; // to be the number of commands

  trim(input_cmds);

  //start to tokenize for ";" and store
  ptr = strtok(input_cmds, ";");
  tokenized_cmds[i] = (char**)malloc(sizeof(char*) * MAX_ARRAY_SIZE);
  tokenized_cmds[i][0] = (char*)malloc(sizeof(char) * MAX_ARRAY_SIZE);
  strcpy(tokenized_cmds[i][0], ptr);
  i++;

  while(ptr = strtok(NULL, ";")) {
    tokenized_cmds[i] = (char**)malloc(sizeof(char*) * MAX_ARRAY_SIZE);
    tokenized_cmds[i][0] = (char*)malloc(sizeof(char) * MAX_STRING_SIZE);
    strcpy(tokenized_cmds[i][0], ptr);
    //printf("%s\n", tokenized_cmds[i][0]);
    i++;
  }
  //finished to tokenize using ";"

  //start to tokenize for " " and store
  for(int j = 0; j < i; j++) {
    ptr = strtok(tokenized_cmds[j][0], " ");
    remove_whitespace(tokenized_cmds[j][0]);
    int k = 1;
    while(ptr = strtok(NULL, " ")) {
      tokenized_cmds[j][k] = (char*)malloc(sizeof(char) * MAX_STRING_SIZE);
      remove_whitespace(ptr);
      strcpy(tokenized_cmds[j][k], ptr);
      k++;
    }
    tokenized_cmds[j][k] = NULL; // make the last char * be NULL
  }
  //finished to tokenize using " "

  return i; // return the number of commands
}

/**
  * this function is used to remove whitespace in some string.
  * @param[inout]  source  string to be clean without whitespace
*/
void remove_whitespace(char* source) {
  char* i = source;
  char* j = source;

  while(*j != 0) {
    *i = *j++;
    if(*i != ' ' && *i != '\t' && *i != '\n') {
      i++;
    }
  }
  *i = 0;
}
/**
  * this function is used to trim string
  * @param[inout]  string before trim
  * @return        trimmed string
*/
char *trim(char *s) {
  char *end;

  // trim front of string
  while(*s == ' ' || *s == '\t' || *s == ';' || *s == '\n') {
    s++;
  }
  if(*s == 0) {
    return s;
  }
  end = s + strlen(s) - 1;

  // trim the last of string
  while(*end == ' ' || *end == '\t' || *end == ';' || *end == '\n') {
    end--;
  }
  *(end + 1) = 0; // make the last character be NULL
  return s;
}

/**
  * this function is used to execute the batchfile
  * @param[in]     batch  file stream for input batchfile
  * @param[inout]  cmds_  to contain tokenized commands and execute commands
  * @return        in general case return 0, or return error number
  */
int exec_batch(FILE * batch, char ***cmds_) {
  char buffer[MAX_ARRAY_SIZE];
  int count;

  if(batch == NULL) { // when the file cannot open
    fprintf(stderr, "ERROR: %s\n", strerror(errno));
    return -1;
  }

  while(fgets(buffer, MAX_ARRAY_SIZE, batch)) {
    fputs(buffer, stdout);

    count = tokenizer(buffer, cmds_);

    // if "quit" is in input exec_cmd, return non-zero
    if(exec_cmd(count, cmds_) != 0) {
      //kill(getpid(), SIGKILL);
    }
  }
  fclose(batch);
  return 0;
}
