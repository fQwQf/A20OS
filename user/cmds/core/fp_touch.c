#include <stdint.h>
#include <stdio.h>

/* Minimal "does user-mode VFP work at all" probe.  The arm32 userland is built
 * hard-float, so ordinary double arithmetic and passing a double to printf both
 * execute VFP instructions in EL0. */
static double fp_add(double a, double b)
{
    return a + b;
}

int main(void)
{
    volatile double x = 1.5;
    volatile double y = 2.25;
    double z = fp_add(x, y);
    if (z != 3.75) {
        printf("FP_TOUCH: FAIL z=%d\n", (int)(z * 100));
        return 1;
    }
    printf("FP_TOUCH: PASS\n");
    return 0;
}
