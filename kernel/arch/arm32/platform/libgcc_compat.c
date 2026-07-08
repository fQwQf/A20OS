#ifdef CONFIG_ARM32

int raise(int sig) {
    (void)sig;
    return 0;
}

#endif
