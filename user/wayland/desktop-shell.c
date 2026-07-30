#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "weston-desktop-shell-client-protocol.h"

struct desktop_state {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_output *output;
    struct weston_desktop_shell *shell;
};

static int create_buffer(struct desktop_state *state, int width, int height,
                         uint32_t color, struct wl_buffer **buffer_out)
{
    size_t size = (size_t)width * (size_t)height * 4;
    char path[] = "/tmp/a20-desktop-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
        return -1;
    unlink(path);
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        close(fd);
        return -1;
    }
    for (size_t i = 0; i < size / sizeof(*pixels); i++)
        pixels[i] = color;
    struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, (int32_t)size);
    close(fd);
    if (!pool)
        return -1;
    *buffer_out = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
                                            WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    return *buffer_out ? 0 : -1;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct desktop_state *state = data;
    if (!strcmp(interface, wl_compositor_interface.name))
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (!strcmp(interface, wl_shm_interface.name))
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, wl_output_interface.name))
        state->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    else if (!strcmp(interface, weston_desktop_shell_interface.name))
        state->shell = wl_registry_bind(registry, name,
                                        &weston_desktop_shell_interface,
                                        version > 1 ? 1 : version);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void shell_configure(void *data, struct weston_desktop_shell *shell,
                            uint32_t edges, struct wl_surface *surface,
                            int32_t width, int32_t height)
{
    (void)data;
    (void)shell;
    (void)edges;
    (void)surface;
    (void)width;
    (void)height;
}

static void shell_prepare_lock(void *data, struct weston_desktop_shell *shell)
{
    (void)data;
    weston_desktop_shell_unlock(shell);
}

static void shell_grab_cursor(void *data, struct weston_desktop_shell *shell,
                              uint32_t cursor)
{
    (void)data;
    (void)shell;
    (void)cursor;
}

static const struct weston_desktop_shell_listener shell_listener = {
    .configure = shell_configure,
    .prepare_lock_surface = shell_prepare_lock,
    .grab_cursor = shell_grab_cursor,
};

int main(void)
{
    struct desktop_state state = {0};
    state.display = wl_display_connect(NULL);
    if (!state.display)
        return 1;
    struct wl_registry *registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(registry, &registry_listener, &state);
    int roundtrip = wl_display_roundtrip(state.display);
    if (roundtrip < 0 || !state.compositor || !state.shm ||
        !state.output || !state.shell)
        return 1;
    weston_desktop_shell_add_listener(state.shell, &shell_listener, &state);

    struct wl_surface *background = wl_compositor_create_surface(state.compositor);
    struct wl_surface *panel = wl_compositor_create_surface(state.compositor);
    struct wl_buffer *background_buffer = NULL;
    struct wl_buffer *panel_buffer = NULL;
    if (!background || !panel ||
        create_buffer(&state, 1024, 768, 0xff202b3d, &background_buffer) < 0 ||
        create_buffer(&state, 1024, 32, 0xff101722, &panel_buffer) < 0)
    {
        return 1;
    }

    wl_surface_attach(background, background_buffer, 0, 0);
    wl_surface_damage(background, 0, 0, 1024, 768);
    wl_surface_attach(panel, panel_buffer, 0, 0);
    wl_surface_damage(panel, 0, 0, 1024, 32);
    weston_desktop_shell_set_background(state.shell, state.output, background);
    weston_desktop_shell_set_panel(state.shell, state.output, panel);
    weston_desktop_shell_set_panel_position(state.shell, 0);
    wl_surface_commit(background);
    wl_surface_commit(panel);
    weston_desktop_shell_desktop_ready(state.shell);
    if (wl_display_flush(state.display) < 0)
        return 1;
    while (wl_display_dispatch(state.display) >= 0)
        ;
    return 0;
}
