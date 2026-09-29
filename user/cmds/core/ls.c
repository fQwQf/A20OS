/* ls — list directory contents */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* d_name is bounded by NAME_MAX (255); the buffer only has to add the NUL. */
#define NAME_BUF 256

/* Subdirectories noticed while a directory is being listed, kept for a -R
 * walk to descend into afterwards.  Grown on demand so a wide directory is
 * never silently truncated. */
typedef struct {
    char name[NAME_BUF];
} subdir_t;

typedef struct {
    subdir_t *item;
    int count;
    int size;
} subdir_list_t;

static void print_size(unsigned long sz) {
    if (sz >= 1024*1024*1024) printf("%4lu G", sz/(1024*1024*1024));
    else if (sz >= 1024*1024) printf("%4lu M", sz/(1024*1024));
    else if (sz >= 1024)      printf("%4lu K", sz/1024);
    else                      printf("%4lu  ", sz);
}

static void subdir_list_add(subdir_list_t *list, const char *name) {
    if (list->count == list->size) {
        int size = list->size ? list->size * 2 : 32;
        subdir_t *item = realloc(list->item, (size_t)size * sizeof(*item));
        if (!item) return;  /* leave this one out rather than abandon the walk */
        list->item = item;
        list->size = size;
    }
    snprintf(list->item[list->count].name, NAME_BUF, "%s", name);
    list->count++;
}

/* Note a real subdirectory for a later -R descent.  lstat, not stat: a
 * symlink to a directory is listed like any other entry but never followed,
 * so a link pointing back at an ancestor cannot make the walk loop forever.
 * "." and ".." are the walk's own bookkeeping, never a place to descend to. */
static void collect_subdir(subdir_list_t *list, const char *dir, const char *name) {
    char full[512];
    struct stat st;

    if (name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return;
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (lstat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    subdir_list_add(list, name);
}

/* List one directory.  subs is NULL for a plain listing -- the same call the
 * non-recursive path has always made -- and receives the subdirectories when a
 * -R walk is in progress, collected on the readdir pass that prints the
 * entries so no directory is opened twice. */
static void ls_dir(const char *path, int show_all, int long_fmt, int human,
                   subdir_list_t *subs) {
    DIR *d = opendir(path);
    if (!d) { printf("ls: cannot open '%s'\n", path); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!show_all && ent->d_name[0] == '.') continue;
        if (subs) collect_subdir(subs, path, ent->d_name);
        if (long_fmt) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            struct stat st; stat(full, &st);
            char type = S_ISDIR(st.st_mode) ? 'd' : (S_ISLNK(st.st_mode) ? 'l' : '-');
            char r = (st.st_mode & S_IRUSR) ? 'r' : '-';
            char w = (st.st_mode & S_IWUSR) ? 'w' : '-';
            char x = (st.st_mode & S_IXUSR) ? 'x' : '-';
            if (human) {
                printf("%c%c%c%c %6u ", type, r, w, x, st.st_nlink);
                print_size(st.st_size);
                printf("  ");
            } else {
                printf("%c%c%c%c %6u %8lu ", type, r, w, x, st.st_nlink, st.st_size);
            }
            if (ent->d_type == DT_DIR)
                printf("\033[1;34m%s\033[0m\n", ent->d_name);
            else if (ent->d_type == DT_LNK)
                printf("\033[1;36m%s\033[0m\n", ent->d_name);
            else if (st.st_mode & S_IXUSR)
                printf("\033[1;32m%s\033[0m\n", ent->d_name);
            else
                printf("%s\n", ent->d_name);
        } else {
            if (ent->d_type == DT_DIR)      printf("\033[1;34m%s\033[0m  ", ent->d_name);
            else if (ent->d_type == DT_LNK) printf("\033[1;36m%s\033[0m  ", ent->d_name);
            else                             printf("%s  ", ent->d_name);
        }
    }
    if (!long_fmt) putchar('\n');
    closedir(d);
}

static void ls_recurse(const char *path, int show_all, int long_fmt, int human,
                       int separated) {
    subdir_list_t subs = { NULL, 0, 0 };

    /* The POSIX/BSD shape: a "path:" header, then that directory's entries,
     * then the same for every subdirectory, depth first.  The blocks are
     * separated by a blank line, except the first, which starts at the prompt. */
    if (separated) putchar('\n');
    printf("%s:\n", path);
    ls_dir(path, show_all, long_fmt, human, &subs);
    for (int i = 0; i < subs.count; i++) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, subs.item[i].name);
        ls_recurse(full, show_all, long_fmt, human, 1);
    }
    free(subs.item);
}

int main(int argc, char *argv[]) {
    int show_all = 0, long_fmt = 0, human = 0, recursive = 0;
    char *paths[64]; int npaths = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'a': show_all = 1; break;
                    case 'l': long_fmt = 1; break;
                    case 'h': human = 1; break;
                    case 'R': recursive = 1; break;
                }
            }
        } else {
            if (npaths < 64) paths[npaths++] = argv[i];
        }
    }
    if (npaths == 0) { paths[0] = "."; npaths = 1; }
    for (int i = 0; i < npaths; i++) {
        if (recursive) {
            /* -R heads every directory it walks, the ones named on the command
             * line included, so the header comes from the walk itself. */
            ls_recurse(paths[i], show_all, long_fmt, human, 0);
        } else {
            if (npaths > 1) printf("%s:\n", paths[i]);
            ls_dir(paths[i], show_all, long_fmt, human, NULL);
        }
    }
    return 0;
}
