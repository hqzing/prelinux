#define _GNU_SOURCE 1
/* PreLinux acceptance test program: exercises every
   intercepted API family.  All host-observed paths must land under the
   instance root (or a pass-through prefix).  */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

extern char **environ;

static int ncalls;

#define TRY(expr) do { errno = 0; (void) (expr); ncalls++; } while (0)

int
main (void)
{
  struct stat st;
  struct statx stx;
  struct statvfs sv;
  char buf[PATH_MAX];

  /* open class */
  TRY (open ("/etc/hostname", O_RDONLY));
  TRY (open64 ("/etc/passwd", O_RDONLY));
  TRY (openat (AT_FDCWD, "/etc/group", O_RDONLY));
  TRY (fopen ("/etc/os-release", "r"));
  TRY (creat ("/tmp/prel-test-creat", 0644));

  /* metadata class */
  TRY (stat ("/etc/hostname", &st));
  TRY (lstat ("/etc/hostname", &st));
  TRY (fstatat (AT_FDCWD, "/etc/passwd", &st, 0));
  TRY (statx (AT_FDCWD, "/etc/group", 0, 0, &stx));
  TRY (access ("/etc/hostname", F_OK));
  TRY (faccessat (AT_FDCWD, "/etc/passwd", R_OK, 0));
  TRY (statvfs ("/var", &sv));

  /* directory class */
  TRY (mkdir ("/tmp/prel-test-dir", 0755));
  TRY (mkdirat (AT_FDCWD, "/tmp/prel-test-dir2", 0755));
  TRY (rmdir ("/tmp/prel-test-dir2"));
  TRY (opendir ("/etc"));
  TRY (chdir ("/tmp"));
  /* chroot translates the path but does not fake privileges (design):
     as root it would really confine us and break the rest of the test;
     exercise it only when unprivileged (expect EPERM).  */
  if (geteuid () != 0)
    TRY (chroot ("/tmp"));

  /* link class */
  TRY (symlink ("/etc/hostname", "/tmp/prel-test-lnk"));
  TRY (readlink ("/tmp/prel-test-lnk", buf, sizeof (buf)));
  TRY (link ("/etc/hostname", "/tmp/prel-test-hard"));
  TRY (unlink ("/tmp/prel-test-hard"));
  TRY (rename ("/tmp/prel-test-lnk", "/tmp/prel-test-lnk2"));

  /* attribute class */
  TRY (chmod ("/tmp/prel-test-creat", 0600));
  TRY (chown ("/tmp/prel-test-creat", 0, 0));
  TRY (utimensat (AT_FDCWD, "/tmp/prel-test-creat", NULL, 0));
  TRY (truncate ("/tmp/prel-test-creat", 0));
  TRY (mknod ("/tmp/prel-test-nod", S_IFIFO | 0644, 0));
  TRY (setxattr ("/tmp/prel-test-creat", "user.x", "y", 1, 0));

  /* realpath & friends */
  {
    char *rp = realpath ("/etc/../etc/hostname", buf);
    (void) rp;
  }

  /* exec class: /bin/true must exist in the instance */
  {
    pid_t pid = fork ();
    if (pid == 0)
      {
	char *a[] = { (char *) "/bin/true", NULL };
	execve ("/bin/true", a, environ);
	_exit (127);
      }
    int stt;
    waitpid (pid, &stt, 0);
    if (!WIFEXITED (stt) || WEXITSTATUS (stt) != 0)
      {
	fprintf (stderr, "pathtest: execve(/bin/true) failed\n");
	return 1;
      }
  }

  printf ("pathtest: %d API calls exercised\n", ncalls);
  return 0;
}
