// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

// EDEN_NX2_FRAME_LIMIT=1: where the frame time goes, once a second. Each presented frame is timed
// with the interval since the previous one, the GPU busy time of the frame before (the union of
// its queue submissions' timestamps, the same figure the performance overlay shows as GPU %), how
// long the swapchain made the present wait, how long the present call took, and how many
// submissions, draws and render passes the frame had. The line ends with a verdict: the GPU, the
// swapchain, or the emulation threads leaving the GPU idle.
namespace Vulkan::FrameLimit {

/// Whether the frame limit report is on (EDEN_NX2_FRAME_LIMIT).
[[nodiscard]] bool Enabled();

/// A queue submission ran for gpu_ms on the device.
void NoteSubmission(double gpu_ms);

/// A draw was recorded (the attempts the GPU thread makes, not the ones it skips).
void NoteDraw();

/// A render pass ended.
void NoteRenderPass();

/// A frame is presented. gpu_busy_ms is the last sampled frame's GPU time, empty when no sample is
/// ready yet; image_wait_ms is how long the image was waited for; present_ms is the present call;
/// width and height are the window (swapchain) size of the frame that was presented.
void NotePresent(std::optional<double> gpu_busy_ms, double image_wait_ms, double present_ms,
                 u32 width, u32 height);

/// The size the game renders at, before the window upscale.
void NoteGuestSize(u32 width, u32 height);

/// The size of the scene's own render target this frame, where a title reports it. A game with
/// dynamic resolution changes it to hold its GPU time, so GPU times are only comparable between
/// windows that show the same scene size.
void NoteSceneSize(u32 width, u32 height);

} // namespace Vulkan::FrameLimit
