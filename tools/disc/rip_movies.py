#!/usr/bin/env python3
"""Rip the game's movies (a raw 2352-byte CD-XA stream file) into one file per movie.

    tools/disc/rip_movies.py [--serial SLPS-03101] [--input FILE.raw2352] [--no-mpg]

Writes, per segment of config/<serial>/movies.json:
    assets/<serial>/movie_src/movie<N>.mp4          H.264 + AAC at the true frame rate (for upscaling)
    assets/converted/<serial>/movie/movie<N>.mpg    MPEG-1 + MP2, 30 fps (what the native player loads)

The input defaults to assets/<serial>/disc/<file>.raw2352 (an override, e.g. the US movie) or else
extracted/<serial>/fs/<file>.raw2352.

The frame rate is measured, not assumed: ffmpeg's psxstr reader always reports 15 fps, but a movie
streamed at double speed (like this game's opening) is 30 fps. Each segment's duration comes from
its XA audio (sector count x samples per sector / sample rate) and its frame count from the video
sector headers (STR magic 0x0160, frame number at +8), so fps = frames / duration. MPEG-1 has no
15 fps mode, so the .mpg is always 30 fps (15 fps movies show each frame twice).

Needs ffmpeg on PATH. Game data stays in the gitignored assets/ folders.
"""

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SECTOR = 2352
COMMON_FPS = (10, 12, 15, 20, 24, 25, 30, 60)


def xa_frames_per_sector(coding: int) -> tuple[int, int]:
    """(sample frames per sector, sample rate) for an XA audio sector's coding byte."""
    rate = 18900 if coding & 0x04 else 37800
    stereo = bool(coding & 0x01)
    samples = 4032 if not (coding & 0x10) else 2016  # 4-bit: 18 groups x 8 units x 28; 8-bit: half
    return (samples // 2 if stereo else samples), rate


def measure(data: bytes) -> tuple[int, float]:
    """(video frames, audio seconds) of one segment."""
    frames = set()
    audio_seconds = 0.0
    for off in range(0, len(data) - SECTOR + 1, SECTOR):
        sub = data[off + 16:off + 20]
        if sub[2] & 0x04:  # audio sector
            per, rate = xa_frames_per_sector(sub[3])
            audio_seconds += per / rate
            continue
        magic, _type, _chunk, _chunks, frame = struct.unpack_from("<HHHHI", data, off + 24)
        if magic == 0x0160:
            frames.add(frame)
    return len(frames), audio_seconds


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"ffmpeg failed: {' '.join(cmd)}\n{result.stderr[-2000:]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", default="SLPS-03101")
    ap.add_argument("--input", type=Path)
    ap.add_argument("--no-mpg", action="store_true", help="only write the .mp4 sources")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is required")

    cfg = json.loads((ROOT / "config" / args.serial / "movies.json").read_text())
    name = cfg["file"] + ".raw2352"
    src = args.input
    if src is None:
        for cand in (ROOT / "assets" / args.serial / "disc" / name, ROOT / "extracted" / args.serial / "fs" / name):
            if cand.is_file():
                src = cand
                break
    if src is None or not src.is_file():
        sys.exit(f"no {name}: import the disc first (dcb --import) or pass --input")
    data = src.read_bytes()
    total = len(data) // SECTOR
    segments = sorted(cfg["segments"], key=lambda s: s["start"])
    mp4_dir = ROOT / "assets" / args.serial / "movie_src"
    mpg_dir = ROOT / "assets" / "converted" / args.serial / "movie"
    mp4_dir.mkdir(parents=True, exist_ok=True)
    mpg_dir.mkdir(parents=True, exist_ok=True)
    print(f"{src}: {total} sectors")

    with tempfile.TemporaryDirectory() as tmp:
        for i, seg in enumerate(segments):
            end = segments[i + 1]["start"] if i + 1 < len(segments) else total
            part = data[seg["start"] * SECTOR:end * SECTOR]
            frames, seconds = measure(part)
            fps = min(COMMON_FPS, key=lambda f: abs(f - frames / seconds)) if seconds else 15
            str_path = Path(tmp) / f"movie{seg['index']}.str"
            str_path.write_bytes(part)
            mp4 = mp4_dir / f"movie{seg['index']}.mp4"
            # Re-time the video by frame number (the reader's 15 fps is wrong for double-speed movies).
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "psxstr", "-i", str(str_path),
                 "-vf", f"setpts=N/({fps}*TB)", "-r", str(fps), "-c:v", "libx264", "-preset", "slow", "-crf", "12",
                 "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-b:a", "192k", "-movflags", "+faststart",
                 str(mp4)])
            line = f"movie{seg['index']} ({seg['name']}): {frames} frames, {seconds:.1f} s of audio -> {fps} fps; {mp4}"
            if not args.no_mpg:
                mpg = mpg_dir / f"movie{seg['index']}.mpg"
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(mp4), "-c:v", "mpeg1video",
                     "-q:v", "2", "-r", "30", "-c:a", "mp2", "-b:a", "256k", "-ar", "44100", "-f", "mpeg", str(mpg)])
                line += f", {mpg}"
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
