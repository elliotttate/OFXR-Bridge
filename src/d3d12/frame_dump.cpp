#include "xrfg/frame_dump.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;

std::uint32_t environment_number(const char* name, std::uint32_t fallback) {
    char value[32]{};
    const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return n && n < sizeof(value) ? static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)) : fallback;
}

constexpr std::uint32_t kMagic = 0x44585846;  // "FXXD"
}  // namespace

bool FrameDump::wanted() noexcept {
    if (!configured_) {
        configured_ = true;
        char value[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableA("XRFG_TEST_CAPTURE_FRAMES", value, MAX_PATH);
        if (n && n < MAX_PATH) directory_ = value;
        skip_ = environment_number("XRFG_TEST_CAPTURE_SKIP", skip_);
        count_ = environment_number("XRFG_TEST_CAPTURE_COUNT", count_);
        sequences_ = environment_number("XRFG_TEST_CAPTURE_SEQUENCES", sequences_);
        gap_ = environment_number("XRFG_TEST_CAPTURE_GAP", gap_);
        waiting_ = environment_number("XRFG_TEST_CAPTURE_WAIT", 0) != 0;
    }
    if (directory_.empty() || sequence_ >= sequences_ || count_ == 0) return false;
    if (waiting_) {
        // Started by a file named go in the directory, looked for twice a
        // second, and taken so that only one synthesizer answers it.
        if (frame_++ % 30 != 0 || !MoveFileExW((directory_ / L"go").c_str(),
                (directory_ / L"go.taken").c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        waiting_ = false;
        skip_ = 0;
        frame_ = 0;
    }
    const std::uint64_t frame = frame_++;
    if (frame < skip_) return false;
    const std::uint64_t period = static_cast<std::uint64_t>(count_) + gap_;
    const std::uint64_t position = (frame - skip_) % period;
    return position < count_ && (frame - skip_) / period == sequence_;
}

bool FrameDump::wait(std::uint64_t value) {
    if (fence_->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    if (SUCCEEDED(fence_->SetEventOnCompletion(value, event))) WaitForSingleObject(event, 10000);
    CloseHandle(event);
    return fence_->GetCompletedValue() >= value;
}

void FrameDump::capture(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* colour,
                        DXGI_FORMAT view_format, std::span<const D3D12ReprojectionView> views,
                        const DlssMotionVectorSet* guides) noexcept {
    try {
        const auto folder = directory_ / ("seq" + std::to_string(sequence_)) /
                            ("frame" + std::to_string(written_));
        const auto type = queue->GetDesc().Type;
        if (!fence_ && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            directory_.clear();
            return;
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
            directory_.clear();
            return;
        }
        std::map<ID3D12Resource*, std::string> names;
        // Each resource is copied whole into a texture of its own on the GPU.
        const auto add = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                             const std::string& name) -> std::string {
            if (!resource) return "";
            if (auto found = names.find(resource); found != names.end()) return found->second;
            auto description = resource->GetDesc();
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> copy;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy)))) {
                return "";
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = state;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            // A resource at rest in COMMON is promoted to a copy source.
            if (state != D3D12_RESOURCE_STATE_COMMON) list->ResourceBarrier(1, &barrier);
            list->CopyResource(copy.Get(), resource);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            if (state != D3D12_RESOURCE_STATE_COMMON) list->ResourceBarrier(1, &barrier);
            names[resource] = name;
            held_.push_back({folder / name, copy});
            return name;
        };
        std::string meta = "view_format=" + std::to_string(int(view_format)) + "\n";
        add(colour, D3D12_RESOURCE_STATE_COMMON, "colour.bin");
        meta += "view_count=" + std::to_string(views.size()) + "\n";
        char line[512];
        for (std::size_t i = 0; i < views.size(); ++i) {
            const auto& v = views[i];
            std::snprintf(line, sizeof(line),
                "view%zu=%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %u %u %u %u %u\n", i,
                v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z,
                v.pose.orientation.w, v.pose.position.x, v.pose.position.y, v.pose.position.z,
                v.fov.angle_left, v.fov.angle_right, v.fov.angle_up, v.fov.angle_down,
                v.image_rect.offset_x, v.image_rect.offset_y, v.image_rect.width,
                v.image_rect.height, v.array_slice);
            meta += line;
        }
        const std::uint32_t eyes = guides ? guides->eye_count : 0;
        meta += "eye_count=" + std::to_string(eyes) + "\n";
        for (std::uint32_t e = 0; e < eyes; ++e) {
            const auto& g = guides->eyes[e];
            if (!g) continue;
            const std::string motion = add(g->motion_vectors.Get(), g->resource_state,
                                           "motion" + std::to_string(e) + ".bin");
            const std::string depth = add(g->depth.Get(), g->depth_resource_state,
                                          "depth" + std::to_string(e) + ".bin");
            std::snprintf(line, sizeof(line),
                "guide%u=%llu %llu %llu %llu %u %u %u %u %u %u %u %u %u %u %.9g %.9g %.9g %.9g %.9g "
                "%.9g %d %d %u %u %u %u %.9g %.9g %.9g %d %d\n", e,
                static_cast<unsigned long long>(g->stream), static_cast<unsigned long long>(g->epoch),
                static_cast<unsigned long long>(g->serial),
                static_cast<unsigned long long>(g->previous_serial), g->output_x, g->output_y,
                g->output_width, g->output_height, g->output_slice, g->motion_x, g->motion_y,
                g->motion_width, g->motion_height, g->motion_slice, g->scale_x, g->scale_y,
                g->jitter_x, g->jitter_y, g->previous_jitter_x, g->previous_jitter_y,
                g->jittered ? 1 : 0, g->reset ? 1 : 0, g->depth_x, g->depth_y, g->depth_width,
                g->depth_height, g->frame_time_delta_ms, g->camera_near, g->camera_far,
                g->depth_inverted ? 1 : 0, g->depth_infinite ? 1 : 0);
            meta += line;
            meta += "guide" + std::to_string(e) + "_files=" + (motion.empty() ? "-" : motion) + " " +
                    (depth.empty() ? "-" : depth) + "\n";
        }
        if (FAILED(list->Close())) return;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence_.Get(), ++fence_value_))) return;
        allocators_.push_back(allocator);
        lists_.push_back(list);
        metas_.push_back({folder / "meta.txt", meta});
        if (++written_ >= count_) {
            flush(device, queue);
            written_ = 0;
            ++sequence_;
        }
    } catch (...) {
        directory_.clear();
    }
}

// Reads the run's copies back and writes them, once the run is complete.
void FrameDump::flush(ID3D12Device* device, ID3D12CommandQueue* queue) {
    struct Readback {
        const Held* held{};
        D3D12_RESOURCE_DESC description{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
        std::vector<UINT> rows;
        std::vector<UINT64> row_bytes;
        ComPtr<ID3D12Resource> buffer;
    };
    std::vector<Readback> readbacks;
    const auto type = queue->GetDesc().Type;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (wait(fence_value_) &&
        SUCCEEDED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
        for (const auto& held : held_) {
            Readback r;
            r.held = &held;
            r.description = held.copy->GetDesc();
            const UINT count = r.description.DepthOrArraySize * r.description.MipLevels;
            r.layouts.resize(count);
            r.rows.resize(count);
            r.row_bytes.resize(count);
            UINT64 total = 0;
            device->GetCopyableFootprints(&r.description, 0, count, 0, r.layouts.data(),
                                          r.rows.data(), r.row_bytes.data(), &total);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.buffer)))) {
                continue;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = held.copy.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
            for (UINT i = 0; i < count; ++i) {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = r.buffer.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = r.layouts[i];
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = held.copy.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = i;
                list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            readbacks.push_back(std::move(r));
        }
        if (SUCCEEDED(list->Close())) {
            ID3D12CommandList* lists[] = {list.Get()};
            queue->ExecuteCommandLists(1, lists);
            if (SUCCEEDED(queue->Signal(fence_.Get(), ++fence_value_)) && wait(fence_value_)) {
                for (auto& r : readbacks) {
                    std::filesystem::create_directories(r.held->path.parent_path());
                    std::ofstream out(r.held->path, std::ios::binary);
                    const std::uint32_t header[6]{kMagic, static_cast<std::uint32_t>(r.description.Width),
                        r.description.Height, r.description.DepthOrArraySize,
                        static_cast<std::uint32_t>(r.description.Format),
                        static_cast<std::uint32_t>(r.layouts.size())};
                    out.write(reinterpret_cast<const char*>(header), sizeof(header));
                    void* mapped = nullptr;
                    if (FAILED(r.buffer->Map(0, nullptr, &mapped))) continue;
                    for (std::size_t i = 0; i < r.layouts.size(); ++i) {
                        const std::uint32_t sizes[2]{static_cast<std::uint32_t>(r.row_bytes[i]), r.rows[i]};
                        out.write(reinterpret_cast<const char*>(sizes), sizeof(sizes));
                        const auto* base = static_cast<const std::uint8_t*>(mapped) + r.layouts[i].Offset;
                        for (UINT row = 0; row < r.rows[i]; ++row) {
                            out.write(reinterpret_cast<const char*>(base + std::size_t(row) *
                                r.layouts[i].Footprint.RowPitch), static_cast<std::streamsize>(r.row_bytes[i]));
                        }
                    }
                    const D3D12_RANGE none{0, 0};
                    r.buffer->Unmap(0, &none);
                }
                for (const auto& [path, text] : metas_) {
                    std::filesystem::create_directories(path.parent_path());
                    std::ofstream(path) << text;
                }
            }
        }
    }
    held_.clear();
    metas_.clear();
    allocators_.clear();
    lists_.clear();
}

bool read_dumped_texture(const std::filesystem::path& path, DumpedTexture* texture) {
    std::ifstream in(path, std::ios::binary);
    std::uint32_t header[6]{};
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic) return false;
    texture->width = header[1];
    texture->height = header[2];
    texture->array_size = header[3];
    texture->format = header[4];
    texture->subresources.resize(header[5]);
    for (auto& s : texture->subresources) {
        std::uint32_t sizes[2]{};
        if (!in.read(reinterpret_cast<char*>(sizes), sizeof(sizes))) return false;
        s.row_bytes = sizes[0];
        s.rows = sizes[1];
        s.data.resize(std::size_t(s.row_bytes) * s.rows);
        if (!in.read(reinterpret_cast<char*>(s.data.data()), static_cast<std::streamsize>(s.data.size())))
            return false;
    }
    return true;
}

}  // namespace xrfg
