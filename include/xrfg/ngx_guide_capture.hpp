#pragma once
#include <d3d12.h>

namespace xrfg {
// Capture the game's NGX upscaler guides without replacing its upscaler.
// Called outside the loader lock. Disabling preserves submitted snapshots
// until the layer drains its queues and retires the guide registry.
void configure_ngx_guide_capture(ID3D12CommandQueue* queue, bool enabled) noexcept;
void stop_ngx_guide_capture(ID3D12CommandQueue* queue) noexcept;
void update_ngx_guide_depth(float near_z, float far_z, float min_depth, float max_depth) noexcept;
}
