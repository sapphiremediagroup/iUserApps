float _Complex __mulsc3(float a, float b, float c, float d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0fi;
}

double _Complex __muldc3(double a, double b, double c, double d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0i;
}

long double _Complex __mulxc3(long double a, long double b, long double c, long double d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0Li;
}

/* mount-table enumeration (mntent.h).
 *
 * Reads a real mounts file (the kernel seeds /etc/mtab at boot with every VFS
 * mount). setmntent() opens the file; getmntent()/getmntent_r() parse one
 * whitespace-delimited line per entry (musl-derived). This lets `df` (no args)
 * and `mount` list the live mount table. */

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>

static char *__mntent_internal_buf;
static size_t __mntent_internal_bufsize;

#define MNTENT_SENTINEL ((char *)&__mntent_internal_buf)

FILE *setmntent(const char *name, const char *mode) {
  return fopen(name, mode);
}

struct mntent *getmntent_r(FILE *f, struct mntent *mnt, char *linebuf, int buflen) {
  int n[8];
  int use_internal = (linebuf == MNTENT_SENTINEL);
  int len;
  size_t i;

  mnt->mnt_freq = 0;
  mnt->mnt_passno = 0;

  do {
    if (use_internal) {
      if (getline(&__mntent_internal_buf, &__mntent_internal_bufsize, f) < 0) {
        return NULL;
      }
      linebuf = __mntent_internal_buf;
    } else {
      if (!fgets(linebuf, buflen, f)) {
        return NULL;
      }
    }
    if (feof(f) || ferror(f)) {
      return NULL;
    }
    if (!strchr(linebuf, '\n')) {
      fscanf(f, "%*[^\n]%*[\n]");
      errno = ERANGE;
      return NULL;
    }

    len = (int)strlen(linebuf);
    for (i = 0; i < sizeof n / sizeof *n; i++) {
      n[i] = len;
    }

    sscanf(linebuf, " %n%*s%n %n%*s%n %n%*s%n %n%*s%n %d %d",
           n, n + 1, n + 2, n + 3, n + 4, n + 5, n + 6, n + 7,
           &mnt->mnt_freq, &mnt->mnt_passno);
  } while (linebuf[n[0]] == '#' || n[1] == len);

  linebuf[n[1]] = 0;
  linebuf[n[3]] = 0;
  linebuf[n[5]] = 0;
  linebuf[n[7]] = 0;

  mnt->mnt_fsname = linebuf + n[0];
  mnt->mnt_dir = linebuf + n[2];
  mnt->mnt_type = linebuf + n[4];
  mnt->mnt_opts = linebuf + n[6];

  return mnt;
}

struct mntent *getmntent(FILE *f) {
  static struct mntent mnt;
  return getmntent_r(f, &mnt, MNTENT_SENTINEL, 0);
}

int addmntent(FILE *f, const struct mntent *mnt) {
  if (fseek(f, 0, SEEK_END)) {
    return 1;
  }
  return fprintf(f, "%s\t%s\t%s\t%s\t%d\t%d\n",
                 mnt->mnt_fsname, mnt->mnt_dir, mnt->mnt_type, mnt->mnt_opts,
                 mnt->mnt_freq, mnt->mnt_passno) < 0;
}

int endmntent(FILE *f) {
  if (f) {
    fclose(f);
  }
  return 1; /* per POSIX, endmntent always returns 1 */
}

char *hasmntopt(const struct mntent *mnt, const char *opt) {
  return strstr(mnt->mnt_opts, opt);
}

