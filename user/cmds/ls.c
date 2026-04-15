/* ls — list directory contents */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static void print_size(unsigned long sz) {
    if (sz >= 1024*1024*1024) printf("%4lu G", sz/(1024*1024*1024));
    else if (sz >= 1024*1024) printf("%4lu M", sz/(1024*1024));
    else if (sz >= 1024)      printf("%4lu K", sz/1024);
    else                      printf("%4lu  ", sz);
}

static void ls_dir(const char *path, int show_all, int long_fmt, int human) {
    DIR *d = opendir(path);
    if (!d) { printf("ls: cannot open '%s'\n", path); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!show_all && ent->d_name[0] == '.') continue;
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
        if (npaths > 1) printf("%s:\n", paths[i]);
        ls_dir(paths[i], show_all, long_fmt, human);
        (void)recursive; /* TODO: recursive listing */
    }
    return 0;
}
