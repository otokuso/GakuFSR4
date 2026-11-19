#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Software-side measurements only. Present entry is not the display/scanout time.
struct TimingDistribution {
    std::size_t samples = 0;
    double medianMs = 0;
    double p95Ms = 0;
    double p99Ms = 0;
    double stddevMs = 0;
};

struct LiveTimingSummary {
    TimingDistribution present;
    TimingDistribution input;
    bool inputHookActive = false;
};

template<std::size_t Capacity = 512>
class TimingSampleWindow {
    static_assert(Capacity > 0, "Timing sample window must not be empty");
    struct Sample {
        double milliseconds = 0;
        uint64_t at = 0;
    };
    std::array<Sample, Capacity> samples_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;

public:
    void Add(double milliseconds, uint64_t at) {
        if (!std::isfinite(milliseconds) || milliseconds < 0 || !at) return;
        samples_[next_] = {milliseconds, at};
        next_ = (next_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    TimingDistribution Snapshot(uint64_t now, uint64_t frequency) const {
        TimingDistribution result;
        if (!frequency || !now) return result;
        std::array<double, Capacity> values{};
        double sum = 0;
        // Only the most recent ten seconds. Old input samples must not linger
        // in the UI after the player stops using the keyboard or mouse.
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& sample = samples_[i];
            if (sample.at > now || static_cast<double>(now - sample.at) / frequency > 10.0) continue;
            values[result.samples++] = sample.milliseconds;
            sum += sample.milliseconds;
        }
        if (!result.samples) return result;
        const double mean = sum / result.samples;
        double squares = 0;
        for (std::size_t i = 0; i < result.samples; ++i) {
            const double diff = values[i] - mean;
            squares += diff * diff;
        }
        result.stddevMs = std::sqrt(squares / result.samples);
        std::sort(values.begin(), values.begin() + result.samples);
        const auto percentile = [&](std::size_t numerator, std::size_t denominator) {
            // Nearest-rank percentile, with the rank rounded up.
            const std::size_t rank = (result.samples * numerator + denominator - 1) / denominator;
            return values[rank - 1];
        };
        result.medianMs = percentile(1, 2);
        result.p95Ms = percentile(95, 100);
        result.p99Ms = percentile(99, 100);
        return result;
    }
};

class LiveTimingTracker {
    TimingSampleWindow<> presents_;
    TimingSampleWindow<> inputs_;
    uint64_t lastPresent_ = 0;
    uint64_t pendingInput_ = 0;

public:
    void ObserveInput(uint64_t at) {
        // The most recent consumed event is paired with at most one subsequent
        // application Present. No message payloads or key values are retained.
        if (at) pendingInput_ = at;
    }

    void ObservePresent(uint64_t at, uint64_t frequency) {
        if (!at || !frequency) return;
        // Presents can return from different threads out of entry-time order.
        // A late return from an older Present must not rewind the baseline or
        // pair an input event with a frame that started before it arrived.
        if (lastPresent_ && at <= lastPresent_) return;
        if (lastPresent_) {
            const double gapMs = 1000.0 * static_cast<double>(at - lastPresent_) / frequency;
            if (gapMs <= 1000.0) presents_.Add(gapMs, at);
        }
        lastPresent_ = at;
        if (pendingInput_ && at >= pendingInput_) {
            const double ageMs = 1000.0 * static_cast<double>(at - pendingInput_) / frequency;
            if (ageMs <= 1000.0) inputs_.Add(ageMs, at);
            pendingInput_ = 0;
        }
    }

    LiveTimingSummary Snapshot(uint64_t now, uint64_t frequency) const {
        return {presents_.Snapshot(now, frequency), inputs_.Snapshot(now, frequency), false};
    }

    void Reset() { *this = {}; }
};

// Implemented by the Windows Present hook, and read by the GakuLoader F1 UI.
LiveTimingSummary ReadLiveTiming();
void RecordGamePresentQpc(uint64_t at);
void StartGameInputHook(void* window);
void StopGameInputHook();
