/*
 * musl A20 signal — minimal implementation (no async signal delivery).
 * Per startup.md §4.4.2: no async signal interrupts on A20.
 */
#include <stdint.h>
#include <errno.h>

#define _NSIG 64

static void (*signal_handlers[_NSIG])(int);

int __a20_sigaction(int sig, const void *act, void *oact)
{
    if (sig < 0 || sig >= _NSIG) return -EINVAL;
    if (oact) *(void **)oact = signal_handlers[sig];
    if (act) signal_handlers[sig] = *(void **)act;
    return 0;
}

int __a20_raise(int sig)
{
    if (sig < 0 || sig >= _NSIG || !signal_handlers[sig])
        return 0;
    signal_handlers[sig](sig);
    return 0;
}

int __a20_kill(int pid, int sig)
{
    (void)pid; (void)sig;
    return 0;
}
