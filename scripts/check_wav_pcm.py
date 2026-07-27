#!/usr/bin/env python3
import array
import sys
import wave


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} FILE.wav", file=sys.stderr)
        return 2

    try:
        with wave.open(sys.argv[1], "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            frame_rate = wav_file.getframerate()
            frame_count = wav_file.getnframes()
            payload = wav_file.readframes(frame_count)
    except (OSError, EOFError, wave.Error) as error:
        print(f"check_wav_pcm: invalid WAV: {error}", file=sys.stderr)
        return 1

    if channels not in (1, 2) or sample_width != 2 or frame_count < 1000:
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

    print(
        "check_wav_pcm: PASS "
        f"rate={frame_rate} channels={channels} frames={frame_count} "
        f"peak={peak} active={active}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
