static int __errno_val;

int *___errno_location(void)
{
    return &__errno_val;
}
