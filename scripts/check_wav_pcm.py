#!/usr/bin/env python3
import array
import sys
import wave


def main() -> int:
    max_delta_limit = None
    min_frames = 1000
    args = sys.argv[1:]
    while len(args) >= 2 and args[0] in ("--max-delta", "--min-frames"):
        option = args.pop(0)
        try:
            value = int(args.pop(0))
        except ValueError:
            value = -1
        if value < 0:
            print(f"check_wav_pcm: invalid value for {option}",
                  file=sys.stderr)
            return 2
        if option == "--max-delta":
            max_delta_limit = value
        else:
            min_frames = value
    if len(args) != 1:
        print(
            f"usage: {sys.argv[0]} [--max-delta N] [--min-frames N] FILE.wav",
            file=sys.stderr,
        )
        return 2
    path = args[0]

    try:
        with wave.open(path, "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            frame_rate = wav_file.getframerate()
            frame_count = wav_file.getnframes()
            payload = wav_file.readframes(frame_count)
    except (OSError, EOFError, wave.Error) as error:
        print(f"check_wav_pcm: invalid WAV: {error}", file=sys.stderr)
        return 1

    if channels not in (1, 2) or sample_width != 2 or frame_count < min_frames:
        print(
            "check_wav_pcm: unexpected PCM layout "
            f"channels={channels} width={sample_width} frames={frame_count}",
            file=sys.stderr,
        )
        return 1

    samples = array.array("h")
    samples.frombytes(payload)
    if sys.byteorder != "little":
        samples.byteswap()
    peak = max((abs(sample) for sample in samples), default=0)
    active = sum(abs(sample) >= 100 for sample in samples)
    if peak < 1000 or active < 1000:
        print(
            f"check_wav_pcm: no audible PCM peak={peak} active={active}",
            file=sys.stderr,
        )
        return 1

    max_delta = 0
    for channel in range(channels):
        previous = samples[channel]
        for index in range(channel + channels, len(samples), channels):
            current = samples[index]
            delta = abs(current - previous)
            if delta > max_delta:
                max_delta = delta
            previous = current
    if max_delta_limit is not None and max_delta > max_delta_limit:
        print(
            "check_wav_pcm: discontinuous PCM "
            f"max_delta={max_delta} limit={max_delta_limit}",
            file=sys.stderr,
        )
        return 1

    print(
        "check_wav_pcm: PASS "
        f"rate={frame_rate} channels={channels} frames={frame_count} "
        f"peak={peak} active={active} max_delta={max_delta}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
