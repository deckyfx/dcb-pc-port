# Movies

[Home](Home.md)

The game's three movies (`movie0` opening, `movie1` credits, `movie2` BANDAI logo) play natively
when `movie/movie<N>.mpg` is in the asset pack or folder (see [Textures](Textures.md#replacement-textures)
for where the pack is found): full resolution, their own audio, any key skips; without them the disc
movie plays. With native movies the game reads nothing through the CD drive.

Files are MPEG-1 video + MP2 audio (decoded with [pl_mpeg](../../third_party/pl_mpeg)); MPEG-1 has
no 15 fps mode, so use 30:

```sh
# 1. Cut the disc movie into its three parts (config/<serial>/movies.json) at their true frame
#    rate (the opening streams at double speed: 30 fps; ffmpeg's reader assumes 15):
tools/disc/rip_movies.py        # -> assets/<serial>/movie_src/movie<N>.mp4 + converted .mpg
# 2. Your (upscaled) movie -> MPEG-1, then re-pack:
ffmpeg -i movie0_upscaled.mp4 -c:v mpeg1video -q:v 2 -r 30 -c:a mp2 -b:a 256k -ar 44100 -f mpeg \
       assets/converted/SLPS-03101/movie/movie0.mpg
./build/linux-debug/dcb_asset_ripper pack assets/converted/SLPS-03101 assets/SLPS-03101.pak
```

`rip_movies.py [--serial SLPS-03101] [--input FILE.raw2352] [--no-mpg]` needs ffmpeg on `PATH`. It
writes `assets/<serial>/movie_src/movie<N>.mp4` (H.264 + AAC at the true frame rate, for upscaling)
and `assets/converted/<serial>/movie/movie<N>.mpg` (MPEG-1 + MP2, 30 fps; 15 fps movies show each
frame twice). The input defaults to a disc override in `assets/<serial>/disc/` (e.g. the US movie,
see [Game Data](Game-Data.md#disc-file-overrides)), or else `extracted/<serial>/fs/`.

Save states are refused while a native movie plays. `DCB_TRACE_MOVIE=1` prints, per second, host
frames, movie audio samples and video frames (they should read ~60 / 44100 / the movie's fps).

Without native movies the boot FMV is decoded from the disc stream (MDEC, 24-bit, XA-ADPCM audio).
