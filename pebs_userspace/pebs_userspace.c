#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

int syscall_pebs_start = 463;
int syscall_pebs_end = 464;



long pebs_start(pid_t pid, char *cgroup_path)
{
    return syscall(syscall_pebs_start, pid, cgroup_path);
}

long pebs_end(pid_t pid, char *cgroup_path)
{
    return syscall(syscall_pebs_end, pid, cgroup_path);
}


int main (int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s [start|end] <pid> <cgroup_path>\n", argv[0]);
        return 1;
    }
    pid_t pid = atoi(argv[2]);
    printf("pid: %d\n", pid);
    char *cgroup_path = argv[3];
    printf("cgroup_path: %s\n", cgroup_path);


    // if argument is start, call pebs_start
    if (argc == 4 && strcmp(argv[1], "start") == 0) {
        printf("pebs start\n");
        pebs_start(pid, cgroup_path);
    }
    // if argument is end, call pebs_end
    else if (argc == 4 && strcmp(argv[1], "end") == 0) {
        printf("pebs end\n");
        pebs_end(pid, cgroup_path);
    }
}