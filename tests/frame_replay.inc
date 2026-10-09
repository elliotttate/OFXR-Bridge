// XRFG_TEST_REPLAY=<sequence directory>: real frames recorded with
// XRFG_TEST_CAPTURE_FRAMES. Every method generates frame1 from frame0 and
// frame2 - the game's vectors doubled, as from frame2 to frame0 - and is
// compared with the real frame1: overall, and where frames 0 and 2 differ.

struct ReplayFrame {
    DXGI_FORMAT view_format{};
    std::vector<xrfg::D3D12ReprojectionView> views;
    xrfg::DumpedTexture colour;
    std::vector<xrfg::DlssMotionVectorFrame> guides;
    std::vector<std::string> motion_files, depth_files;
};

ReplayFrame load_replay_frame(const std::filesystem::path& folder) {
    ReplayFrame frame;
    std::ifstream meta(folder / "meta.txt");
    require(meta.good(), "replay: missing " + (folder / "meta.txt").string());
    std::string line;
    while (std::getline(meta, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        std::istringstream value(line.substr(equals + 1));
        if (key == "view_format") {
            int format = 0;
            value >> format;
            frame.view_format = static_cast<DXGI_FORMAT>(format);
        } else if (key.rfind("view", 0) == 0 && key != "view_count") {
            xrfg::D3D12ReprojectionView v{};
            value >> v.pose.orientation.x >> v.pose.orientation.y >> v.pose.orientation.z >>
                v.pose.orientation.w >> v.pose.position.x >> v.pose.position.y >> v.pose.position.z >>
                v.fov.angle_left >> v.fov.angle_right >> v.fov.angle_up >> v.fov.angle_down >>
                v.image_rect.offset_x >> v.image_rect.offset_y >> v.image_rect.width >>
                v.image_rect.height >> v.array_slice;
            frame.views.push_back(v);
        } else if (key.rfind("guide", 0) == 0 && key.find("_files") != std::string::npos) {
            std::string motion, depth;
            value >> motion >> depth;
            frame.motion_files.push_back(motion);
            frame.depth_files.push_back(depth);
        } else if (key.rfind("guide", 0) == 0) {
            xrfg::DlssMotionVectorFrame g{};
            unsigned long long stream = 0, epoch = 0, serial = 0, previous = 0;
            int jittered = 0, reset = 0, inverted = 0, infinite = 0;
            value >> stream >> epoch >> serial >> previous >> g.output_x >> g.output_y >>
                g.output_width >> g.output_height >> g.output_slice >> g.motion_x >> g.motion_y >>
                g.motion_width >> g.motion_height >> g.motion_slice >> g.scale_x >> g.scale_y >>
                g.jitter_x >> g.jitter_y >> g.previous_jitter_x >> g.previous_jitter_y >> jittered >>
                reset >> g.depth_x >> g.depth_y >> g.depth_width >> g.depth_height >>
                g.frame_time_delta_ms >> g.camera_near >> g.camera_far >> inverted >> infinite;
            g.stream = stream;
            g.epoch = epoch;
            g.jittered = jittered != 0;
            g.reset = reset != 0;
            g.depth_inverted = inverted != 0;
            g.depth_infinite = infinite != 0;
            frame.guides.push_back(g);
        }
    }
    require(xrfg::read_dumped_texture(folder / "colour.bin", &frame.colour), "replay: colour");
    // XRFG_TEST_REPLAY_SWAP_EYES=1 gives each view the other eye's guide.
    if (std::getenv("XRFG_TEST_REPLAY_SWAP_EYES") && frame.guides.size() == 2) {
        std::swap(frame.motion_files[0], frame.motion_files[1]);
        std::swap(frame.depth_files[0], frame.depth_files[1]);
        auto& a = frame.guides[0];
        auto& b = frame.guides[1];
        std::swap(a.motion_x, b.motion_x); std::swap(a.motion_y, b.motion_y);
        std::swap(a.motion_slice, b.motion_slice);
        std::swap(a.depth_x, b.depth_x); std::swap(a.depth_y, b.depth_y);
        std::swap(a.jitter_x, b.jitter_x); std::swap(a.jitter_y, b.jitter_y);
        std::swap(a.previous_jitter_x, b.previous_jitter_x);
        std::swap(a.previous_jitter_y, b.previous_jitter_y);
    }
    return frame;
}

ComPtr<ID3D12Resource> upload_dumped(D3D12WarpFixture& fixture, const xrfg::DumpedTexture& texture,
                                     D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES rest) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = texture.width;
    description.Height = texture.height;
    description.DepthOrArraySize = static_cast<UINT16>(texture.array_size);
    description.MipLevels = 1;
    description.Format = static_cast<DXGI_FORMAT>(texture.format);
    description.SampleDesc.Count = 1;
    description.Flags = flags;
    const D3D12_HEAP_PROPERTIES defaults = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;
    require_hresult(fixture.device()->CreateCommittedResource(&defaults, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource)),
        "replay texture");
    const UINT count = static_cast<UINT>(texture.subresources.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    fixture.device()->GetCopyableFootprints(&description, 0, count, 0, layouts.data(), rows.data(),
                                            row_bytes.data(), &total);
    auto upload = create_buffer(fixture, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    require_hresult(upload->Map(0, nullptr, &mapped), "replay upload map");
    for (UINT i = 0; i < count; ++i) {
        const auto& s = texture.subresources[i];
        for (UINT row = 0; row < rows[i]; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(mapped) + layouts[i].Offset +
                            std::size_t(row) * layouts[i].Footprint.RowPitch,
                        s.data.data() + std::size_t(row) * s.row_bytes, s.row_bytes);
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = i;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[i];
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const auto barrier = transition_barrier(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, rest);
        list->ResourceBarrier(1, &barrier);
    });
    return resource;
}

xrfg::DumpedTexture read_back_texture(D3D12WarpFixture& fixture, ID3D12Resource* resource,
                                      D3D12_RESOURCE_STATES state) {
    const auto description = resource->GetDesc();
    const UINT count = description.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
    std::vector<UINT> rows(count);
    std::vector<UINT64> row_bytes(count);
    UINT64 total = 0;
    fixture.device()->GetCopyableFootprints(&description, 0, count, 0, layouts.data(), rows.data(),
                                            row_bytes.data(), &total);
    auto readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
        auto barrier = transition_barrier(resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        for (UINT i = 0; i < count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[i];
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = resource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = i;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        barrier = transition_barrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        list->ResourceBarrier(1, &barrier);
    });
    xrfg::DumpedTexture texture;
    texture.width = static_cast<std::uint32_t>(description.Width);
    texture.height = description.Height;
    texture.array_size = count;
    texture.format = description.Format;
    void* mapped = nullptr;
    require_hresult(readback->Map(0, nullptr, &mapped), "replay readback map");
    for (UINT i = 0; i < count; ++i) {
        xrfg::DumpedTexture::Subresource s;
        s.row_bytes = static_cast<std::uint32_t>(row_bytes[i]);
        s.rows = rows[i];
        s.data.resize(std::size_t(s.row_bytes) * s.rows);
        for (UINT row = 0; row < rows[i]; ++row) {
            std::memcpy(s.data.data() + std::size_t(row) * s.row_bytes,
                        static_cast<const std::uint8_t*>(mapped) + layouts[i].Offset +
                            std::size_t(row) * layouts[i].Footprint.RowPitch,
                        s.row_bytes);
        }
        texture.subresources.push_back(std::move(s));
    }
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return texture;
}

// A texel's colour on an 8-bit scale, for the formats swapchains use.
std::array<float, 3> replay_texel(const xrfg::DumpedTexture& t, UINT slice, UINT x, UINT y) {
    const auto& s = t.subresources[slice];
    const std::uint8_t* p = s.data.data() + std::size_t(y) * s.row_bytes;
    switch (static_cast<DXGI_FORMAT>(t.format)) {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        std::uint32_t v = 0;
        std::memcpy(&v, p + std::size_t(x) * 4, 4);
        return {float(v & 1023) * 255 / 1023, float((v >> 10) & 1023) * 255 / 1023,
                float((v >> 20) & 1023) * 255 / 1023};
    }
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<float, 3> out{};
        for (UINT c = 0; c < 3; ++c) {
            std::uint16_t h = 0;
            std::memcpy(&h, p + std::size_t(x) * 8 + c * 2, 2);
            const int exponent = (h >> 10) & 31, mantissa = h & 1023;
            float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                        : std::ldexp(float(mantissa | 1024), exponent - 25);
            if (h & 0x8000) value = -value;
            out[c] = std::clamp(value, 0.0F, 1.0F) * 255;
        }
        return out;
    }
    default:
        return {float(p[std::size_t(x) * 4]), float(p[std::size_t(x) * 4 + 1]),
                float(p[std::size_t(x) * 4 + 2])};
    }
}

void replay_held_out(const std::filesystem::path& directory) {
    D3D12WarpFixture fixture(true);
    // XRFG_TEST_REPLAY_FIRST picks the first of the three frames (0).
    const int first = std::getenv("XRFG_TEST_REPLAY_FIRST") ? std::atoi(std::getenv("XRFG_TEST_REPLAY_FIRST")) : 0;
    const auto frame_dir = [&](int i) { return directory / ("frame" + std::to_string(first + i)); };
    // XRFG_TEST_REPLAY_EXTRAPOLATE=1 predicts frame 2 from frames 0 and 1
    // instead: B is then frame 1, with its own vectors, and frame 2 the
    // truth. Below, f1 is always the truth and f2 always B.
    const bool extrapolate = std::getenv("XRFG_TEST_REPLAY_EXTRAPOLATE") != nullptr;
    const int b_index = extrapolate ? 1 : 2, truth_index = extrapolate ? 2 : 1;
    const ReplayFrame f0 = load_replay_frame(frame_dir(0));
    const ReplayFrame f1 = load_replay_frame(frame_dir(truth_index));
    const ReplayFrame f2 = load_replay_frame(frame_dir(b_index));
    // XRFG_TEST_REPLAY_GUIDE_SHIFT takes frame2's guides from a neighbouring
    // frame instead, to test whether colour and guides line up in time.
    const int guide_shift = std::getenv("XRFG_TEST_REPLAY_GUIDE_SHIFT")
        ? std::atoi(std::getenv("XRFG_TEST_REPLAY_GUIDE_SHIFT")) : 0;
    const ReplayFrame f2_guides = guide_shift ? load_replay_frame(frame_dir(b_index + guide_shift)) : f2;
    const UINT width = f0.colour.width, height = f0.colour.height;
    const UINT slices = f0.colour.array_size;
    std::cout << "replay " << directory.string() << ": " << width << "x" << height << "x" << slices
              << " format " << f0.colour.format << ", " << f0.views.size() << " views, "
              << f2.guides.size() << " guides\n";
    const auto colour_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    // Frame times from DLSS: frame2's vectors reach frame1 in dt2, and frame0
    // is dt1 earlier still, so they are scaled to cover both, and frame1 lies
    // dt1 / (dt1 + dt2) of the way. Native generation is fixed at a half.
    const float dt1 = f1.guides.empty() ? 1.0F : f1.guides[0].frame_time_delta_ms;
    const float dt2 = f2.guides.empty() ? 1.0F : f2.guides[0].frame_time_delta_ms;
    // XRFG_TEST_REPLAY_VECTOR_GAIN multiplies that, to find what the vectors span.
    const float gain = std::getenv("XRFG_TEST_REPLAY_VECTOR_GAIN")
        ? float(std::atof(std::getenv("XRFG_TEST_REPLAY_VECTOR_GAIN"))) : 1.0F;
    // Extrapolating, B's vectors span exactly B to A, and the truth lies
    // dt1 past B: 1 + dt1 / dt2 in spans from A.
    const float vector_scale = extrapolate ? gain : gain * (dt2 > 0 ? (dt1 + dt2) / dt2 : 2.0F);
    // XRFG_TEST_REPLAY_FRACTION puts OFXR's frame elsewhere, such as at a
    // half like native generation's.
    const float fraction = std::getenv("XRFG_TEST_REPLAY_FRACTION")
        ? float(std::atof(std::getenv("XRFG_TEST_REPLAY_FRACTION")))
        : extrapolate ? 1.0F + (dt2 > 0 ? dt1 / dt2 : 1.0F)
        : dt1 + dt2 > 0 ? dt1 / (dt1 + dt2) : 0.5F;
    std::cout << "replay frame times " << dt1 << " and " << dt2 << " ms: fraction " << fraction
              << ", vectors x" << vector_scale << '\n';
    // Guides as the layer hands them on: A's, and B's scaled to reach A.
    const auto make_guides = [&](const ReplayFrame& frame, std::uint64_t serial, float scale,
                                 const ReplayFrame* previous) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = static_cast<std::uint32_t>(frame.guides.size());
        std::map<std::string, ComPtr<ID3D12Resource>> uploaded;
        for (std::size_t e = 0; e < frame.guides.size(); ++e) {
            auto g = std::make_shared<xrfg::DlssMotionVectorFrame>(frame.guides[e]);
            g->stream = f2.guides[e].stream;
            g->epoch = f2.guides[e].epoch;
            g->serial = serial;
            g->previous_serial = serial - 1;
            g->scale_x *= scale;
            g->scale_y *= scale;
            if (previous) {
                g->previous_jitter_x = previous->guides[e].jitter_x;
                g->previous_jitter_y = previous->guides[e].jitter_y;
            }
            g->reset = false;
            g->producer_queue = fixture.queue();
            const auto load = [&](const std::string& name) -> ComPtr<ID3D12Resource> {
                if (name == "-") return {};
                auto found = uploaded.find(name);
                if (found != uploaded.end()) return found->second;
                xrfg::DumpedTexture t;
                require(xrfg::read_dumped_texture(frame_dir(&frame == &f0 ? 0 : b_index + guide_shift) / name, &t),
                        "replay: guide " + name);
                auto r = upload_dumped(fixture, t, D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                uploaded[name] = r;
                return r;
            };
            g->motion_vectors = load(frame.motion_files[e]);
            g->depth = load(frame.depth_files[e]);
            g->resource_state = g->depth_resource_state =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            set->eyes[e] = g;
        }
        return set;
    };
    const auto guides_a = make_guides(f0, 1, 1.0F, nullptr);
    const auto guides_b = make_guides(f2_guides, 2, vector_scale, &f0);
    // Runs body(first, end, thread) over [0, count) on every core.
    const auto parallel = [](UINT count, const auto& body) {
        const UINT threads = std::max(1U, std::min(count, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (UINT i = 0; i < threads; ++i)
            pool.emplace_back([&, i] { body(count * i / threads, count * (i + 1) / threads, i); });
        for (auto& thread : pool) thread.join();
        return threads;
    };
    // An image as RGB floats, one row of the array after another.
    const auto rgb = [&](const xrfg::DumpedTexture& image) {
        std::vector<float> out(std::size_t(width) * height * slices * 3);
        parallel(height * slices, [&](UINT first, UINT end, UINT) {
            for (UINT row = first; row < end; ++row)
                for (UINT x = 0; x < width; ++x) {
                    const auto c = replay_texel(image, row / height, x, row % height);
                    for (UINT k = 0; k < 3; ++k) out[(std::size_t(row) * width + x) * 3 + k] = c[k];
                }
        });
        return out;
    };
    const std::vector<float> rgb0 = rgb(extrapolate ? f1.colour : f0.colour), rgb2 = rgb(f2.colour);
    // Where the scene moved between frames 0 and 2 (extrapolating, between
    // B and the truth).
    std::vector<std::uint8_t> moving(std::size_t(width) * height * slices, 0);
    for (std::size_t i = 0; i < moving.size(); ++i) {
        float d = 0;
        for (UINT c = 0; c < 3; ++c) d = std::max(d, std::abs(rgb0[i * 3 + c] - rgb2[i * 3 + c]));
        moving[i] = d > 6;
    }
    // XRFG_TEST_REPLAY_SAVE=<dir> writes each image's crop around the motion.
    const char* save_dir = std::getenv("XRFG_TEST_REPLAY_SAVE");
    UINT box_left = width, box_top = height, box_right = 0, box_bottom = 0, box_slice = 0;
    for (UINT y = 8; y + 8 < height; ++y)
        for (UINT x = 8; x + 8 < width; ++x)
            if (moving[std::size_t(y) * width + x]) {
                box_left = std::min(box_left, x); box_right = std::max(box_right, x);
                box_top = std::min(box_top, y); box_bottom = std::max(box_bottom, y);
            }
    // XRFG_TEST_REPLAY_SAVE_FULL=1 saves whole images instead.
    if (std::getenv("XRFG_TEST_REPLAY_SAVE_FULL")) {
        box_left = 0; box_top = 0; box_right = width - 1; box_bottom = height - 1;
    } else if (box_left < box_right) {
        box_left = box_left > 32 ? box_left - 32 : 0;
        box_top = box_top > 32 ? box_top - 32 : 0;
        box_right = std::min(std::min(box_right + 32, width - 1), box_left + 1023);
        box_bottom = std::min(std::min(box_bottom + 32, height - 1), box_top + 1023);
    }
    const auto save = [&](const std::string& name, const xrfg::DumpedTexture& image) {
        if (!save_dir || box_left >= box_right) return;
        std::filesystem::create_directories(save_dir);
        std::string file = name;
        for (auto& c : file) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        std::ofstream out(std::filesystem::path(save_dir) / (file + ".ppm"), std::ios::binary);
        out << "P6\n" << (box_right - box_left + 1) << " " << (box_bottom - box_top + 1) << "\n255\n";
        for (UINT y = box_top; y <= box_bottom; ++y)
            for (UINT x = box_left; x <= box_right; ++x) {
                const auto c = replay_texel(image, box_slice, x, y);
                for (UINT k = 0; k < 3; ++k) out.put(static_cast<char>(std::lround(std::clamp(c[k], 0.0F, 255.0F))));
            }
    };
    save("0 frame0", f0.colour);
    save("1 truth frame1", f1.colour);
    save("2 frame2", f2.colour);
    // Luma, for the structural scores below.
    const auto luma = [&](const std::vector<float>& image) {
        std::vector<float> out(image.size() / 3);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = 0.299F * image[i * 3] + 0.587F * image[i * 3 + 1] + 0.114F * image[i * 3 + 2];
        return out;
    };
    const std::vector<float> truth_rgb = rgb(f1.colour);
    const std::vector<float> truth_luma = luma(truth_rgb);
    const auto score = [&](const char* name, const xrfg::DumpedTexture& actual) {
        save(std::string("m ") + name, actual);
        const std::vector<float> out_rgb = rgb(actual);
        const std::vector<float> out_luma = luma(out_rgb);
        // Per thread: absolute error everywhere and where the scene moved,
        // their counts, and the structural sums described below.
        struct Sums {
            double all = 0, moved = 0, ssim = 0, gradient_out = 0, gradient_truth = 0, gradient_error = 0;
            std::size_t all_count = 0, moved_count = 0, blocks = 0, gradients = 0;
        };
        std::vector<Sums> sums(std::thread::hardware_concurrency() + 1);
        const UINT rows = height * slices;
        parallel(rows, [&](UINT first, UINT end, UINT thread) {
            Sums& sum = sums[thread];
            for (UINT row = first; row < end; ++row) {
                const UINT y = row % height;
                if (y < 8 || y + 8 >= height) continue;
                for (UINT x = 8; x + 8 < width; ++x) {
                    const std::size_t i = std::size_t(row) * width + x;
                    double e = 0;
                    for (UINT c = 0; c < 3; ++c) e += std::abs(out_rgb[i * 3 + c] - truth_rgb[i * 3 + c]);
                    sum.all += e;
                    sum.all_count += 3;
                    if (!moving[i]) continue;
                    sum.moved += e;
                    sum.moved_count += 3;
                    // The output's gradient against the truth's (below 1 is
                    // blurrier) and their mean difference, which a blur and a
                    // misplaced edge both raise.
                    const float* a = out_luma.data();
                    const float* tr = truth_luma.data();
                    const double gax = a[i + 1] - a[i - 1], gay = a[i + width] - a[i - width];
                    const double gtx = tr[i + 1] - tr[i - 1], gty = tr[i + width] - tr[i - width];
                    sum.gradient_out += std::sqrt(gax * gax + gay * gay);
                    sum.gradient_truth += std::sqrt(gtx * gtx + gty * gty);
                    sum.gradient_error += std::sqrt((gax - gtx) * (gax - gtx) + (gay - gty) * (gay - gty));
                    ++sum.gradients;
                }
                // SSIM over the 8x8 luma blocks starting on this row, where
                // most of the block moved.
                if (y % 8 != 0 || y + 16 > height) continue;
                constexpr double c1 = 6.5025, c2 = 58.5225;
                for (UINT bx = 8; bx + 16 <= width; bx += 8) {
                    double ma = 0, mt = 0, aa = 0, tt = 0, at = 0;
                    int count = 0;
                    for (UINT by = 0; by < 8; ++by)
                        for (UINT x = bx; x < bx + 8; ++x) {
                            const std::size_t i = (std::size_t(row) + by) * width + x;
                            const double va = out_luma[i], vt = truth_luma[i];
                            ma += va; mt += vt; aa += va * va; tt += vt * vt; at += va * vt;
                            count += moving[i];
                        }
                    if (count < 32) continue;
                    ma /= 64; mt /= 64;
                    const double va = aa / 64 - ma * ma, vt = tt / 64 - mt * mt, cov = at / 64 - ma * mt;
                    sum.ssim += ((2 * ma * mt + c1) * (2 * cov + c2)) / ((ma * ma + mt * mt + c1) * (va + vt + c2));
                    ++sum.blocks;
                }
            }
        });
        Sums total;
        for (const auto& s : sums) {
            total.all += s.all; total.moved += s.moved; total.ssim += s.ssim;
            total.gradient_out += s.gradient_out; total.gradient_truth += s.gradient_truth;
            total.gradient_error += s.gradient_error;
            total.all_count += s.all_count; total.moved_count += s.moved_count;
            total.blocks += s.blocks; total.gradients += s.gradients;
        }
        std::cout << "replay " << name << " mae=" << total.all / double(total.all_count)
                  << " moving=" << (total.moved_count ? total.moved / double(total.moved_count) : 0.0)
                  << " moving_fraction=" << double(total.moved_count) / double(total.all_count)
                  << " ssim=" << (total.blocks ? total.ssim / double(total.blocks) : 1.0)
                  << " sharpness=" << (total.gradient_truth > 0 ? total.gradient_out / total.gradient_truth : 1.0)
                  << " gradient_error=" << (total.gradients ? total.gradient_error / double(total.gradients) : 0.0)
                  << '\n';
    };
    score("repeat frame2", f2.colour);
    {
        xrfg::DumpedTexture blend = f2.colour;
        for (UINT s = 0; s < slices; ++s)
            for (std::size_t i = 0; i < blend.subresources[s].data.size(); ++i)
                blend.subresources[s].data[i] = static_cast<std::uint8_t>(
                    (int(f0.colour.subresources[s].data[i]) + f2.colour.subresources[s].data[i] + 1) / 2);
        const auto f = static_cast<DXGI_FORMAT>(f0.colour.format);
        if (f != DXGI_FORMAT_R10G10B10A2_UNORM && f != DXGI_FORMAT_R10G10B10A2_TYPELESS &&
            f != DXGI_FORMAT_R16G16B16A16_FLOAT && f != DXGI_FORMAT_R16G16B16A16_TYPELESS)
            score("blend", blend);
    }
    const std::span<const xrfg::D3D12ReprojectionView> views0(f0.views), views1(f1.views), views2(f2.views);
    struct Method {
        std::string name;
        xrfg::D3D12OpticalFlowBackend backend;
        xrfg::D3D12OpticalFlowInputScale scale;
        bool guides;
        xrfg::D3D12NvidiaPerformancePreset preset{xrfg::D3D12NvidiaPerformancePreset::medium};
        bool bidirectional{};
        bool hybrid{};
        bool extrapolate{};
        bool extrapolate_hybrid{};
    };
    std::vector<Method> methods{
        {"OFXR+DLSS vectors", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::half, true},
        {"OFXR FidelityFX half-res flow", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::half, false},
        {"OFXR FidelityFX full-res flow", xrfg::D3D12OpticalFlowBackend::fidelity_fx,
         xrfg::D3D12OpticalFlowInputScale::full, false},
        {"OFXR NVIDIA medium flow", xrfg::D3D12OpticalFlowBackend::nvidia,
         xrfg::D3D12OpticalFlowInputScale::half, false}};
    // XRFG_TEST_REPLAY_FLOWS lists OFXR configurations to run instead, each
    // backend/preset/scale[/bi]: vectors, hybrid (vectors and FidelityFX
    // flow), extrapolate (from the game's vectors and depth, for
    // XRFG_TEST_REPLAY_EXTRAPOLATE), ffx or nvidia; slow, medium or fast;
    // full, three_quarter or half. For example nvidia/fast/full/bi.
    if (const char* list = std::getenv("XRFG_TEST_REPLAY_FLOWS")) {
        methods.clear();
        std::stringstream stream(list);
        for (std::string item; std::getline(stream, item, ',');) {
            std::vector<std::string> parts;
            std::stringstream fields(item);
            for (std::string part; std::getline(fields, part, '/');) parts.push_back(part);
            require(parts.size() >= 3, "replay: flow " + item);
            Method m{item, parts[0] == "nvidia" ? xrfg::D3D12OpticalFlowBackend::nvidia
                                                : xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                     parts[2] == "full"            ? xrfg::D3D12OpticalFlowInputScale::full
                     : parts[2] == "three_quarter" ? xrfg::D3D12OpticalFlowInputScale::three_quarter
                                                   : xrfg::D3D12OpticalFlowInputScale::half,
                     parts[0] == "vectors" || parts[0] == "hybrid"};
            m.hybrid = parts[0] == "hybrid";
            // extrapolate_flow extrapolates from FidelityFX's flow, without
            // guides, and extrapolate_hybrid from both.
            m.extrapolate = parts[0] == "extrapolate" || parts[0] == "extrapolate_flow" ||
                            parts[0] == "extrapolate_hybrid";
            m.extrapolate_hybrid = parts[0] == "extrapolate_hybrid";
            m.guides = m.guides || parts[0] == "extrapolate" || parts[0] == "extrapolate_hybrid";
            m.preset = parts[1] == "slow"   ? xrfg::D3D12NvidiaPerformancePreset::slow
                       : parts[1] == "fast" ? xrfg::D3D12NvidiaPerformancePreset::fast
                                            : xrfg::D3D12NvidiaPerformancePreset::medium;
            m.bidirectional = parts.size() > 3 && parts[3] == "bi";
            methods.push_back(m);
        }
    }
    // XRFG_TEST_REPLAY_NATIVE_ONLY=1 skips OFXR's methods;
    // XRFG_TEST_REPLAY_NATIVE_SCALES lists native resolutions (100,67).
    const bool native_only = std::getenv("XRFG_TEST_REPLAY_NATIVE_ONLY") != nullptr;
    for (const auto& m : methods) {
        if (native_only) break;
        // XRFG_TEST_REPLAY_VECTORS_ONLY=1 runs only OFXR + DLSS vectors.
        if (std::getenv("XRFG_TEST_REPLAY_VECTORS_ONLY") && !m.guides) continue;
        auto a = upload_dumped(fixture, f0.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto b = upload_dumped(fixture, f2.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto blank = [&] {
            return create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         static_cast<UINT16>(slices), static_cast<DXGI_FORMAT>(f0.colour.format));
        };
        std::array<ComPtr<ID3D12Resource>, 2> currents{blank(), blank()};
        auto synthetic = blank();
        std::array<ID3D12Resource*, 2> sources{a.Get(), b.Get()};
        std::array<ID3D12Resource*, 2> current_pointers{currents[0].Get(), currents[1].Get()};
        std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic.Get()};
        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(), sources,
                    D3D12_RESOURCE_STATE_RENDER_TARGET)), "replay history");
        xrfg::D3D12FrameSynthesizer synthesizer;
        xrfg::D3D12NvidiaOpticalFlowOptions options;
        options.input_scale = m.scale;
        options.preset = m.preset;
        options.bidirectional = m.bidirectional;
        options.hybrid = m.hybrid;
        options.extrapolate = m.extrapolate;
        options.extrapolate_hybrid = m.extrapolate_hybrid;
        require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(), history,
                    current_pointers, synthetic_pointers, f0.view_format,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, m.backend, options, true)), "replay synthesizer");
        xrfg::D3D12HistoryCaptureTicket capture_a{}, capture_b{};
        require(operation_succeeded(history->capture(0, &capture_a)) &&
                    operation_succeeded(history->commit(capture_a)), "replay capture A");
        xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
        require(operation_succeeded(synthesizer.submit_prime(capture_a, views0, 0, &prime,
                    m.guides ? guides_a : nullptr)), "replay prime");
        require_frame_start_gate(synthesizer, "replay frame-start gate");
        require(operation_succeeded(history->capture(1, &capture_b)) &&
                    operation_succeeded(history->commit(capture_b)), "replay capture B");
        require(operation_succeeded(synthesizer.submit_pair(capture_b, views2, views1, 0, 1, &pair,
                    std::nullopt, m.guides ? guides_b : nullptr, false, fraction)), "replay pair");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
        // The pair's GPU span on real content: one cold pair, for comparing
        // configurations rather than as a benchmark.
        xrfg::D3D12NvidiaGpuTiming timing{};
        if (synthesizer.consume_nvidia_gpu_timing(&timing) == S_OK) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const double microseconds = timing.gpu_end_qpc > timing.gpu_begin_qpc
                ? double(timing.gpu_end_qpc - timing.gpu_begin_qpc) * 1e6 / double(frequency.QuadPart)
                : double(timing.total_microseconds);
            std::cout << "replay " << m.name << " gpu_us=" << microseconds << std::endl;
        }
        score(m.name.c_str(), read_back_texture(fixture, synthetic.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET));
        require(operation_succeeded(synthesizer.wait_for_idle()), "replay drain");
    }
#ifdef XRFG_NATIVE_DLSSG
    std::vector<std::string> native_scales{"100", "67"};
    if (const char* list = std::getenv("XRFG_TEST_REPLAY_NATIVE_SCALES")) {
        native_scales.clear();
        std::stringstream stream(list);
        for (std::string item; std::getline(stream, item, ',');) native_scales.push_back(item);
    }
    if (extrapolate || std::getenv("XRFG_TEST_REPLAY_VECTORS_ONLY") ||
        (std::getenv("XRFG_TEST_REPLAY_FLOWS") && !std::getenv("XRFG_TEST_REPLAY_NATIVE_SCALES")))
        native_scales.clear();
    for (const auto& scale_text : native_scales) {
        const char* scale = scale_text.c_str();
        SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", scale);
        auto a = upload_dumped(fixture, f0.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto b = upload_dumped(fixture, f2.colour, colour_flags, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto output = create_source_texture(fixture, width, height, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                            static_cast<UINT16>(slices), static_cast<DXGI_FORMAT>(f0.colour.format));
        const std::array<xrfg::D3D12NativeDlssG::Output, 1> outputs{{{output.Get()}}};
        xrfg::D3D12NativeDlssG native;
        require_hresult(native.initialize(fixture.device(), fixture.queue(), a->GetDesc(), f0.view_format),
                        "replay native initialization");
        auto seed_a = make_guides(f0, 1, 1.0F, nullptr);
        auto seed_b = make_guides(f0, 2, 1.0F, nullptr);
        auto moved = make_guides(f2_guides, 3, vector_scale, &f0);
        HRESULT seeded = E_FAIL, generated = E_FAIL;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            seeded = native.record(list, 0, a.Get(), a.Get(), views0, views0, seed_a.get(), seed_b.get(),
                                   outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            generated = native.record(list, 0, a.Get(), b.Get(), views0, views2, seed_b.get(), moved.get(),
                                      outputs, D3D12_RESOURCE_STATE_RENDER_TARGET);
        });
        std::cout << "replay native " << scale << "% seeded=" << seeded << " generated=" << generated
                  << " skip=" << int(native.last_skip()) << '\n';
        if (generated == S_OK)
            score((std::string("native DLSS FG ") + scale + "%").c_str(),
                  read_back_texture(fixture, output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET));
    }
    SetEnvironmentVariableA("XRFG_NATIVE_DLSSG_SCALE", nullptr);
#endif
}
