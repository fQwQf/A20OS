#include "../lib/libc.h"

int main(int argc, char *argv[]) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) write(1, " ", 1);
        const char *s = argv[i];
        while (*s) {
            if (*s == '\\' && s[1]) {
                s++;
                switch (*s) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case 'e': putchar(27);   break;
                    case '\\': putchar('\\'); break;
                    default: putchar('\\'); putchar(*s); break;
                }
            } else {
                putchar(*s);
            }
            s++;
        }
    }
    if (newline) putchar('\n');
    return 0;
}
