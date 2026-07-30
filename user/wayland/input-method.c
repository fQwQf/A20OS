#include <wayland-client.h>

int main(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        return 1;
    while (wl_display_dispatch(display) >= 0)
        ;
    return 0;
}
