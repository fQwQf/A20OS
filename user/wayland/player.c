#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <wayland-client.h>

#include <uapi/a20/audio.h>

#include "xdg-shell-client-protocol.h"

#define AUDIO_SCAN_LIMIT 32
#define AUDIO_QUEUE_LIMIT (48000U * 4U)
#define VIDEO_BUFFER_COUNT 3

struct video_buffer {
    struct wl_buffer *wl_buffer;
    uint8_t *data;
    int busy;
};

struct display_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_shm_pool *pool;
    struct video_buffer buffers[VIDEO_BUFFER_COUNT];
    int width;
    int height;
    int stride;
    int configured;
    int closed;
};

struct audio_chunk {
    struct audio_chunk *next;
    size_t size;
    uint8_t data[];
};

struct audio_state {
    int fd;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t space;
    struct audio_chunk *head;
    struct audio_chunk *tail;
    size_t queued;
    int stopping;
    int failed;
};

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void print_av_error(const char *operation, int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, text, sizeof(text));
    fprintf(stderr, "a20-player: %s: %s\n", operation, text);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ((struct video_buffer *)data)->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
                         uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    struct display_state *state = data;
    xdg_surface_ack_configure(surface, serial);
    state->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    ((struct display_state *)data)->closed = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    struct display_state *state = data;
    (void)version;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        state->compositor = wl_registry_bind(registry, name,
                                              &wl_compositor_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        state->wm_base = wl_registry_bind(registry, name,
                                           &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->wm_base, &wm_base_listener, state);
    }
}

static void registry_remove(void *data, struct wl_registry *registry,
                            uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static int create_shm_file(size_t size)
{
    char path[] = "/tmp/a20-player-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int display_open(struct display_state *state, int width, int height)
{
    memset(state, 0, sizeof(*state));
    state->width = width;
    state->height = height;
    state->stride = width * 4;
    for (int attempt = 0; attempt < 100 && !state->display; attempt++) {
        state->display = wl_display_connect(NULL);
        if (!state->display)
            sched_yield();
    }
    if (!state->display)
        return -1;
    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    if (wl_display_roundtrip(state->display) < 0 || !state->compositor ||
        !state->shm || !state->wm_base)
        return -1;

    state->surface = wl_compositor_create_surface(state->compositor);
    state->xdg_surface = xdg_wm_base_get_xdg_surface(state->wm_base,
                                                     state->surface);
    xdg_surface_add_listener(state->xdg_surface, &xdg_surface_listener, state);
    state->toplevel = xdg_surface_get_toplevel(state->xdg_surface);
    xdg_toplevel_add_listener(state->toplevel, &toplevel_listener, state);
    xdg_toplevel_set_title(state->toplevel, "A20OS Media Player");
    wl_surface_commit(state->surface);
    while (!state->configured && wl_display_dispatch(state->display) >= 0)
        ;
    if (!state->configured)
        return -1;

    size_t frame_size = (size_t)state->stride * (size_t)height;
    size_t pool_size = frame_size * VIDEO_BUFFER_COUNT;
    int fd = create_shm_file(pool_size);
    if (fd < 0)
        return -1;
    uint8_t *mapping = mmap(NULL, pool_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return -1;
    }
    state->pool = wl_shm_create_pool(state->shm, fd, (int32_t)pool_size);
    close(fd);
    for (int i = 0; i < VIDEO_BUFFER_COUNT; i++) {
        state->buffers[i].data = mapping + frame_size * (size_t)i;
        state->buffers[i].wl_buffer = wl_shm_pool_create_buffer(
            state->pool, (int32_t)(frame_size * (size_t)i), width, height,
            state->stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(state->buffers[i].wl_buffer, &buffer_listener,
                               &state->buffers[i]);
    }
    return 0;
}

static int display_pump(struct display_state *state, int timeout_ms)
{
    if (wl_display_dispatch_pending(state->display) < 0)
        return -1;
    wl_display_flush(state->display);
    struct pollfd pfd = {
        .fd = wl_display_get_fd(state->display),
        .events = POLLIN,
    };
    int result;
    do {
        result = poll(&pfd, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result > 0 && (pfd.revents & POLLIN) &&
        wl_display_dispatch(state->display) < 0)
        return -1;
    return state->closed ? -1 : 0;
}

static struct video_buffer *display_acquire(struct display_state *state)
{
    for (;;) {
        for (int i = 0; i < VIDEO_BUFFER_COUNT; i++) {
            if (!state->buffers[i].busy)
                return &state->buffers[i];
        }
        if (display_pump(state, 20) < 0)
            return NULL;
    }
}

static int display_frame(struct display_state *state, struct SwsContext *sws,
                         const AVFrame *frame)
{
    struct video_buffer *buffer = display_acquire(state);
    if (!buffer)
        return -1;
    uint8_t *planes[4] = { buffer->data, NULL, NULL, NULL };
    int strides[4] = { state->stride, 0, 0, 0 };
    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, planes, strides);
    buffer->busy = 1;
    wl_surface_attach(state->surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage(state->surface, 0, 0, state->width, state->height);
    wl_surface_commit(state->surface);
    return display_pump(state, 0);
}

static int open_audio_device(void)
{
    for (int i = 0; i < AUDIO_SCAN_LIMIT; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/audio%d", i);
        int fd = open(path, O_WRONLY);
        if (fd < 0)
            continue;
        a20_audio_caps_t caps = {0};
        if (ioctl(fd, A20_AUDIO_IOCTL_GET_CAPS, &caps) == 0 &&
            (caps.flags & A20_AUDIO_CAP_PCM)) {
            a20_audio_format_t format = {
                .rate = 48000,
                .channels = 2,
                .format = A20_AUDIO_FORMAT_S16_LE,
            };
            if (ioctl(fd, A20_AUDIO_IOCTL_SET_FORMAT, &format) == 0)
                return fd;
        }
        close(fd);
    }
    return -1;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size) {
        ssize_t count = write(fd, data, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        data += count;
        size -= (size_t)count;
    }
    return 0;
}

static void *audio_worker(void *opaque)
{
    struct audio_state *audio = opaque;
    for (;;) {
        pthread_mutex_lock(&audio->lock);
        while (!audio->head && !audio->stopping)
            pthread_cond_wait(&audio->ready, &audio->lock);
        if (!audio->head && audio->stopping) {
            pthread_mutex_unlock(&audio->lock);
            break;
        }
        struct audio_chunk *chunk = audio->head;
        audio->head = chunk->next;
        if (!audio->head)
            audio->tail = NULL;
        pthread_mutex_unlock(&audio->lock);

        if (write_all(audio->fd, chunk->data, chunk->size) < 0)
            audio->failed = 1;
        pthread_mutex_lock(&audio->lock);
        audio->queued -= chunk->size;
        pthread_cond_broadcast(&audio->space);
        pthread_mutex_unlock(&audio->lock);
        free(chunk);
        if (audio->failed)
            break;
    }
    return NULL;
}

static int audio_start(struct audio_state *audio)
{
    memset(audio, 0, sizeof(*audio));
    audio->fd = open_audio_device();
    if (audio->fd < 0)
        return -1;
    pthread_mutex_init(&audio->lock, NULL);
    pthread_cond_init(&audio->ready, NULL);
    pthread_cond_init(&audio->space, NULL);
    if (pthread_create(&audio->thread, NULL, audio_worker, audio) != 0) {
        close(audio->fd);
        audio->fd = -1;
        return -1;
    }
    return 0;
}

static int audio_enqueue(struct audio_state *audio, const uint8_t *data,
                          size_t size)
{
    struct audio_chunk *chunk = malloc(sizeof(*chunk) + size);
    if (!chunk)
        return -1;
    chunk->next = NULL;
    chunk->size = size;
    memcpy(chunk->data, data, size);
    pthread_mutex_lock(&audio->lock);
    while (audio->queued + size > AUDIO_QUEUE_LIMIT && !audio->failed)
        pthread_cond_wait(&audio->space, &audio->lock);
    if (audio->failed) {
        pthread_mutex_unlock(&audio->lock);
        free(chunk);
        return -1;
    }
    if (audio->tail)
        audio->tail->next = chunk;
    else
        audio->head = chunk;
    audio->tail = chunk;
    audio->queued += size;
    pthread_cond_signal(&audio->ready);
    pthread_mutex_unlock(&audio->lock);
    return 0;
}

static void audio_stop(struct audio_state *audio)
{
    if (audio->fd < 0)
        return;
    pthread_mutex_lock(&audio->lock);
    audio->stopping = 1;
    pthread_cond_broadcast(&audio->ready);
    pthread_mutex_unlock(&audio->lock);
    pthread_join(audio->thread, NULL);
    ioctl(audio->fd, A20_AUDIO_IOCTL_STOP, NULL);
    close(audio->fd);
}

static AVCodecContext *open_decoder(AVFormatContext *format, int stream_index)
{
    AVStream *stream = format->streams[stream_index];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return NULL;
    AVCodecContext *context = avcodec_alloc_context3(codec);
    if (!context || avcodec_parameters_to_context(context, stream->codecpar) < 0 ||
        avcodec_open2(context, codec, NULL) < 0) {
        avcodec_free_context(&context);
        return NULL;
    }
    return context;
}

static double frame_time(const AVFrame *frame, const AVStream *stream)
{
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE)
        return NAN;
    return (double)pts * av_q2d(stream->time_base);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE.mp4\n", argv[0]);
        return 2;
    }

    AVFormatContext *format = NULL;
    int error = avformat_open_input(&format, argv[1], NULL, NULL);
    if (error < 0) {
        print_av_error("cannot open input", error);
        return 1;
    }
    if ((error = avformat_find_stream_info(format, NULL)) < 0) {
        print_av_error("cannot read stream information", error);
        return 1;
    }
    int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO,
                                          -1, -1, NULL, 0);
    int audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO,
                                          -1, -1, NULL, 0);
    if (video_index < 0) {
        fprintf(stderr, "a20-player: input has no video stream\n");
        return 1;
    }
    AVCodecContext *video = open_decoder(format, video_index);
    AVCodecContext *audio = audio_index >= 0 ? open_decoder(format, audio_index)
                                             : NULL;
    if (!video || (audio_index >= 0 && !audio)) {
        fprintf(stderr, "a20-player: required decoder is unavailable\n");
        return 1;
    }

    struct display_state display;
    if (display_open(&display, video->width, video->height) < 0) {
        fprintf(stderr, "a20-player: cannot connect to Wayland compositor: %s\n",
                strerror(errno));
        return 1;
    }
    struct SwsContext *sws = sws_getContext(
        video->width, video->height, video->pix_fmt,
        video->width, video->height, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws)
        return 1;

    struct audio_state output;
    output.fd = -1;
    struct SwrContext *swr = NULL;
    if (audio && audio_start(&output) == 0) {
        AVChannelLayout stereo;
        av_channel_layout_default(&stereo, 2);
        error = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
                                    &audio->ch_layout, audio->sample_fmt,
                                    audio->sample_rate, 0, NULL);
        av_channel_layout_uninit(&stereo);
        if (error < 0 || swr_init(swr) < 0) {
            fprintf(stderr, "a20-player: cannot initialize audio converter\n");
            audio_stop(&output);
            output.fd = -1;
            swr_free(&swr);
        }
    } else if (audio) {
        fprintf(stderr, "a20-player: no PCM output; playing video without audio\n");
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    double media_origin = NAN;
    double clock_origin = 0.0;
    int failed = !packet || !frame;
    while (!failed && !display.closed && av_read_frame(format, packet) >= 0) {
        AVCodecContext *decoder = NULL;
        if (packet->stream_index == video_index)
            decoder = video;
        else if (packet->stream_index == audio_index && swr)
            decoder = audio;
        if (!decoder) {
            av_packet_unref(packet);
            continue;
        }
        error = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (error < 0 && error != AVERROR(EAGAIN)) {
            failed = 1;
            break;
        }
        while ((error = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (decoder == video) {
                double pts = frame_time(frame, format->streams[video_index]);
                if (!isnan(pts)) {
                    if (isnan(media_origin)) {
                        media_origin = pts;
                        clock_origin = monotonic_seconds();
                    }
                    double wait = clock_origin + (pts - media_origin) -
                                  monotonic_seconds();
                    while (wait > 0.0 && !display.closed) {
                        int ms = wait > 0.02 ? 20 : (int)(wait * 1000.0);
                        if (display_pump(&display, ms > 0 ? ms : 1) < 0)
                            break;
                        wait = clock_origin + (pts - media_origin) -
                               monotonic_seconds();
                    }
                }
                if (!display.closed && display_frame(&display, sws, frame) < 0)
                    failed = 1;
            } else {
                int out_samples = (int)av_rescale_rnd(
                    swr_get_delay(swr, audio->sample_rate) + frame->nb_samples,
                    48000, audio->sample_rate, AV_ROUND_UP);
                uint8_t *converted = NULL;
                int line_size = 0;
                if (av_samples_alloc(&converted, &line_size, 2, out_samples,
                                     AV_SAMPLE_FMT_S16, 0) < 0) {
                    failed = 1;
                } else {
                    int samples = swr_convert(swr, &converted, out_samples,
                                              (const uint8_t **)frame->extended_data,
                                              frame->nb_samples);
                    if (samples < 0 || audio_enqueue(&output, converted,
                                                     (size_t)samples * 4U) < 0)
                        failed = 1;
                    av_freep(&converted);
                }
            }
            av_frame_unref(frame);
            if (failed)
                break;
        }
        if (error != AVERROR(EAGAIN) && error != AVERROR_EOF && error < 0)
            failed = 1;
        display_pump(&display, 0);
    }

    audio_stop(&output);
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    sws_freeContext(sws);
    avcodec_free_context(&audio);
    avcodec_free_context(&video);
    avformat_close_input(&format);
    return failed ? 1 : 0;
}
