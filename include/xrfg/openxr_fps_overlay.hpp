#pragma once

#include "xrfg/fps_overlay_model.hpp"
#include "xrfg/status_panel_model.hpp"
#include "xrfg/steamvr_delivery.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <filesystem>
#include <memory>
#include <optional>

namespace xrfg {

// Owns only its VIEW and LOCAL spaces and small color swapchains: the FPS
// counter's and the status panel's. Never modifies a game image, temporal
// history, game space, optical-flow job, or frame schedule.
class OpenXrFpsOverlay final {
public:
    OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
        PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
        ID3D12Device* device12, ID3D12CommandQueue* queue12,
        ID3D11Device* device11, const std::filesystem::path& ini,
        // Borrowed, owned by the session, and may be null. Shared with the
        // presenter, which reads the vsync anchor from the same connection.
        SteamVrDelivery* delivery);
    ~OpenXrFpsOverlay();
    // Called on the application's end-frame thread, not the presenter thread.
    // Uploads at most 4 Hz; no explicit GPU fence wait, image-wait timeout zero.
    // `status` is what the status panel shows, given whenever status_wanted()
    // said it was due; the panel keeps its last image otherwise.
    void application_frame(const XrFrameEndInfo* info,
                           const StatusPanelInput* status = nullptr) noexcept;
    // Whether the status panel is up, or about to be, and its next repaint
    // is due: the caller gathers a StatusPanelInput for application_frame
    // only then, a few times a second at most. Any thread.
    [[nodiscard]] bool status_wanted() noexcept;
    // The status panel's flip gesture reads these: grip-pose spaces of the
    // layer's own action on the application's input. Borrowed, destroyed by
    // the layer after this object. Either may be null.
    void set_grip_spaces(XrSpace left, XrSpace right) noexcept;
    // `new_content` is false for a repeat - the presenter handing the runtime
    // a frame it already submitted, to keep the cadence when the application
    // produced nothing. The quad still goes on, but it is not a frame and the
    // counter must not treat it as one.
    [[nodiscard]] XrResult end_frame(
        const XrFrameEndInfo* info, bool synthetic, bool new_content = true);
    void reset_metrics() noexcept;
    // The tray's "Pause frame generation": the counter carries a pause symbol
    // while set, so a screenshot says which state it was taken in.
    void set_paused(bool paused) noexcept;
    // The headset's scanout period as the layer knows it, so the counter can
    // draw the refresh rate for a rate within 2% of it (displayed_rate). Any
    // thread; 0 clears it.
    void set_display_period(std::int64_t period_ns) noexcept;
    // Terminal for this session. Keep resources alive until normal teardown;
    // stop adding the quad or uploading textures immediately.
    void suspend() noexcept;
    [[nodiscard]] FpsSnapshot metrics() const noexcept;
    // Angular counter bounds for a diagnostic burned into the S projection.
    // Available without a spare runtime quad layer; Off/suspend disables it.
    [[nodiscard]] std::optional<OverlayPlacement> marker_placement() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xrfg
