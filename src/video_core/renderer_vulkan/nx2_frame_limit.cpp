// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/nx2_frame_limit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/logging.h"

namespace Vulkan::FrameLimit {

namespace {

using Clock = std::chrono::steady_clock;

/// A minute at 60 fps, ten seconds at 10: enough for the percentiles of a one-second window with
/// room for a present that ran long.
constexpr size_t MaxSamples = 600;
constexpr size_t MaxSlowSubmissions = 8;
constexpr auto ReportPeriod = std::chrono::seconds{1};

struct Sample {
    double interval_ms{};
    /// The GPU busy time of the frame presented before this one, or -1 without a sample.
    double gpu_busy_ms{-1.0};
    double image_wait_ms{};
    double present_ms{};
    u32 submissions{};
    u32 draws{};
    u32 render_passes{};
};

struct SlowSubmission {
    double ms{};
    u32 order{};
};

struct State {
    std::mutex mutex;
    std::array<Sample, MaxSamples> samples{};
    size_t count{};
    size_t next{};
    Clock::time_point window_start{};
    u64 window_frames{};
    Clock::time_point last_present{};
    u64 submissions{};
    u64 draws{};
    u64 render_passes{};
    u64 submissions_at_present{};
    u64 draws_at_present{};
    u64 render_passes_at_present{};
    std::array<SlowSubmission, MaxSlowSubmissions> slow{};
    size_t slow_count{};
    u32 width{};
    u32 height{};
    u32 guest_width{};
    u32 guest_height{};
    /// The smallest and largest scene size of the window, by area (0 until one is noted).
    u32 scene_min_width{};
    u32 scene_min_height{};
    u32 scene_max_width{};
    u32 scene_max_height{};
};

State& Data() {
    static State state;
    return state;
}

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

double Percentile(std::vector<double>& values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const double position = fraction * static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(position);
    const size_t next = std::min(index + 1, values.size() - 1);
    const double weight = position - static_cast<double>(index);
    return values[index] * (1.0 - weight) + values[next] * weight;
}

void Push(State& state, const Sample& sample) {
    state.samples[state.next] = sample;
    state.next = (state.next + 1) % MaxSamples;
    state.count = std::min(state.count + 1, MaxSamples);
}

/// Keeps the heaviest submissions of the window: a frame whose GPU time sits in one or two
/// submissions is a heavy pass, one spread over hundreds is per-draw or per-dispatch cost.
void KeepSlow(State& state, double ms, u32 order) {
    if (ms <= 0.0) {
        return;
    }
    if (state.slow_count < MaxSlowSubmissions) {
        state.slow[state.slow_count++] = {ms, order};
    } else if (ms > state.slow.back().ms) {
        state.slow.back() = {ms, order};
    } else {
        return;
    }
    std::ranges::sort(state.slow.begin(), state.slow.begin() + state.slow_count, std::greater<>{},
                      &SlowSubmission::ms);
}

} // Anonymous namespace

namespace {

void Report(State& state, Clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - state.window_start).count();
    const u64 frames = state.window_frames;
    state.window_start = now;
    state.window_frames = 0;
    if (frames == 0 || seconds <= 0.0) {
        state.count = 0;
        state.next = 0;
        state.slow_count = 0;
        return;
    }
    std::vector<double> intervals;
    std::vector<double> busy;
    double image_wait_sum{};
    double present_sum{};
    u64 submissions{};
    u64 draws{};
    u64 render_passes{};
    intervals.reserve(state.count);
    busy.reserve(state.count);
    for (size_t i = 0; i < state.count; ++i) {
        const Sample& sample = state.samples[i];
        if (sample.interval_ms > 0.0) {
            intervals.push_back(sample.interval_ms);
        }
        if (sample.gpu_busy_ms >= 0.0) {
            busy.push_back(sample.gpu_busy_ms);
        }
        image_wait_sum += sample.image_wait_ms;
        present_sum += sample.present_ms;
        submissions += sample.submissions;
        draws += sample.draws;
        render_passes += sample.render_passes;
    }
    const double frame_count = static_cast<double>(frames);
    const double interval_p50 = Percentile(intervals, 0.50);
    const double interval_p95 = Percentile(intervals, 0.95);
    const double interval_p99 = Percentile(intervals, 0.99);
    const double interval_max = intervals.empty() ? 0.0 : *std::ranges::max_element(intervals);
    const double busy_p50 = Percentile(busy, 0.50);
    const double busy_p95 = Percentile(busy, 0.95);
    const double busy_share = interval_p50 > 0.0 ? busy_p50 / interval_p50 * 100.0 : 0.0;
    const double image_wait_avg = image_wait_sum / frame_count;
    const double present_avg = present_sum / frame_count;
    // What is left of a typical frame once the device and the present path took their share: the
    // emulation threads not feeding the GPU.
    const double other_ms = interval_p50 - busy_p50 - image_wait_avg - present_avg;
    const char* verdict = "cpu/sync: the GPU is idle part of every frame";
    if (busy.empty()) {
        verdict = "no GPU sample yet (queue timestamps unavailable)";
    } else if (busy_share >= 90.0) {
        verdict = "gpu: the device is busy almost all of every frame";
    } else if (busy_share >= 70.0) {
        verdict = "mixed: the device is the main limit";
    } else if (image_wait_avg >= busy_p50 * 0.5) {
        verdict = "present: the swapchain holds frames back";
    }
    std::string scene;
    if (state.scene_max_width != 0) {
        scene = fmt::format(", scene {}x{}", state.scene_min_width, state.scene_min_height);
        if (state.scene_min_width != state.scene_max_width ||
            state.scene_min_height != state.scene_max_height) {
            scene += fmt::format(" to {}x{}", state.scene_max_width, state.scene_max_height);
        }
    }
    state.scene_min_width = state.scene_min_height = 0;
    state.scene_max_width = state.scene_max_height = 0;
    LOG_INFO(Render_Vulkan,
             "Nx2FrameLimit {:.1f} s: {:.1f} fps of {}x{} (game renders {}x{}{}); interval p50 "
             "{:.2f} p95 {:.2f} p99 {:.2f} max {:.2f} ms; GPU busy p50 {:.2f} p95 {:.2f} ms = "
             "{:.0f}% of the interval ({:.2f} ms a frame outside it); present image wait {:.2f} ms "
             "and call {:.2f} ms a frame; {:.1f} submissions, {:.0f} draws and {:.0f} render "
             "passes a frame; {}",
             seconds, frame_count / seconds, state.width, state.height, state.guest_width,
             state.guest_height, scene, interval_p50, interval_p95, interval_p99, interval_max,
             busy_p50,
             busy_p95, busy_share, other_ms, image_wait_avg, present_avg,
             static_cast<double>(submissions) / frame_count,
             static_cast<double>(draws) / frame_count,
             static_cast<double>(render_passes) / frame_count, verdict);
    if (state.slow_count != 0) {
        std::string slowest;
        for (size_t i = 0; i < state.slow_count; ++i) {
            slowest += fmt::format("{}{:.2f} ms", slowest.empty() ? "" : ", ", state.slow[i].ms);
        }
        LOG_INFO(Render_Vulkan, "Nx2FrameLimit slowest submissions of the window: {}", slowest);
    }
    state.count = 0;
    state.next = 0;
    state.slow_count = 0;
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FRAME_LIMIT");
        const bool on = value != nullptr && *value != '\0' && *value != '0';
        if (on) {
            LOG_INFO(Render_Vulkan,
                     "Frame limit report on (EDEN_NX2_FRAME_LIMIT): one line a second");
        }
        return on;
    }();
    return enabled;
}

void NoteSubmission(double gpu_ms) {
    if (!Enabled()) {
        return;
    }
    State& state = Data();
    std::scoped_lock lock{state.mutex};
    ++state.submissions;
    KeepSlow(state, gpu_ms, static_cast<u32>(state.submissions));
}

void NoteDraw() {
    if (!Enabled()) {
        return;
    }
    State& state = Data();
    std::scoped_lock lock{state.mutex};
    ++state.draws;
}

void NoteRenderPass() {
    if (!Enabled()) {
        return;
    }
    State& state = Data();
    std::scoped_lock lock{state.mutex};
    ++state.render_passes;
}

void NoteGuestSize(u32 width, u32 height) {
    if (!Enabled()) {
        return;
    }
    State& state = Data();
    std::scoped_lock lock{state.mutex};
    state.guest_width = width;
    state.guest_height = height;
}

void NoteSceneSize(u32 width, u32 height) {
    if (!Enabled() || width == 0 || height == 0) {
        return;
    }
    State& state = Data();
    std::scoped_lock lock{state.mutex};
    const u64 area = u64{width} * height;
    if (state.scene_max_width == 0 ||
        area < u64{state.scene_min_width} * state.scene_min_height) {
        state.scene_min_width = width;
        state.scene_min_height = height;
    }
    if (area > u64{state.scene_max_width} * state.scene_max_height) {
        state.scene_max_width = width;
        state.scene_max_height = height;
    }
}

void NotePresent(std::optional<double> gpu_busy_ms, double image_wait_ms, double present_ms,
                 u32 width, u32 height) {
    if (!Enabled()) {
        return;
    }
    State& state = Data();
    const Clock::time_point now = Clock::now();
    std::scoped_lock lock{state.mutex};
    state.width = width;
    state.height = height;
    if (state.window_start == Clock::time_point{}) {
        state.window_start = now;
    }
    Sample sample{};
    if (state.last_present != Clock::time_point{}) {
        sample.interval_ms = Ms(now - state.last_present);
    }
    state.last_present = now;
    sample.gpu_busy_ms = gpu_busy_ms ? *gpu_busy_ms : -1.0;
    sample.image_wait_ms = image_wait_ms;
    sample.present_ms = present_ms;
    sample.submissions = static_cast<u32>(state.submissions - state.submissions_at_present);
    sample.draws = static_cast<u32>(state.draws - state.draws_at_present);
    sample.render_passes = static_cast<u32>(state.render_passes - state.render_passes_at_present);
    state.submissions_at_present = state.submissions;
    state.draws_at_present = state.draws;
    state.render_passes_at_present = state.render_passes;
    Push(state, sample);
    ++state.window_frames;
    if (now - state.window_start >= ReportPeriod) {
        Report(state, now);
    }
}

} // namespace Vulkan::FrameLimit
