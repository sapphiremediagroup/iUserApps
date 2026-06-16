#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>

#define MOUNTED "/etc/mtab"

#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO       "ro"
#define MNTOPT_RW       "rw"
#define MNTOPT_SUID     "suid"
#define MNTOPT_NOSUID   "nosuid"
#define MNTOPT_NOAUTO   "noauto"

#ifdef __cplusplus
extern "C" {
#endif

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

FILE *setmntent(const char *__filename, const char *__type);
struct mntent *getmntent(FILE *__f);
int addmntent(FILE *__f, const struct mntent *__mnt);
int endmntent(FILE *__f);
char *hasmntopt(const struct mntent *__mnt, const char *__opt);
struct mntent *getmntent_r(FILE *__f, struct mntent *__mnt, char *__linebuf, int __buflen);

#ifdef __cplusplus
}
#endif

#endif /* _MNTENT_H */
