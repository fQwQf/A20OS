#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/a20/audio.h>

#define AUDIO_SCAN_LIMIT 32
#define PCM_BUFFER_SIZE  32768U

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || parsed > UINT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int read_exact(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = buffer;
    while (size) {
        ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}

static int skip_bytes(int fd, uint32_t size)
{
    if (lseek(fd, (off_t)size, SEEK_CUR) >= 0)
        return 0;
    uint8_t discard[256];
    while (size) {
        size_t chunk = size < sizeof(discard) ? size : sizeof(discard);
        if (read_exact(fd, discard, chunk) < 0)
            return -1;
        size -= (uint32_t)chunk;
    }
    errno = 0;
    return 0;
}

static int open_pcm_device(const char *requested, char *selected,
                           size_t selected_size)
{
    int last = requested ? 1 : AUDIO_SCAN_LIMIT;
    for (int i = 0; i < last; i++) {
        char path[32];
        if (requested) {
            snprintf(path, sizeof(path), "%s", requested);
        } else {
            snprintf(path, sizeof(path), "/dev/audio%d", i);
        }
        int fd = open(path, O_WRONLY);
        if (fd < 0)
            continue;
        a20_audio_caps_t caps;
        memset(&caps, 0, sizeof(caps));
        if (ioctl(fd, A20_AUDIO_IOCTL_GET_CAPS, &caps) == 0 &&
            (caps.flags & A20_AUDIO_CAP_PCM)) {
            snprintf(selected, selected_size, "%s", path);
            return fd;
        }
        close(fd);
        if (requested)
            break;
    }
    return -1;
}

static int write_pcm(int audio_fd, const uint8_t *buffer, size_t size)
{
    while (size) {
        ssize_t count = write(audio_fd, buffer, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        buffer += count;
        size -= (size_t)count;
    }
    return 0;
}

static int play_stream(int input_fd, int audio_fd, uint32_t bytes,
                       int bounded)
{
    uint8_t buffer[PCM_BUFFER_SIZE];
    uint32_t frame_remainder = 0;
    while (!bounded || bytes) {
        size_t wanted = sizeof(buffer);
        if (bounded && bytes < wanted)
            wanted = bytes;
        ssize_t count = read(input_fd, buffer, wanted);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return -1;
        if (count == 0)
            break;
        if (write_pcm(audio_fd, buffer, (size_t)count) < 0)
            return -1;
        frame_remainder = (frame_remainder + (uint32_t)count) & 3U;
        if (bounded)
            bytes -= (uint32_t)count;
    }
    if ((bounded && bytes) || frame_remainder) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int play_wav(int input_fd, int audio_fd)
{
    uint8_t header[12];
    if (read_exact(input_fd, header, sizeof(header)) < 0 ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) {
        fprintf(stderr, "audioplay: not a RIFF/WAVE file\n");
        return -1;
    }

    int format_seen = 0;
    for (;;) {
        uint8_t chunk_header[8];
        if (read_exact(input_fd, chunk_header, sizeof(chunk_header)) < 0)
            break;
        uint32_t chunk_size = read_le32(chunk_header + 4);
        if (!memcmp(chunk_header, "fmt ", 4)) {
            uint8_t format[16];
            if (chunk_size < sizeof(format) ||
                read_exact(input_fd, format, sizeof(format)) < 0 ||
                skip_bytes(input_fd, chunk_size - sizeof(format)) < 0)
                break;
            if (read_le16(format) != 1 || read_le16(format + 2) != 2 ||
                read_le32(format + 4) != 48000U ||
                read_le16(format + 12) != 4 ||
                read_le16(format + 14) != 16) {
                fprintf(stderr,
                        "audioplay: WAV must be PCM 48000 Hz stereo S16_LE\n");
                return -1;
            }
            format_seen = 1;
        } else if (!memcmp(chunk_header, "data", 4)) {
            if (!format_seen) {
                fprintf(stderr, "audioplay: WAV data precedes format\n");
                return -1;
            }
            if (chunk_size & 3U) {
                fprintf(stderr, "audioplay: WAV data is not frame aligned\n");
                return -1;
            }
            return play_stream(input_fd, audio_fd, chunk_size, 1);
        } else if (skip_bytes(input_fd, chunk_size) < 0) {
            break;
        }
        if ((chunk_size & 1U) && skip_bytes(input_fd, 1) < 0)
            break;
    }
    fprintf(stderr, "audioplay: incomplete WAV file\n");
    return -1;
}

static int play_tone(int audio_fd, uint32_t frequency, uint32_t duration_ms)
{
    if (frequency < 20U || frequency > 20000U ||
        !duration_ms || duration_ms > 10000U) {
        errno = EINVAL;
        return -1;
    }
    uint8_t buffer[4096];
    uint64_t frames_left = (uint64_t)duration_ms * 48000U / 1000U;
    uint32_t phase = 0;
    while (frames_left) {
        size_t frames = frames_left < sizeof(buffer) / 4U ?
                        (size_t)frames_left : sizeof(buffer) / 4U;
        for (size_t i = 0; i < frames; i++) {
            int32_t sample;
            if (phase < 24000U)
                sample = -6000 + (int32_t)(phase * 12000U / 24000U);
            else
                sample = 6000 - (int32_t)((phase - 24000U) * 12000U /
                                          24000U);
            uint16_t encoded = (uint16_t)(int16_t)sample;
            buffer[i * 4U] = (uint8_t)encoded;
            buffer[i * 4U + 1U] = (uint8_t)(encoded >> 8);
            buffer[i * 4U + 2U] = (uint8_t)encoded;
            buffer[i * 4U + 3U] = (uint8_t)(encoded >> 8);
            phase += frequency;
            if (phase >= 48000U)
                phase -= 48000U;
        }
        if (write_pcm(audio_fd, buffer, frames * 4U) < 0)
            return -1;
        frames_left -= frames;
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [-r] [-d /dev/audioN] FILE\n", program);
    fprintf(stderr, "       %s [-d /dev/audioN] --tone HZ [--duration MS]\n",
            program);
    fprintf(stderr, "  -r  input is raw 48000 Hz stereo S16_LE PCM\n");
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    const char *input_path = NULL;
    uint32_t tone_frequency = 0;
    uint32_t tone_duration = 1000U;
    int duration_set = 0;
    int raw = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--raw")) {
            raw = 1;
        } else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            device = argv[i];
        } else if (!strcmp(argv[i], "--tone")) {
            if (++i >= argc || parse_u32(argv[i], &tone_frequency) < 0) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(argv[i], "--duration")) {
            if (++i >= argc || parse_u32(argv[i], &tone_duration) < 0) {
                usage(argv[0]);
                return 2;
            }
            duration_set = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!input_path) {
            input_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if ((!input_path && !tone_frequency) || (input_path && tone_frequency) ||
        (tone_frequency && raw) || (duration_set && !tone_frequency)) {
        usage(argv[0]);
        return 2;
    }

    int input_fd = -1;
    if (input_path)
        input_fd = open(input_path, O_RDONLY);
    if (input_path && input_fd < 0) {
        fprintf(stderr, "audioplay: cannot open %s: %s\n",
                input_path, strerror(errno));
        return 1;
    }
    char selected[32];
    int audio_fd = open_pcm_device(device, selected, sizeof(selected));
    if (audio_fd < 0) {
        fprintf(stderr, "audioplay: no PCM audio device found\n");
        if (input_fd >= 0)
            close(input_fd);
        return 1;
    }
    a20_audio_format_t format = {
        .rate = 48000U,
        .channels = 2,
        .format = A20_AUDIO_FORMAT_S16_LE,
    };
    if (ioctl(audio_fd, A20_AUDIO_IOCTL_SET_FORMAT, &format) < 0) {
        fprintf(stderr, "audioplay: %s rejected PCM format\n", selected);
        close(audio_fd);
        if (input_fd >= 0)
            close(input_fd);
        return 1;
    }

    if (tone_frequency)
        printf("audioplay: %u Hz for %u ms -> %s\n",
               tone_frequency, tone_duration, selected);
    else
        printf("audioplay: %s -> %s\n", input_path, selected);
    errno = 0;
    int result = tone_frequency ?
                 play_tone(audio_fd, tone_frequency, tone_duration) :
                 (raw ? play_stream(input_fd, audio_fd, 0, 0) :
                        play_wav(input_fd, audio_fd));
    if (result < 0) {
        int saved_errno = errno;
        ioctl(audio_fd, A20_AUDIO_IOCTL_STOP, NULL);
        if (saved_errno)
            fprintf(stderr, "audioplay: playback failed: %s\n",
                    strerror(saved_errno));
    }
    if (close(audio_fd) < 0 && result == 0) {
        fprintf(stderr, "audioplay: drain on close failed: %s\n",
                strerror(errno));
        result = -1;
    }
    if (result == 0)
        printf("audioplay: playback complete\n");
    if (input_fd >= 0)
        close(input_fd);
    return result < 0 ? 1 : 0;
}
