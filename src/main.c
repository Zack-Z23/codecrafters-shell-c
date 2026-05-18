#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *builtins[] = { "echo", "exit", "type", "pwd", "cd", "complete", "jobs", NULL};

static int tab_press_count = 0;
static int next_job_number = 1;

#define MAX_JOBS 256
typedef struct {
    int job_number;
    pid_t pid;
    char *command;
    int active;
    int done;
} Job;

static Job job_table[MAX_JOBS];
static int job_count = 0;
static int alloc_job_number(void){
    for(int n = 1; ; n++){
        int taken = 0;
        for(int i = 0; i < MAX_JOBS; i++){
            if(job_table[i].active && job_table[i].job_number == n){
                taken = 1; break;
            }
        }
        if(!taken) return n;
    }
}
static int split_pipeline(char **args, int n, char ***segments, int *seg_lens, int max_segs){
    int seg_count = 0;
    int seg_start = 0;
    segments[0] = args;
    for(int i = 0; i <= n; i++){
        if(i == n || (args[i] && strcmp(args[i], "|") == 0)){
            seg_lens[seg_count] = i - seg_start;
            seg_count++;
            if(i < n){
                free(args[i]);
                args[i] = NULL;
                seg_start = i + 1;
                if(seg_count < max_segs)
                    segments[seg_count] = args + seg_start;
            }
        }
    }
    return seg_count;
}
static void add_job(int job_number, pid_t pid, const char *command){
    for(int i = 0; i < MAX_JOBS; i++){
        if(!job_table[i].active){
            job_table[i].job_number = job_number;
            job_table[i].pid       = pid;
            job_table[i].command   = strdup(command);
            job_table[i].active    = 1;
            job_count++;
            return;
        }
    }
}

static char *cached_candidates[256];
static int cached_cand_count = 0;
static char cached_line[4096] = "";
static int cached_start = -1;

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
                                int dup = 0;
                                for(int k = 0; k < match_count; k++){
                                    if(strcmp(matches[k], entry->d_name) == 0){
                                        dup = 1; break;
                                    }
                                }
                                if(!dup){
                                    matches = realloc(matches, sizeof(char*) * (match_count + 1));
                                    matches[match_count++] = strdup(entry->d_name);
                                }
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

static char *filename_generator(const char *text, int state){
    static DIR *dp;
    static int len;

    if(state == 0){
        if(dp) closedir(dp);
        dp = opendir(".");
        len = strlen(text);
    }

    if(!dp) return NULL;

    struct dirent *entry;
    while((entry = readdir(dp)) != NULL){
        if(strncmp(entry->d_name, text, len) == 0)
            return strdup(entry->d_name);
    }

    closedir(dp);
    dp = NULL;
    return NULL;
}

static const char *find_completion(const char *command);
static void register_completion(const char *command, const char *script);

static char **shell_completion(const char *text, int start, int end){
    (void)end;

    if(start == 0){
        tab_press_count = 0;
        return rl_completion_matches(text, builtins_generator);
    }

    {
        char cmd_name[256];
        int ci = 0;
        const char *buf = rl_line_buffer;
        while(*buf == ' ') buf++;
        while(*buf && *buf != ' ' && ci < (int)sizeof(cmd_name) - 1)
            cmd_name[ci++] = *buf++;
        cmd_name[ci] = '\0';

        const char *script = find_completion(cmd_name);
        if(script){
            rl_attempted_completion_over = 1;
            char prev_word[1024] = "";
            {
                char linebuf[4096];
                strncpy(linebuf, rl_line_buffer, sizeof(linebuf));
                linebuf[sizeof(linebuf)-1] = '\0';
                if(start < (int)sizeof(linebuf)) linebuf[start] = '\0';

                char *p = linebuf;
                while(*p){
                    while(*p == ' ') p++;
                    if(*p == '\0') break;
                    char *tok_start = p;
                    while(*p && *p != ' ') p++;
                    int tok_len = p - tok_start;
                    if(tok_len > 0){
                        int copy_len = tok_len < (int)sizeof(prev_word)-1 ? tok_len : (int)sizeof(prev_word)-1;
                        strncpy(prev_word, tok_start, copy_len);
                        prev_word[copy_len] = '\0';
                    }
                }
            }

            char **candidates = NULL;
            int cand_count = 0;
            char cand_buf[4096];

            int line_changed = (strcmp(cached_line, rl_line_buffer) != 0 || cached_start != start);
            if(!line_changed && cached_cand_count >= 0){
                candidates = cached_candidates;
                cand_count = cached_cand_count;
            } else {
                int pipefd[2];
                if(pipe(pipefd) == 0){
                    pid_t pid = fork();
                    if(pid == 0){
                        close(pipefd[0]);
                        dup2(pipefd[1], STDOUT_FILENO);
                        close(pipefd[1]);
                        setenv("COMP_LINE", rl_line_buffer, 1);
                        char comp_point_str[32];
                        snprintf(comp_point_str, sizeof(comp_point_str), "%d", rl_point);
                        setenv("COMP_POINT", comp_point_str, 1);
                        execlp(script, script, cmd_name, text, prev_word, NULL);
                        exit(1);
                    }
                    close(pipefd[1]);

                    int total = 0;
                    ssize_t n;
                    while((n = read(pipefd[0], cand_buf + total, sizeof(cand_buf) - total - 1)) > 0)
                        total += n;
                    cand_buf[total] = '\0';
                    close(pipefd[0]);
                    waitpid(pid, NULL, 0);

                    for(int i = 0; i < cached_cand_count; i++) free(cached_candidates[i]);
                    cached_cand_count = 0;

                    char *line2 = cand_buf;
                    while(*line2 && cand_count < 256){
                        char *nl = strchr(line2, '\n');
                        if(nl) *nl = '\0';
                        int ll = strlen(line2);
                        if(ll > 0 && line2[ll-1] == '\r') line2[ll-1] = '\0';
                        if(strlen(line2) > 0){
                            cached_candidates[cand_count++] = strdup(line2);
                        }
                        if(nl) line2 = nl + 1;
                        else break;
                    }
                    cached_cand_count = cand_count;
                    strncpy(cached_line, rl_line_buffer, sizeof(cached_line)-1);
                    cached_line[sizeof(cached_line)-1] = '\0';
                    cached_start = start;
                    candidates = cached_candidates;
                }
            }

            if(cand_count == 1){
                rl_delete_text(start, rl_end);
                rl_point = start;
                rl_insert_text(candidates[0]);
                rl_insert_text(" ");
                rl_redisplay();
                tab_press_count = 0;
                cached_line[0] = '\0';
            } else if(cand_count > 1){
                for(int i = 0; i < cand_count - 1; i++)
                    for(int j = i + 1; j < cand_count; j++)
                        if(strcmp(candidates[i], candidates[j]) > 0){
                            char *tmp = candidates[i];
                            candidates[i] = candidates[j];
                            candidates[j] = tmp;
                        }

                char lcp[1024];
                strncpy(lcp, candidates[0], sizeof(lcp)-1);
                lcp[sizeof(lcp)-1] = '\0';
                for(int i = 1; i < cand_count; i++){
                    int j = 0;
                    while(lcp[j] && candidates[i][j] && lcp[j] == candidates[i][j]) j++;
                    lcp[j] = '\0';
                }

                int text_typed_len = strlen(text);
                int lcp_len = strlen(lcp);

                if(lcp_len > text_typed_len){
                    rl_delete_text(start, rl_end);
                    rl_point = start;
                    rl_insert_text(lcp);
                    rl_redisplay();
                    tab_press_count = 0;
                    cached_line[0] = '\0';
                } else {
                    tab_press_count++;
                    if(tab_press_count == 1){
                        write(STDOUT_FILENO, "\x07", 1);
                    } else {
                        printf("\n");
                        for(int i = 0; i < cand_count; i++){
                            printf("%s", candidates[i]);
                            if(i < cand_count - 1) printf("  ");
                        }
                        printf("\n");
                        fflush(stdout);
                        rl_on_new_line();
                        rl_redisplay();
                        tab_press_count = 0;
                        cached_line[0] = '\0';
                    }
                }
            }
            return NULL;
        }
    }

    rl_attempted_completion_over = 1;

    char dir_path[4096];
    const char *prefix;
    int text_len = strlen(text);

    if(text_len > 0 && text[text_len - 1] == '/'){
        strncpy(dir_path, text, text_len - 1);
        dir_path[text_len - 1] = '\0';
        if(strlen(dir_path) == 0) strcpy(dir_path, ".");
        prefix = "";
    } else {
        char *last_slash = strrchr(text, '/');
        if(last_slash){
            int dir_len = last_slash - text;
            if(dir_len == 0) strcpy(dir_path, "/");
            else {
                strncpy(dir_path, text, dir_len);
                dir_path[dir_len] = '\0';
            }
            prefix = last_slash + 1;
        } else {
            strcpy(dir_path, ".");
            prefix = text;
        }
    }

    DIR *dp = opendir(dir_path);
    if(!dp) return NULL;

    int prefix_len = strlen(prefix);
    char **matches = NULL;
    int match_count = 0;

    struct dirent *entry;
    while((entry = readdir(dp)) != NULL){
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if(strncmp(entry->d_name, prefix, prefix_len) == 0){
            matches = realloc(matches, sizeof(char*) * (match_count + 1));
            matches[match_count++] = strdup(entry->d_name);
        }
    }
    closedir(dp);

    if(match_count == 0){
        if(matches) free(matches);
        return NULL;
    }

    for(int i = 0; i < match_count - 1; i++)
        for(int j = i + 1; j < match_count; j++)
            if(strcmp(matches[i], matches[j]) > 0){
                char *tmp = matches[i];
                matches[i] = matches[j];
                matches[j] = tmp;
            }

    if(match_count == 1){
        char full_match[8192];
        if(strcmp(dir_path, ".") == 0 && strrchr(text, '/') == NULL){
            snprintf(full_match, sizeof(full_match), "%s", matches[0]);
        } else {
            const char *last_slash = strrchr(text, '/');
            if(last_slash){
                int slash_prefix_len = (last_slash - text) + 1;
                snprintf(full_match, sizeof(full_match), "%.*s%s", slash_prefix_len, text, matches[0]);
            } else {
                snprintf(full_match, sizeof(full_match), "%s", matches[0]);
            }
        }

        char stat_path[8192];
        snprintf(stat_path, sizeof(stat_path), "%s/%s", dir_path, matches[0]);
        struct stat st;
        int is_dir = (stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode));

        rl_delete_text(start, rl_point);
        rl_point = start;
        rl_insert_text(full_match);
        rl_insert_text(is_dir ? "/" : " ");
        rl_redisplay();

        free(matches[0]);
        free(matches);
        return NULL;
    }

    char lcp[8192];
    strncpy(lcp, matches[0], sizeof(lcp));
    lcp[sizeof(lcp)-1] = '\0';
    for(int i = 1; i < match_count; i++){
        int j = 0;
        while(lcp[j] && matches[i][j] && lcp[j] == matches[i][j]) j++;
        lcp[j] = '\0';
    }

    if(strlen(lcp) > (size_t)prefix_len){
        char full_match[8192];
        if(strcmp(dir_path, ".") == 0 && strrchr(text, '/') == NULL){
            snprintf(full_match, sizeof(full_match), "%s", lcp);
        } else {
            const char *last_slash = strrchr(text, '/');
            if(last_slash){
                int slash_prefix_len = (last_slash - text) + 1;
                snprintf(full_match, sizeof(full_match), "%.*s%s", slash_prefix_len, text, lcp);
            } else {
                snprintf(full_match, sizeof(full_match), "%s", lcp);
            }
        }
        rl_delete_text(start, rl_point);
        rl_point = start;
        rl_insert_text(full_match);
        rl_redisplay();
        tab_press_count = 0;
    } else {
        tab_press_count++;
        if(tab_press_count == 1){
            write(STDOUT_FILENO, "\x07", 1);
        } else {
            printf("\n");
            for(int i = 0; i < match_count; i++){
                char stat_path[8192];
                snprintf(stat_path, sizeof(stat_path), "%s/%s", dir_path, matches[i]);
                struct stat st;
                int is_dir = (stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode));
                printf("%s%s", matches[i], is_dir ? "/" : "");
                if(i < match_count - 1) printf("  ");
            }
            printf("\n");
            fflush(stdout);
            rl_on_new_line();
            rl_redisplay();
            tab_press_count = 0;
        }
    }

    for(int i = 0; i < match_count; i++) free(matches[i]);
    free(matches);
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
           strcmp(temp, "cd") == 0 ||
           strcmp(temp, "complete") == 0 ||
           strcmp(temp, "jobs") == 0;
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

#define MAX_COMPLETIONS 256
typedef struct {
    char *command;
    char *script;
} CompletionEntry;

static CompletionEntry completion_registry[MAX_COMPLETIONS];
static int completion_count = 0;

static const char *find_completion(const char *command){
    for(int i = 0; i < completion_count; i++)
        if(strcmp(completion_registry[i].command, command) == 0)
            return completion_registry[i].script;
    return NULL;
}

static void register_completion(const char *command, const char *script){
    for(int i = 0; i < completion_count; i++){
        if(strcmp(completion_registry[i].command, command) == 0){
            free(completion_registry[i].script);
            completion_registry[i].script = strdup(script);
            return;
        }
    }
    if(completion_count < MAX_COMPLETIONS){
        completion_registry[completion_count].command = strdup(command);
        completion_registry[completion_count].script  = strdup(script);
        completion_count++;
    }
}

static void remove_completion(const char *command){
    for(int i = 0; i < completion_count; i++){
        if(strcmp(completion_registry[i].command, command) == 0){
            free(completion_registry[i].command);
            free(completion_registry[i].script);
            for(int j = i; j < completion_count - 1; j++)
                completion_registry[j] = completion_registry[j+1];
            completion_count--;
            return;
        }
    }
}

static void reap_jobs(void){
    for(int i = 0; i < MAX_JOBS; i++){
        if(!job_table[i].active || job_table[i].done) continue;
        int status = 0;
        pid_t r = waitpid(job_table[i].pid, &status, WNOHANG);
        if(r == job_table[i].pid && WIFEXITED(status))
            job_table[i].done = 1;
    }

    for(int pass = 1; pass < next_job_number; pass++){
        for(int i = 0; i < MAX_JOBS; i++){
            if(!job_table[i].active || job_table[i].job_number != pass) continue;
            if(!job_table[i].done) continue;

            int cur_num = -1, prev_num = -1;
            for(int k = 0; k < MAX_JOBS; k++){
                if(!job_table[k].active) continue;
                int jn = job_table[k].job_number;
                if(jn > cur_num){ prev_num = cur_num; cur_num = jn; }
                else if(jn > prev_num){ prev_num = jn; }
            }

            char marker;
            if(job_table[i].job_number == cur_num)       marker = '+';
            else if(job_table[i].job_number == prev_num) marker = '-';
            else                                          marker = ' ';

            char display[4096];
            strncpy(display, job_table[i].command, sizeof(display)-1);
            display[sizeof(display)-1] = '\0';
            int dlen = strlen(display);
            if(dlen >= 2 && strcmp(display + dlen - 2, " &") == 0)
                display[dlen - 2] = '\0';

            printf("[%d]%c  %-24s%s\n", job_table[i].job_number, marker, "Done", display);
            fflush(stdout);

            free(job_table[i].command);
            job_table[i].command = NULL;
            job_table[i].active  = 0;
            job_table[i].done    = 0;
            job_count--;
        }
    }
}

static void print_jobs(void){
  
    for(int i = 0; i < MAX_JOBS; i++){
        if(!job_table[i].active || job_table[i].done) continue;
        int status = 0;
        pid_t r = waitpid(job_table[i].pid, &status, WNOHANG);
        if(r == job_table[i].pid && WIFEXITED(status))
            job_table[i].done = 1;
    }


    int cur_num = -1, prev_num = -1;
    for(int i = 0; i < MAX_JOBS; i++){
        if(!job_table[i].active) continue;
        int jn = job_table[i].job_number;
        if(jn > cur_num){ prev_num = cur_num; cur_num = jn; }
        else if(jn > prev_num){ prev_num = jn; }
    }

  
    for(int pass = 1; pass < next_job_number; pass++){
        for(int i = 0; i < MAX_JOBS; i++){
            if(!job_table[i].active || job_table[i].job_number != pass) continue;

            char marker;
            if(job_table[i].job_number == cur_num)       marker = '+';
            else if(job_table[i].job_number == prev_num) marker = '-';
            else                                          marker = ' ';

            if(job_table[i].done){
                char display[4096];
                strncpy(display, job_table[i].command, sizeof(display)-1);
                display[sizeof(display)-1] = '\0';
                int dlen = strlen(display);
                if(dlen >= 2 && strcmp(display + dlen - 2, " &") == 0)
                    display[dlen - 2] = '\0';
                printf("[%d]%c  %-24s%s\n", job_table[i].job_number, marker, "Done", display);
                fflush(stdout);
                free(job_table[i].command);
                job_table[i].command = NULL;
                job_table[i].active  = 0;
                job_table[i].done    = 0;
                job_count--;
            } else {
                printf("[%d]%c  %-24s%s\n",
                       job_table[i].job_number, marker, "Running", job_table[i].command);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    rl_attempted_completion_function = shell_completion;

    char *command;

    while(1){
        reap_jobs();
        command = readline("$ ");
        if(!command) break;
        if(command[0] == '\0'){
            free(command); continue;
        }

        add_history(command);

        char *args[100];
        int n = parseArgs(command, args, 100);
        if(n == 0){ free(command); continue; }

        char *cmd = args[0];

        if(strcmp(cmd, "exit") == 0){
            for(int j = 0; j < n; j++) free(args[j]);
            free(command);
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
                if(append == 1)
                    fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                else
                    fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
            if(n < 2){ for(int j=0;j<n;j++) free(args[j]); free(command); continue; }
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
        else if(strcmp(cmd, "complete") == 0){
            if(n >= 2 && strcmp(args[1], "-p") == 0){
                if(n >= 3){
                    const char *script = find_completion(args[2]);
                    if(script)
                        printf("complete -C '%s' %s\n", script, args[2]);
                    else
                        printf("complete: %s: no completion specification\n", args[2]);
                }
            } else if(n >= 3 && strcmp(args[1], "-r") == 0){
                remove_completion(args[2]);
            } else if(n >= 4 && strcmp(args[1], "-C") == 0){
                register_completion(args[3], args[2]);
            }
        }
        else if(strcmp(cmd, "jobs") == 0){

            print_jobs();
        }
    else {
            int background = 0;
            if(n > 0 && strcmp(args[n-1], "&") == 0){
                background = 1;
                free(args[n-1]);
                args[n-1] = NULL;
                n--;
            }

            /* check for pipeline */
            int has_pipe = 0;
            for(int i = 0; i < n; i++){
                if(strcmp(args[i], "|") == 0){ has_pipe = 1; break; }
            }

            if(has_pipe){
                char **segments[64];
                int seg_lens[64];
                int seg_count = split_pipeline(args, n, segments, seg_lens, 64);

                int pipefds[63][2];
                pid_t pids[64];

                for(int s = 0; s < seg_count - 1; s++)
                    pipe(pipefds[s]);

                for(int s = 0; s < seg_count; s++){
                    char **sargs = segments[s];
                    int slen = seg_lens[s];
                    sargs[slen] = NULL;

                    char full_path[4096];
                    if(!findInPath(sargs[0], full_path, sizeof(full_path))){
                        fprintf(stderr, "%s: not found\n", sargs[0]);
                        pids[s] = -1;
                        continue;
                    }

                    pid_t pid = fork();
                    if(pid == 0){
                        if(s > 0)
                            dup2(pipefds[s-1][0], STDIN_FILENO);
                        if(s < seg_count - 1)
                            dup2(pipefds[s][1], STDOUT_FILENO);
                        for(int p = 0; p < seg_count - 1; p++){
                            close(pipefds[p][0]);
                            close(pipefds[p][1]);
                        }
                        execv(full_path, sargs);
                        perror("execv");
                        exit(1);
                    }
                    pids[s] = pid;
                }

                for(int p = 0; p < seg_count - 1; p++){
                    close(pipefds[p][0]);
                    close(pipefds[p][1]);
                }

                for(int s = 0; s < seg_count; s++)
                    if(pids[s] != -1) waitpid(pids[s], NULL, 0);

            } else {
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
                            if(append == 1)
                                fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                            else
                                fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if(fd < 0){ perror("open"); exit(1); }
                            dup2(fd, target_fd);
                            close(fd);
                        }
                        execv(full_path, args);
                        perror("execv");
                        exit(1);
                    } else {
                        if(background){
                            int job_num = alloc_job_number();
                            if(job_num >= next_job_number) next_job_number = job_num + 1;
                            printf("[%d] %d\n", job_num, (int)pid);
                            fflush(stdout);
                            char cmd_str[4096] = "";
                            for(int j = 0; j < n; j++){
                                if(j > 0) strncat(cmd_str, " ", sizeof(cmd_str)-strlen(cmd_str)-1);
                                strncat(cmd_str, args[j], sizeof(cmd_str)-strlen(cmd_str)-1);
                            }
                            strncat(cmd_str, " &", sizeof(cmd_str)-strlen(cmd_str)-1);
                            add_job(job_num, pid, cmd_str);
                        } else {
                            waitpid(pid, NULL, 0);
                        }
                    }
                }

                if(outfile) free(outfile);
            }
        }
        for(int j = 0; j < n; j++) free(args[j]);
        free(command);
    }

    return 0;
}