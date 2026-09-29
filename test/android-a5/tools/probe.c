#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>

int main(void)
{
    const char *p = "/tmp/probe.txt";
    struct stat st;
    int fd;
    pid_t pid;

    printf("hello: pid=%d uid=%d gid=%d\n", (int)getpid(), (int)getuid(), (int)getgid());
    fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (write(fd, "abc\n", 4) != 4) { perror("write"); return 1; }
    close(fd);
    if (stat(p, &st) != 0) { perror("stat"); return 1; }
    printf("stat: size=%lld mode=%04o\n", (long long)st.st_size, st.st_mode & 07777);
    pid = fork();
    if (pid == 0) { execl("/bin/echo", "echo", "child-exec-ok", (char *)NULL); _exit(127); }
    if (pid < 0) { perror("fork"); return 1; }
    waitpid(pid, NULL, 0);
    printf("syscall-probe: PASS\n");
    return 0;
}
