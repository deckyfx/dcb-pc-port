#pragma once
// Native movie playback: an MPEG-1 file (video + MP2 audio) decoded with pl_mpeg
// (third_party/pl_mpeg, MIT) into RGB frames and kAudioRate stereo samples. Pure C++, no SDL:
// the host loop advances it once per game frame and presents what it produced, so playback is
// deterministic (driven by frame count, not the wall clock).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace platform {

class MoviePlayer {
public:
    MoviePlayer();
    ~MoviePlayer();
    MoviePlayer(const MoviePlayer&) = delete;
    MoviePlayer& operator=(const MoviePlayer&) = delete;

    /// Start playing `bytes` (a whole .mpg file). False if it is not a playable MPEG-1 stream.
    bool open(std::vector<uint8_t> bytes);
    /// Advance by `seconds` of movie time, decoding the video frames and audio that fall in it.
    void advance(double seconds);
    /// The stream ended (or was never opened).
    bool ended() const;

    int width() const { return width_; }
    int height() const { return height_; }
    /// The latest video frame, 8-bit RGB, width() * height() * 3 bytes (empty before the first).
    const std::vector<uint8_t>& rgb() const { return rgb_; }
    /// Video frames decoded since open() (diagnostics).
    uint64_t video_frames() const { return video_frames_; }

    /// Interleaved stereo s16 samples at platform::kAudioRate decoded since the last call.
    std::vector<int16_t> take_audio();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<uint8_t> bytes_;  ///< the file (pl_mpeg reads it in place)
    std::vector<uint8_t> rgb_;
    std::vector<int16_t> audio_;
    int width_ = 0, height_ = 0;
    uint64_t video_frames_ = 0;
    double resample_pos_ = 0.0;  ///< fractional read position of the linear resampler
    float last_l_ = 0.0f, last_r_ = 0.0f;

    void on_video(const void* frame);
    void on_audio(const float* interleaved, unsigned count, int rate);
};

}  // namespace platform
