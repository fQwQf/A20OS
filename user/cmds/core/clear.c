#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    write(1, "\033[2J\033[H", 7);
    return 0;
}
