/*
 * a5run - drop privileges from root to the Termux app uid *with* the
 * supplementary groups a normal Android app has (notably AID_INET=3003),
 * then exec a command.  Needed because `su 10117` from adb only yields
 * gid 0 / group 0, which breaks Android's per-UID DNS resolution.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TERMUX_UID 10117
#define PREFIX "/data/data/com.termux/files/usr"
#define HOME_DIR "/data/data/com.termux/files/home"

int main(int argc, char **argv)
{
    uid_t uid = TERMUX_UID;
    gid_t gid = TERMUX_UID;
    int i = 1;

    if (argc > 2 && strcmp(argv[1], "--uid") == 0) {
        uid = (uid_t)strtoul(argv[2], NULL, 10);
        gid = (gid_t)uid;
        i = 3;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: a5run [--uid N] cmd [args...]\n");
        return 2;
    }

    /* Supplementary groups of a normal Android app in user 0:
     * 3003 AID_INET, 9997 AID_EVERYBODY, 20117 ext_data_rw, 50117. */
    gid_t groups[] = { 3003, 9997, (gid_t)(uid + 10000), (gid_t)(uid + 40000) };
    if (setgroups(sizeof(groups) / sizeof(groups[0]), groups) != 0) {
        perror("setgroups");
        return 1;
    }
    if (setgid(gid) != 0) {
        perror("setgid");
        return 1;
    }
    if (setuid(uid) != 0) {
        perror("setuid");
        return 1;
    }
    if (getuid() != uid || geteuid() != uid) {
        fprintf(stderr, "a5run: privilege drop failed\n");
        return 1;
    }

    setenv("PREFIX", PREFIX, 1);
    setenv("HOME", HOME_DIR, 1);
    setenv("PATH", PREFIX "/bin:" PREFIX "/bin/applets", 1);
    setenv("LD_LIBRARY_PATH", PREFIX "/lib", 1);
    setenv("TMPDIR", PREFIX "/tmp", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "en_US.UTF-8", 1);
    setenv("SHELL", PREFIX "/bin/bash", 1);
    if (chdir(HOME_DIR) != 0)
        chdir("/");

    execvp(argv[i], &argv[i]);
    fprintf(stderr, "a5run: execvp %s: %s\n", argv[i], strerror(errno));
    return 127;
}
