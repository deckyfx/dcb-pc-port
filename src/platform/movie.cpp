#include "movie.hpp"

#include "platform.hpp"

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"

#include <algorithm>
#include <cmath>

namespace platform {

struct MoviePlayer::Impl {
    plm_t* plm = nullptr;
    ~Impl() {
        if (plm) plm_destroy(plm);
    }
};

MoviePlayer::MoviePlayer() : impl_(std::make_unique<Impl>()) {}
MoviePlayer::~MoviePlayer() = default;

bool MoviePlayer::open(std::vector<uint8_t> bytes) {
    if (impl_->plm) {
        plm_destroy(impl_->plm);
        impl_->plm = nullptr;
    }
    bytes_ = std::move(bytes);
    rgb_.clear();
    audio_.clear();
    video_frames_ = 0;
    resample_pos_ = 0.0;
    last_l_ = last_r_ = 0.0f;
    if (bytes_.empty()) return false;
    impl_->plm = plm_create_with_memory(bytes_.data(), bytes_.size(), 0);
    if (!impl_->plm || !plm_probe(impl_->plm, 5000 * 1024)) {
        if (impl_->plm) plm_destroy(impl_->plm);
        impl_->plm = nullptr;
        return false;
    }
    width_ = plm_get_width(impl_->plm);
    height_ = plm_get_height(impl_->plm);
    plm_set_loop(impl_->plm, 0);
    plm_set_audio_enabled(impl_->plm, plm_get_num_audio_streams(impl_->plm) > 0);
    plm_set_video_decode_callback(
        impl_->plm, [](plm_t*, plm_frame_t* frame, void* self) { static_cast<MoviePlayer*>(self)->on_video(frame); },
        this);
    plm_set_audio_decode_callback(
        impl_->plm,
        [](plm_t* plm, plm_samples_t* samples, void* self) {
            static_cast<MoviePlayer*>(self)->on_audio(samples->interleaved, samples->count, plm_get_samplerate(plm));
        },
        this);
    return true;
}

void MoviePlayer::advance(double seconds) {
    if (impl_->plm && !plm_has_ended(impl_->plm)) plm_decode(impl_->plm, seconds);
}

bool MoviePlayer::ended() const { return !impl_->plm || plm_has_ended(impl_->plm); }

std::vector<int16_t> MoviePlayer::take_audio() {
    std::vector<int16_t> out;
    out.swap(audio_);
    return out;
}

void MoviePlayer::on_video(const void* frame) {
    const auto* f = static_cast<const plm_frame_t*>(frame);
    ++video_frames_;
    width_ = static_cast<int>(f->width);
    height_ = static_cast<int>(f->height);
    rgb_.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 3);
    plm_frame_to_rgb(const_cast<plm_frame_t*>(f), rgb_.data(), width_ * 3);
}

void MoviePlayer::on_audio(const float* interleaved, unsigned count, int rate) {
    // Linear resampling from the stream's rate to kAudioRate (MP2 is usually 44.1 or 48 kHz).
    const double step = static_cast<double>(rate) / kAudioRate;
    const auto to_s16 = [](float v) {
        return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    };
    double pos = resample_pos_;
    while (pos < static_cast<double>(count)) {
        const auto i = static_cast<unsigned>(pos);
        const float t = static_cast<float>(pos - i);
        const float l0 = i == 0 ? last_l_ : interleaved[(i - 1) * 2], r0 = i == 0 ? last_r_ : interleaved[(i - 1) * 2 + 1];
        const float l1 = interleaved[i * 2], r1 = interleaved[i * 2 + 1];
        audio_.push_back(to_s16(l0 + (l1 - l0) * t));
        audio_.push_back(to_s16(r0 + (r1 - r0) * t));
        pos += step;
    }
    resample_pos_ = pos - count;
    if (count) {
        last_l_ = interleaved[(count - 1) * 2];
        last_r_ = interleaved[(count - 1) * 2 + 1];
    }
}

}  // namespace platform
