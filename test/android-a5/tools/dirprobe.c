#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>

int main(void)
{
    int fd, fd2, r;

    system("rm -rf /tmp/dp; mkdir -p /tmp/dp");

    /* plain relative mkdir */
    errno = 0; r = mkdir("dp/a", 0755);
    printf("mkdir(dp/a)              = %d %s\n", r, r ? strerror(errno) : "ok");

    /* mkdirat with O_DIRECTORY fd, relative name */
    fd = open("/tmp/dp", O_RDONLY | O_DIRECTORY);
    printf("open(/tmp/dp, O_DIRECTORY) = %d %s\n", fd, fd < 0 ? strerror(errno) : "ok");
    errno = 0; r = mkdirat(fd, "b", 0755);
    printf("mkdirat(fd,\"b\")           = %d %s\n", r, r ? strerror(errno) : "ok");

    /* second level via openat + mkdirat (what tar does) */
    fd2 = openat(fd, "b", O_RDONLY | O_DIRECTORY);
    printf("openat(fd,\"b\")            = %d %s\n", fd2, fd2 < 0 ? strerror(errno) : "ok");
    errno = 0; r = mkdirat(fd2, "c", 0755);
    printf("mkdirat(fd2,\"c\")          = %d %s\n", r, r ? strerror(errno) : "ok");

    /* symlinkat with dirfd, dangling relative target */
    errno = 0; r = symlinkat("nowhere", fd2, "lnk");
    printf("symlinkat(fd2,\"lnk\")      = %d %s\n", r, r ? strerror(errno) : "ok");

    /* create/open a file through openat + fd */
    errno = 0; int f3 = openat(fd2, "f", O_CREAT | O_WRONLY, 0644);
    printf("openat(fd2,\"f\",O_CREAT)   = %d %s\n", f3, f3 < 0 ? strerror(errno) : "ok");
    if (f3 >= 0) close(f3);

    system("find /tmp/dp | sort");
    return 0;
}
