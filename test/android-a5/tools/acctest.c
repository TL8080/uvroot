#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void chk(const char *p)
{
    struct stat st;
    int r, fd, e;
    char rr[40], ww[40], xx[40], fa[40], op[40], so[40], ea[40], ew[40];

    errno = 0; r = access(p, R_OK); e = errno;
    snprintf(rr, sizeof rr, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; r = access(p, W_OK); e = errno;
    snprintf(ww, sizeof ww, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; r = access(p, X_OK); e = errno;
    snprintf(xx, sizeof xx, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; r = faccessat(AT_FDCWD, p, R_OK, 0); e = errno;
    snprintf(fa, sizeof fa, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; r = faccessat(AT_FDCWD, p, R_OK, AT_EACCESS); e = errno;
    snprintf(ea, sizeof ea, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; r = faccessat(AT_FDCWD, p, W_OK, AT_EACCESS); e = errno;
    snprintf(ew, sizeof ew, "%d:%s", r, r ? strerror(e) : "ok");
    errno = 0; fd = open(p, O_RDONLY); e = errno;
    if (fd >= 0) { close(fd); snprintf(op, sizeof op, "ok"); }
    else snprintf(op, sizeof op, "%d:%s", e, strerror(e));
    errno = 0; r = stat(p, &st); e = errno;
    if (r == 0) snprintf(so, sizeof so, "mode=%04o uid=%d", st.st_mode & 07777, st.st_uid);
    else snprintf(so, sizeof so, "stat-err=%s", strerror(e));

    printf("%-44s\n   access      R=%-22s W=%-22s X=%-22s\n   faccessat   R=%-22s (flags=0)\n   faccessat   R=%-22s W=%-22s (AT_EACCESS)  <-- what shell `test -r` uses\n   open=%-16s %s\n",
           p, rr, ww, xx, fa, ea, ew, op, so);
}

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
        chk(argv[i]);
    return 0;
}
