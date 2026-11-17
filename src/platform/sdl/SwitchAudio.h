#pragma once

#ifdef __SWITCH__

#include <switch.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "runtime/Audio.h"

namespace gm::host {

class SwitchAudioOut {
public:
    ~SwitchAudioOut() { shutdown(); }

    bool initialize() {
        if (ready_) return true;

        Result rc = audoutInitialize();
        if (R_FAILED(rc)) {
            std::fprintf(stderr, "[audout] initialize failed: 0x%08x\n", static_cast<unsigned>(rc));
            return false;
        }
        initialized_ = true;

        for (auto& slot : slots_) {
            slot.memory = std::aligned_alloc(kAlignment, kAllocationBytes);
            if (!slot.memory) {
                std::fprintf(stderr, "[audout] aligned allocation failed\n");
                shutdown();
                return false;
            }
            std::memset(slot.memory, 0, kAllocationBytes);
            slot.out = {};
            slot.out.buffer = slot.memory;
            slot.out.buffer_size = kAllocationBytes;
            slot.out.data_size = kOutputDataBytes;
            slot.out.data_offset = 0;
            slot.submitted = false;
        }

        rc = audoutStartAudioOut();
        if (R_FAILED(rc)) {
            std::fprintf(stderr, "[audout] start failed: 0x%08x\n", static_cast<unsigned>(rc));
            shutdown();
            return false;
        }

        source_.resize(kSourceFrames * 2);
        ready_ = true;
        paused_ = false;
        std::fprintf(stderr,
                     "[audout] ready rate=%u channels=%u format=%d buffers=%zu source_frames=%d output_frames=%d\n",
                     audoutGetSampleRate(), audoutGetChannelCount(),
                     static_cast<int>(audoutGetPcmFormat()), slots_.size(),
                     kSourceFrames, kOutputFrames);
        return true;
    }

    bool ready() const { return ready_; }

    void pump(Audio* audio) {
        if (!ready_ || paused_ || !audio) return;
        reclaimReleased();
        for (auto& slot : slots_) {
            if (!slot.submitted) submit(slot, *audio);
        }
    }

    void onGameChange() {
        if (!ready_) return;
        // Do not stop, flush or reopen audout here. Already-submitted audio is at most about
        // 80 ms; keeping the native service untouched avoids the SDL failure this backend replaces.
        std::fprintf(stderr, "[audout] game_change: native output remains active\n");
    }

    void setPaused(bool paused) {
        if (!ready_ || paused_ == paused) return;
        Result rc = paused ? audoutStopAudioOut() : audoutStartAudioOut();
        if (R_FAILED(rc)) {
            std::fprintf(stderr, "[audout] %s failed: 0x%08x\n",
                         paused ? "stop" : "start", static_cast<unsigned>(rc));
            return;
        }
        paused_ = paused;
        std::fprintf(stderr, "[audout] %s\n", paused ? "paused" : "resumed");
    }

    void shutdown() {
        if (ready_) {
            audoutStopAudioOut();
            bool flushed = false;
            audoutFlushAudioOutBuffers(&flushed);
            ready_ = false;
        }
        if (initialized_) {
            audoutExit();
            initialized_ = false;
        }
        for (auto& slot : slots_) {
            std::free(slot.memory);
            slot.memory = nullptr;
            slot.submitted = false;
            slot.out = {};
        }
    }

private:
    static constexpr size_t kAlignment = 0x1000;
    static constexpr size_t kAllocationBytes = 0x1000;
    static constexpr int kSourceFrames = 882;  // 20 ms at 44.1 kHz
    static constexpr int kOutputFrames = 960;  // 20 ms at 48 kHz
    static constexpr size_t kOutputDataBytes = kOutputFrames * 2 * sizeof(int16_t);

    struct Slot {
        AudioOutBuffer out{};
        void* memory = nullptr;
        bool submitted = false;
    };

    static int16_t toPcm16(float value) {
        value = std::max(-1.0f, std::min(1.0f, value));
        return static_cast<int16_t>(std::lrint(value * 32767.0f));
    }

    void reclaimReleased() {
        for (;;) {
            AudioOutBuffer* released = nullptr;
            u32 count = 0;
            Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
            if (R_FAILED(rc)) {
                std::fprintf(stderr, "[audout] reclaim failed: 0x%08x\n", static_cast<unsigned>(rc));
                return;
            }
            if (!released || count == 0) return;
            for (auto& slot : slots_) {
                if (&slot.out == released) {
                    slot.submitted = false;
                    break;
                }
            }
        }
    }

    void submit(Slot& slot, Audio& audio) {
        audio.mix(source_.data(), kSourceFrames);
        auto* output = static_cast<int16_t*>(slot.memory);
        for (int i = 0; i < kOutputFrames; ++i) {
            const double sourcePosition = static_cast<double>(i) * kSourceFrames / kOutputFrames;
            const int first = std::min(static_cast<int>(sourcePosition), kSourceFrames - 1);
            const int second = std::min(first + 1, kSourceFrames - 1);
            const float fraction = static_cast<float>(sourcePosition - first);
            for (int channel = 0; channel < 2; ++channel) {
                const float a = source_[first * 2 + channel];
                const float b = source_[second * 2 + channel];
                output[i * 2 + channel] = toPcm16(a + (b - a) * fraction);
            }
        }

        slot.out.data_size = kOutputDataBytes;
        armDCacheFlush(slot.memory, kAllocationBytes);
        Result rc = audoutAppendAudioOutBuffer(&slot.out);
        if (R_FAILED(rc)) {
            std::fprintf(stderr, "[audout] append failed: 0x%08x\n", static_cast<unsigned>(rc));
            return;
        }
        slot.submitted = true;
    }

    std::array<Slot, 4> slots_{};
    std::vector<float> source_;
    bool initialized_ = false;
    bool ready_ = false;
    bool paused_ = false;
};

}  // namespace gm::host

#endif
