// Records a replay set (see NrReplayFormat.h) from the NR pass of the running game.
//
// The pass calls this around each of its compute dispatches while a capture is running. Before a dispatch the recorder
// copies every resource the dispatch reads into a readback buffer, and after it the resources the dispatch wrote; nothing is
// mapped until the GPU is certainly past those copies, a few frames later, when Write() turns the buffers into files.
// Nothing is recorded, and nothing is copied, when no capture is running.
//
// Two measures keep the size in check. A resource that has not been written since it was last copied is not copied again
// (the pass tells the recorder about writes through NoteWrite, which it already knows from the barriers it records), and
// Write() stores an image once however often it was copied, by content.
//
// Nothing is left out silently. A dispatch or a boundary picture that cannot be recorded (a resource that is not a plain
// texture, a read that cannot be taken, the memory cap) is counted, the pass's dispatch index still advances so the indices of
// the frame show the gap, and the manifest says how many dispatches each frame had and how many were left out. A replay of such
// a set fails.
//
// States: the shader views the pass binds are read in NON_PIXEL_SHADER_RESOURCE and the unordered views are in
// UNORDERED_ACCESS; the recorder transitions to COPY_SOURCE and back on those assumptions. A resource the pass does not
// own and cannot know the state of (the game's exposure texture) is passed as not readable and is not copied.

#pragma once

#include "ext/nr/diag/NrReplayCompare.h"
#include "ext/nr/diag/NrReplayD3D.h"

#include <array>
#include <tuple>
#include <unordered_map>

namespace ext::nr::replay
{
// Frames per capture. Two consecutive frames: the passes are temporal, so one frame alone would hide what carries over.
inline constexpr uint32_t kReplayFrames = 2;
// Frames to let the GPU run before the buffers are mapped. The pass has no fence of its own here.
inline constexpr uint32_t kReadbackDelayFrames = 8;

struct BindingDesc
{
    bool uav = false;
    uint32_t slot = 0;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    bool readable = true; // false: the state of the resource is not known, so its contents are not recorded
};

struct DispatchDesc
{
    const char* kind = "";
    const char* shader = "";
    uint64_t shaderHash = 0;
    uint32_t groups[3] = { 1, 1, 1 };
    const void* constants = nullptr; // kConstantBytes bytes as bound
    BindingDesc bindings[kSrvSlots + kUavSlots];
    uint32_t bindingCount = 0;
};

struct WriteResult
{
    bool ok = false;          // the set was written completely; when false nothing usable is left on disk and error says why
    std::string error;
    std::string directory;
    uint32_t images = 0;      // distinct images written
    uint32_t dispatches = 0;  // dispatches in the manifest
    uint32_t skipped = 0;     // dispatches the pass made that are not in the set
    uint32_t boundariesSkipped = 0; // boundary pictures that are not in the set
    uint64_t bytes = 0;       // bytes of image data written
    uint32_t copies = 0;      // buffers that were copied during the capture (before sharing by content)
};

// The name a dispatch is recorded under: the pass's mode for the main shader, the stage for the other shaders.
inline std::string KindName(const std::string& shader, uint32_t mode)
{
    if (shader == "guide_match")
        return "guide_match";
    if (shader == "spatial" || shader == "spatial_guides")
    {
        switch (mode)
        {
        case 100: return "spatial_pack";
        case 101: return "spatial_guides";
        case 102: return "spatial_unpack";
        default: return "spatial_" + std::to_string(mode);
        }
    }

    // The Classic-sharpen dispatch runs its own shader (not a mode of the main one), so the
    // shader id alone decides the kind.
    if (shader == "classic_sharpen")
        return "classic_sharpen";
    switch (mode)
    {
    case 0: return "encode";
    case 1: return "resolve";
    case 2: return "downsample";
    case 3: return "meter";
    case 4: return "calibrate";
    case 5: return "encode_residual";
    case 6: return "apply_residual";
    case 7: return "unit_exposure";
    case 8: return "normalize_motion";
    case 9: return "compose_motion";
    case 10: return "apply_interpolated_residual";
    case 11: return "zero_motion";
    case 12: return "bound_residual";
    case 13: return "feedback";
    case 14: return "resize_guides";
    default: return "mode_" + std::to_string(mode);
    }
}

class Recorder
{
  public:
    // Asks for a capture of the next frames. Ignored while one is running or waiting to be written.
    bool Request(uint32_t frames = kReplayFrames)
    {
        if (requested_ || writePending_)
            return false;
        wanted_ = frames == 0 ? 1 : (frames > kMaxFrames ? kMaxFrames : frames);
        recorded_ = 0;
        requested_ = true;
        return true;
    }

    bool Requested() const { return requested_; }
    bool Recording() const { return recording_; }

    // A dispatch the pass gave up on before it reached the recorder (no descriptor slot, no constants buffer...): while
    // recording it still takes an index and counts as left out, so the set shows the gap instead of looking complete.
    void DispatchDropped()
    {
        if (recording_)
            LeaveOut();
    }
    bool WritePending() const { return writePending_; }
    uint32_t FramesRecorded() const { return recorded_; }
    size_t ImageCount() const { return images_.size(); }
    size_t DispatchCount() const { return dispatches_.size(); }
    size_t BoundaryCount() const { return boundaries_.size(); }

    // Called by the scene pass at the start of a frame. True when this frame is being recorded.
    bool BeginFrame(bool keyEnabled)
    {
        recording_ = false;
        if (!ReplayRecordingWanted(keyEnabled, requested_, recorded_, wanted_))
            return false;
        recording_ = true;
        dispatchIndex_ = 0;
        frameSkipped_ = 0;
        frameBoundariesSkipped_ = 0;
        pendingOpen_ = false;
        cache_.clear();
        return true;
    }

    // Called at the end of a recorded frame, with the pass's running frame count.
    void EndFrame(uint64_t frameCounter)
    {
        if (!recording_)
            return;
        recording_ = false;
        pendingOpen_ = false;
        FrameInfo info;
        info.frame = recorded_;
        info.dispatches = dispatchIndex_;
        info.skipped = frameSkipped_;
        info.boundariesSkipped = frameBoundariesSkipped_;
        frameInfos_.push_back(info);
        ++recorded_;
        if (recorded_ >= wanted_)
        {
            requested_ = false;
            writePending_ = true;
            readyAtFrame_ = frameCounter + kReadbackDelayFrames;
        }
    }

    bool ReadyToWrite(uint64_t frameCounter) const { return writePending_ && frameCounter >= readyAtFrame_; }

    // The resource was, or may have been, written: its next read must copy it again.
    void NoteWrite(ID3D12Resource* resource)
    {
        if (recording_ && resource != nullptr)
            cache_.erase(resource);
    }

    // Records the copies of everything the dispatch reads. False when the dispatch cannot be recorded (the caller then does
    // nothing more for it). While a frame is being recorded every false is counted as a dispatch left out and the dispatch
    // index advances; outside a recording nothing is counted.
    bool BeforeDispatch(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const DispatchDesc& desc)
    {
        pendingOpen_ = false;
        if (!recording_)
            return false;
        if (cmd == nullptr || device == nullptr || desc.constants == nullptr || desc.bindingCount == 0)
            return LeaveOut();

        PendingDispatch d;
        d.frame = recorded_;
        d.index = dispatchIndex_;
        d.kind = desc.kind;
        d.shader = desc.shader;
        d.shaderHash = desc.shaderHash;
        std::memcpy(d.groups, desc.groups, sizeof(d.groups));
        std::memcpy(d.constants.data(), desc.constants, kConstantBytes);

        std::vector<ID3D12Resource*> order;
        std::map<ID3D12Resource*, bool> written, readIn;
        for (uint32_t i = 0; i < desc.bindingCount; ++i)
        {
            const BindingDesc& b = desc.bindings[i];
            if (b.resource == nullptr)
                return LeaveOut();
            if (written.find(b.resource) == written.end())
            {
                order.push_back(b.resource);
                written[b.resource] = false;
                readIn[b.resource] = false;
            }
            if (b.uav)
                written[b.resource] = true;
            else if (b.readable)
                readIn[b.resource] = true;
        }
        // a resource bound for reading and writing in one dispatch is not one the passes ever do
        for (uint32_t i = 0; i < desc.bindingCount; ++i)
            if (!desc.bindings[i].uav && written[desc.bindings[i].resource])
                return LeaveOut();

        for (ID3D12Resource* res : order)
        {
            const D3D12_RESOURCE_DESC rd = res->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                return LeaveOut();
            PendingResource r;
            r.local = static_cast<uint32_t>(d.resources.size());
            r.width = static_cast<uint32_t>(rd.Width);
            r.height = rd.Height;
            r.resourceFormat = rd.Format;
            r.copyFormat = CopyFormatFor(rd.Format);
            r.mips = rd.MipLevels;
            r.arraySize = rd.DepthOrArraySize;
            r.resource = res;
            r.written = written[res];
            if (readIn[res])
            {
                r.before = Acquire(cmd, device, res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (r.before < 0)
                    return LeaveOut(); // a read that cannot be taken would make the replay wrong, so the dispatch is left out
            }
            d.resources.push_back(r);
        }
        for (uint32_t i = 0; i < desc.bindingCount; ++i)
        {
            const BindingDesc& b = desc.bindings[i];
            Binding x;
            x.uav = b.uav;
            x.slot = b.slot;
            x.viewFormat = b.viewFormat;
            x.state = b.uav ? kStateUnorderedAccess : kStateNonPixelShaderResource;
            for (const PendingResource& r : d.resources)
                if (r.resource == b.resource)
                    x.res = r.local;
            const D3D12_RESOURCE_DESC rd = b.resource->GetDesc();
            x.mips = b.uav ? 1 : rd.MipLevels;
            x.array = rd.DepthOrArraySize > 1;
            x.slices = rd.DepthOrArraySize;
            d.bindings.push_back(x);
        }
        current_ = std::move(d);
        pendingOpen_ = true;
        return true;
    }

    // Records the copies of what the dispatch wrote, right after it. A no-op when BeforeDispatch did not record.
    void AfterDispatch(ID3D12GraphicsCommandList* cmd, ID3D12Device* device)
    {
        if (!recording_ || !pendingOpen_)
            return;
        pendingOpen_ = false;
        bool complete = true;
        for (PendingResource& r : current_.resources)
        {
            if (!r.written)
                continue;
            cache_.erase(r.resource); // its contents just changed
            r.after = Acquire(cmd, device, r.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (r.after < 0)
                complete = false;
        }
        if (complete)
        {
            dispatches_.push_back(std::move(current_));
            ++dispatchIndex_;
        }
        else
            LeaveOut();
        current_ = {};
    }

    // A named picture at a boundary of the pass; the resource is in the given state.
    void AddBoundary(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const char* name, ID3D12Resource* resource,
                     D3D12_RESOURCE_STATES state)
    {
        if (!recording_)
            return;
        const int image = resource != nullptr && cmd != nullptr && device != nullptr ? Acquire(cmd, device, resource, state) : -1;
        if (image < 0)
        {
            ++frameBoundariesSkipped_; // the picture is not in the set, and the manifest says so
            return;
        }
        PendingBoundary b;
        b.frame = recorded_;
        b.name = name;
        b.image = image;
        b.resourceFormat = resource->GetDesc().Format;
        boundaries_.push_back(b);
    }

    // Maps everything, writes the files and frees the buffers. The GPU must be past the copies. The result says whether the set
    // is complete on disk: a folder that cannot be made, an image or the manifest that cannot be written is an error, the half
    // written folder is removed, and no manifest is left behind to make it look like a set. Always frees everything.
    WriteResult Write(const std::filesystem::path& directory)
    {
        WriteResult out;
        if (!writePending_)
        {
            out.error = "no capture is waiting to be written";
            return out;
        }
        struct Frees
        {
            Recorder& r;
            ~Frees() { r.Release(); }
        } frees { *this };

        std::error_code ec;
        std::filesystem::create_directories(directory / "images", ec);
        std::error_code isDir;
        if (ec || !std::filesystem::is_directory(directory / "images", isDir))
        {
            out.error = "cannot create " + (directory / "images").string() + (ec ? ": " + ec.message() : std::string());
            return out;
        }

        Manifest m;
        m.frames = recorded_;
        m.frameInfos = frameInfos_;
        out.copies = static_cast<uint32_t>(images_.size());

        struct Key
        {
            uint64_t hash;
            uint32_t w, h, format, rowBytes;
            bool operator<(const Key& o) const
            {
                return std::tie(hash, w, h, format, rowBytes) < std::tie(o.hash, o.w, o.h, o.format, o.rowBytes);
            }
        };
        std::map<Key, uint32_t> byContent;
        std::vector<int64_t> imageId(images_.size(), -1); // pending index -> manifest id, -1 if it could not be read
        bool failed = false;
        std::vector<std::filesystem::path> ownFiles; // what this call opened for writing, the only files a failure removes

        for (size_t i = 0; i < images_.size(); ++i)
        {
            PendingImage& p = images_[i];
            void* raw = nullptr;
            D3D12_RANGE range { 0, static_cast<SIZE_T>(p.fp.total) };
            if (!failed && p.readback && SUCCEEDED(p.readback->Map(0, &range, &raw)) && raw != nullptr)
            {
                const uint8_t* mapped = static_cast<const uint8_t*>(raw);
                const uint32_t rowBytes = static_cast<uint32_t>(p.fp.rowBytes);
                const uint8_t* base = mapped + p.fp.layout.Offset;
                const uint32_t pitch = p.fp.layout.Footprint.RowPitch;
                const uint64_t hash = HashRows(base, pitch, rowBytes, p.fp.rows);
                const Key key { hash, p.width, p.height, p.fp.copyFormat, rowBytes };
                const auto found = byContent.find(key);
                if (found != byContent.end())
                    imageId[i] = found->second;
                else
                {
                    Image img;
                    img.id = static_cast<uint32_t>(m.images.size());
                    char name[48];
                    std::snprintf(name, sizeof(name), "images/img_%04u.raw", img.id);
                    img.file = name;
                    img.width = p.width;
                    img.height = p.height;
                    img.format = p.fp.copyFormat;
                    img.srcRowPitch = pitch;
                    img.rowBytes = rowBytes;
                    img.bytes = static_cast<uint64_t>(rowBytes) * p.fp.rows;
                    img.hash = hash;
                    const std::vector<uint8_t> tight = PackRows(base, pitch, rowBytes, p.fp.rows);
                    std::ofstream f(directory / img.file, std::ios::binary | std::ios::trunc);
                    if (f.is_open())
                        ownFiles.push_back(directory / img.file);
                    f.write(reinterpret_cast<const char*>(tight.data()), static_cast<std::streamsize>(tight.size()));
                    f.close();
                    if (f.fail())
                    {
                        failed = true;
                        out.error = "cannot write " + (directory / img.file).string();
                    }
                    else
                    {
                        out.bytes += tight.size();
                        byContent[key] = img.id;
                        imageId[i] = img.id;
                        m.images.push_back(img);
                    }
                }
                D3D12_RANGE none { 0, 0 };
                p.readback->Unmap(0, &none);
            }
            p.readback.Reset(); // the memory goes back before the next image is read
        }

        if (!failed)
        {
            for (PendingDispatch& pd : dispatches_)
            {
                Dispatch d;
                d.frame = pd.frame;
                d.index = pd.index;
                d.kind = pd.kind;
                d.shader = pd.shader;
                d.shaderHash = pd.shaderHash;
                std::memcpy(d.groups, pd.groups, sizeof(d.groups));
                d.constants = pd.constants;
                d.bindings = pd.bindings;
                bool usable = true;
                for (const PendingResource& r : pd.resources)
                {
                    Resource x;
                    x.local = r.local;
                    x.width = r.width;
                    x.height = r.height;
                    x.resourceFormat = r.resourceFormat;
                    x.copyFormat = r.copyFormat;
                    x.mips = r.mips;
                    x.arraySize = r.arraySize;
                    if (r.before >= 0)
                    {
                        if (imageId[static_cast<size_t>(r.before)] < 0)
                            usable = false;
                        else
                            x.before = static_cast<uint32_t>(imageId[static_cast<size_t>(r.before)]);
                    }
                    if (r.after >= 0)
                    {
                        if (imageId[static_cast<size_t>(r.after)] < 0)
                            usable = false;
                        else
                            x.after = static_cast<uint32_t>(imageId[static_cast<size_t>(r.after)]);
                    }
                    d.resources.push_back(x);
                }
                if (usable)
                    m.dispatches.push_back(std::move(d));
                else if (pd.frame < m.frameInfos.size())
                    ++m.frameInfos[pd.frame].skipped; // its index is now missing from the frame, and the frame says why
            }

            for (const PendingBoundary& b : boundaries_)
            {
                if (imageId[static_cast<size_t>(b.image)] < 0)
                {
                    if (b.frame < m.frameInfos.size())
                        ++m.frameInfos[b.frame].boundariesSkipped;
                    continue;
                }
                Boundary x;
                x.frame = b.frame;
                x.name = b.name;
                x.image = static_cast<uint32_t>(imageId[static_cast<size_t>(b.image)]);
                x.resourceFormat = b.resourceFormat;
                m.boundaries.push_back(x);
            }

            const std::string text = WriteManifest(m);
            std::ofstream f(directory / "manifest.txt", std::ios::binary | std::ios::trunc);
            if (f.is_open())
                ownFiles.push_back(directory / "manifest.txt");
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            f.close();
            if (f.fail())
            {
                failed = true;
                out.error = "cannot write " + (directory / "manifest.txt").string();
            }
        }

        if (failed)
        {
            // No half written set stays behind, and in particular no manifest. Only the files this call opened are removed,
            // then the images folder and the set folder if that left them empty: whatever was in the folder before stays.
            std::error_code removed;
            for (const std::filesystem::path& file : ownFiles)
                std::filesystem::remove(file, removed);
            std::filesystem::remove(directory / "images", removed); // only removes an empty folder
            std::filesystem::remove(directory, removed);
            return out;
        }

        for (const FrameInfo& f : m.frameInfos)
        {
            out.skipped += f.skipped;
            out.boundariesSkipped += f.boundariesSkipped;
        }
        out.ok = true;
        out.directory = directory.string();
        out.images = static_cast<uint32_t>(m.images.size());
        out.dispatches = static_cast<uint32_t>(m.dispatches.size());
        return out;
    }

    // The most the capture may hold in readback buffers at once; a capture that would go past it stops copying and the
    // dispatches after that point are left out (and counted). Settable so a test does not need three gigabytes.
    void SetMaxPendingBytes(uint64_t bytes) { maxPendingBytes_ = bytes; }

    // Drops everything, recorded or pending.
    void Release()
    {
        images_.clear();
        dispatches_.clear();
        boundaries_.clear();
        cache_.clear();
        current_ = {};
        pendingOpen_ = false;
        recording_ = false;
        requested_ = false;
        writePending_ = false;
        recorded_ = 0;
        dispatchIndex_ = 0;
        frameSkipped_ = 0;
        frameBoundariesSkipped_ = 0;
        frameInfos_.clear();
        pendingBytes_ = 0;
    }

  private:
    // About 46 pictures of 3840x2160 half-float (66 MB each).
    static constexpr uint64_t kDefaultMaxPendingBytes = 3ull << 30;
    static constexpr uint32_t kMaxFrames = 4;

    // One dispatch the pass made that is not in the set: counted, and the index moves on so the gap shows.
    bool LeaveOut()
    {
        ++frameSkipped_;
        ++dispatchIndex_;
        pendingOpen_ = false;
        return false;
    }

    struct PendingImage
    {
        ComPtr<ID3D12Resource> readback;
        Footprint fp;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    struct PendingResource
    {
        uint32_t local = 0, width = 0, height = 0, resourceFormat = 0, copyFormat = 0, mips = 1, arraySize = 1;
        ID3D12Resource* resource = nullptr;
        bool written = false;
        int before = -1;
        int after = -1;
    };
    struct PendingDispatch
    {
        uint32_t frame = 0, index = 0;
        std::string kind, shader;
        uint64_t shaderHash = 0;
        uint32_t groups[3] = { 1, 1, 1 };
        std::array<uint8_t, kConstantBytes> constants {};
        std::vector<PendingResource> resources;
        std::vector<Binding> bindings;
    };
    struct PendingBoundary
    {
        uint32_t frame = 0;
        std::string name;
        int image = -1;
        uint32_t resourceFormat = 0;
    };

    // Copies the resource out (or reuses its latest copy) and returns the pending image index, -1 if it cannot be copied.
    int Acquire(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
    {
        const auto hit = cache_.find(resource);
        if (hit != cache_.end())
            return hit->second;

        const D3D12_RESOURCE_DESC rd = resource->GetDesc();
        PendingImage p;
        if (!QueryFootprint(device, rd, p.fp) || pendingBytes_ + p.fp.total > maxPendingBytes_)
            return -1;
        p.readback = CreateBuffer(device, p.fp.total, D3D12_HEAP_TYPE_READBACK);
        if (!p.readback)
            return -1;
        p.width = static_cast<uint32_t>(rd.Width);
        p.height = rd.Height;

        Transition(cmd, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyOut(cmd, resource, p.readback.Get(), p.fp);
        Transition(cmd, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

        pendingBytes_ += p.fp.total;
        images_.push_back(std::move(p));
        const int index = static_cast<int>(images_.size() - 1);
        cache_[resource] = index;
        return index;
    }

    std::vector<PendingImage> images_;
    std::vector<PendingDispatch> dispatches_;
    std::vector<PendingBoundary> boundaries_;
    std::unordered_map<ID3D12Resource*, int> cache_; // resource -> latest pending image, until it is written
    PendingDispatch current_;
    bool pendingOpen_ = false;
    bool recording_ = false;
    bool requested_ = false;
    bool writePending_ = false;
    uint32_t wanted_ = 0;
    uint32_t recorded_ = 0;
    uint32_t dispatchIndex_ = 0;       // dispatches the pass has made in the frame being recorded, recorded or not
    uint32_t frameSkipped_ = 0;        // of those, the ones left out
    uint32_t frameBoundariesSkipped_ = 0;
    std::vector<FrameInfo> frameInfos_; // one per frame that was recorded
    uint64_t readyAtFrame_ = 0;
    uint64_t pendingBytes_ = 0;
    uint64_t maxPendingBytes_ = kDefaultMaxPendingBytes;
};
} // namespace ext::nr::replay
