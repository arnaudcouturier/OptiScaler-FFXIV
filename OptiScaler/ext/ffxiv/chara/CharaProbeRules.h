#pragma once
// Pure rules of the character preview probe (CharaProbe.cpp): which textures count as "derived from the preview", how
// often the probe may write a log line, the output budget, the live command parser and a small PNG writer.
// Nothing here touches D3D, COM or the file system, so the CPU tests cover all of it.
//
// The probe is off unless [FFXIV] PreviewProbe is on. Times are milliseconds on any monotonic clock the caller picks.

#include "ext/core/Crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ext::ffxiv::chara::probe
{
// ---------------------------------------------------------------------------------------------------------------------
// Derived textures: textures that hold (a copy of) the preview image
// ---------------------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kMaxHops = 3;            // copy chain length: preview -> A (1) -> B (2) -> C (3)
inline constexpr uint64_t kExpiryMs = 3000;        // an entry that was not refreshed for this long is forgotten
inline constexpr int kPreviewEntries = 8;          // table entries 0..7 are the preview outputs

enum class DerivedKind : uint8_t
{
    Copy, // destination of CopyResource / CopySubresourceRegion / ResolveSubresource
    Draw, // render target of a draw that sampled the preview or another derived texture
};

struct TextureInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // DXGI_FORMAT value
};

struct Derived
{
    uintptr_t ptr = 0;
    TextureInfo info;
    int sourceEntry = -1;   // the preview table entry (0..7) the image came from
    uint32_t hops = 0;      // 1 = copied straight from the preview
    DerivedKind kind = DerivedKind::Copy;
    uint64_t firstSeen = 0;
    uint64_t lastSeen = 0;
    bool used = false;
    // A reference to the texture itself (kept for at most kExpiryMs after the last sighting, 16 entries at most): while it lives the
    // address cannot be handed to another texture, so the entry cannot be mistaken for a recycled address. Empty for a swapchain buffer.
    std::shared_ptr<void> hold;
};

// The 8-bit, 16-bit float and 10-bit formats a texture and its copies commonly use come in a typeless and a typed (or sRGB) flavour:
// "the same format" for the purpose of following a copy means the same family. DXGI_FORMAT values: 9 R16G16B16A16_TYPELESS, 10 _FLOAT,
// 23 R10G10B10A2_TYPELESS, 24 _UNORM, 27 R8G8B8A8_TYPELESS, 28 _UNORM, 29 _UNORM_SRGB, 87 B8G8R8A8_UNORM, 90 _TYPELESS, 91 _UNORM_SRGB.
constexpr uint32_t FormatFamily(uint32_t format)
{
    switch (format)
    {
    case 9:
    case 10: return 10;
    case 23:
    case 24: return 24;
    case 27:
    case 28:
    case 29: return 28;
    case 87:
    case 90:
    case 91: return 87;
    default: return format;
    }
}

constexpr bool SameFormatFamily(uint32_t a, uint32_t b) { return FormatFamily(a) == FormatFamily(b); }

// How a copy reached its destination.
enum class CopyVia : uint8_t
{
    CopyResource,
    CopyRegion,
    Resolve,
};

struct CopyFacts
{
    CopyVia via = CopyVia::CopyResource;
    uint32_t destSub = 0;
    uint32_t x = 0, y = 0, z = 0;
    uint32_t sourceSub = 0;
    bool hasBox = false;          // a source box was given
    uint32_t boxLeft = 0, boxTop = 0, boxFront = 0, boxRight = 0, boxBottom = 0, boxBack = 0; // that box (D3D11_BOX); zeros when none
    TextureInfo source;           // the source texture's size and format
    TextureInfo dest;             // the destination texture's size and format
    uint32_t resolveFormat = 0;   // ResolveSubresource: the format argument
};

// A copy is followed only when it puts the WHOLE image into a texture of its own: subresource 0 to subresource 0, the whole of it, into a
// destination of the same width, height and format family. A region copied into an atlas, a mip or an array slice would make the whole
// destination "preview-derived" and with the flood test on every draw that samples the rest of the atlas would turn magenta. (Those
// copies are still logged, as "partial copy, not followed": they are the interesting case for the maintainer.) An explicit source box that
// covers the whole source ({0, 0, 0, width, height, 1}) is a whole copy like no box at all.
constexpr bool WholeImageCopy(const CopyFacts& f)
{
    if (f.source.width == 0 || f.source.height == 0 || f.source.width != f.dest.width || f.source.height != f.dest.height)
        return false;
    switch (f.via)
    {
    case CopyVia::CopyResource: return SameFormatFamily(f.source.format, f.dest.format);
    case CopyVia::CopyRegion:
    {
        const bool boxIsWhole = !f.hasBox || (f.boxLeft == 0 && f.boxTop == 0 && f.boxFront == 0 && f.boxRight == f.source.width &&
                                              f.boxBottom == f.source.height && f.boxBack == 1);
        return f.destSub == 0 && f.sourceSub == 0 && f.x == 0 && f.y == 0 && f.z == 0 && boxIsWhole &&
               SameFormatFamily(f.source.format, f.dest.format);
    }
    case CopyVia::Resolve:
        return f.destSub == 0 && f.sourceSub == 0 &&
               (SameFormatFamily(f.source.format, f.dest.format) || SameFormatFamily(f.resolveFormat, f.dest.format));
    }
    return false;
}

// Whether a render target of a draw that sampled the preview (or a copy of it) may become "derived".
struct PromotionFacts
{
    bool rendererRecent = false;      // the preview renderer was seen active lately (a derived chain only lives while it renders)
    bool haveTarget = false;          // the draw has a 2D render target
    bool targetIsBackbuffer = false;
    bool targetIsTableEntry = false;  // one of the preview renderer's own textures
    bool pixelShaderBound = false;    // a draw without a pixel shader reads nothing: its bindings are stale ones
    TextureInfo target;
    bool backbufferKnown = false;
    TextureInfo backbuffer;           // the swapchain's size (when known)
    uint32_t viewportWidth = 0;       // the draw's viewport, the reference when the backbuffer is not known
    uint32_t viewportHeight = 0;
};

enum class Promotion : uint8_t
{
    Promote,
    RendererIdle,             // the renderer is not active
    NoTarget,
    IsBackbuffer,
    IsTableEntry,
    NoPixelShader,            // "bound, not known to be read": the draw has no pixel shader
    ScreenSizeIntermediate,   // at least half the screen's area: not followed (it would turn the whole screen into "preview")
    NoReference,              // neither the backbuffer nor the viewport is known: cannot tell whether it is screen-sized
};

// The reference size is the backbuffer's own (a 1080p or 1440p game is judged by its own screen, not by a 4K constant); when it is not
// known, the draw's viewport. A target of at least half of that area is not followed as a draw-derived texture unless it is an explicit
// copy destination (the copy path does not use this rule).
constexpr Promotion DecidePromotion(const PromotionFacts& f)
{
    if (!f.rendererRecent)
        return Promotion::RendererIdle;
    if (!f.haveTarget)
        return Promotion::NoTarget;
    if (f.targetIsBackbuffer)
        return Promotion::IsBackbuffer;
    if (f.targetIsTableEntry)
        return Promotion::IsTableEntry;
    if (!f.pixelShaderBound)
        return Promotion::NoPixelShader;
    const uint64_t reference = f.backbufferKnown ? static_cast<uint64_t>(f.backbuffer.width) * f.backbuffer.height
                                                 : static_cast<uint64_t>(f.viewportWidth) * f.viewportHeight;
    if (reference == 0)
        return Promotion::NoReference;
    const uint64_t area = static_cast<uint64_t>(f.target.width) * f.target.height;
    return area * 2 >= reference ? Promotion::ScreenSizeIntermediate : Promotion::Promote;
}

// The target is a screen-size intermediate by the same rule (for the annotation of a copy line).
constexpr bool IsScreenSizeIntermediate(const TextureInfo& target, const TextureInfo& backbuffer)
{
    const uint64_t reference = static_cast<uint64_t>(backbuffer.width) * backbuffer.height;
    return reference != 0 && static_cast<uint64_t>(target.width) * target.height * 2 >= reference;
}

enum class DeriveResult : uint8_t
{
    Added,         // new entry
    Refreshed,     // the destination was already derived: seen again
    Replaced,      // new entry, the oldest live entry was dropped because the set was full
    CopiesOff,     // tracking is switched off ("copies off")
    NullPointer,   // source or destination is null
    SameTexture,   // destination == source
    DestIsPreview, // the destination is itself a preview entry
    UnknownSource, // the source is not a preview entry (0..7) and not a live derived texture
    TooManyHops,   // the destination would be more than kMaxHops copies away from the preview
};

constexpr bool Accepted(DeriveResult r)
{
    return r == DeriveResult::Added || r == DeriveResult::Refreshed || r == DeriveResult::Replaced;
}

class DerivedSet
{
public:
    static constexpr size_t kCapacity = 16;

    void SetCopiesEnabled(bool on) { copies_ = on; }
    bool CopiesEnabled() const { return copies_; }

    // The source is the preview output with table entry `entry` (0..7): the destination is one hop away. `hold` (optional) keeps the
    // destination texture alive while the entry lives (see Derived::hold).
    DeriveResult FromPreview(int entry, uintptr_t source, uintptr_t dest, const TextureInfo& info, bool destIsPreviewEntry,
                             DerivedKind kind, uint64_t now, std::shared_ptr<void> hold = {})
    {
        if (!copies_)
            return DeriveResult::CopiesOff;
        if (source == 0 || dest == 0)
            return DeriveResult::NullPointer;
        if (source == dest)
            return DeriveResult::SameTexture;
        if (destIsPreviewEntry)
            return DeriveResult::DestIsPreview;
        if (entry < 0 || entry >= kPreviewEntries)
            return DeriveResult::UnknownSource;
        return Insert(dest, info, entry, 1, kind, now, std::move(hold));
    }

    // The source is a texture that is already derived (and not expired): the destination is one hop further.
    DeriveResult FromDerived(uintptr_t source, uintptr_t dest, const TextureInfo& info, bool destIsPreviewEntry,
                             DerivedKind kind, uint64_t now, std::shared_ptr<void> hold = {})
    {
        if (!copies_)
            return DeriveResult::CopiesOff;
        if (source == 0 || dest == 0)
            return DeriveResult::NullPointer;
        if (source == dest)
            return DeriveResult::SameTexture;
        if (destIsPreviewEntry)
            return DeriveResult::DestIsPreview;
        const Derived* src = Find(source, now);
        if (!src)
            return DeriveResult::UnknownSource;
        if (src->hops + 1 > kMaxHops)
            return DeriveResult::TooManyHops;
        return Insert(dest, info, src->sourceEntry, src->hops + 1, kind, now, std::move(hold));
    }

    // The live entry of this texture, or null (never stored, or not refreshed for kExpiryMs). An entry that holds a reference to its
    // texture is the texture (its address cannot be reused while the reference lives). An entry without one (a swapchain buffer is
    // never held, it would block ResizeBuffers) is only the same texture when `actual`, if the caller has it, still has the size and
    // format family the entry was made with: a recycled address with another size does not match.
    const Derived* Find(uintptr_t ptr, uint64_t now, const TextureInfo* actual = nullptr) const
    {
        if (ptr == 0)
            return nullptr;
        for (const Derived& d : entries_)
            if (d.used && d.ptr == ptr && Alive(d, now))
                return Matches(d, actual) ? &d : nullptr;
        return nullptr;
    }

    // A draw saw the texture again: keeps it alive. Returns false when it is unknown, expired or no longer the same texture.
    bool Touch(uintptr_t ptr, uint64_t now, const TextureInfo* actual = nullptr)
    {
        for (Derived& d : entries_)
            if (d.used && d.ptr == ptr && Alive(d, now))
            {
                if (!Matches(d, actual))
                    return false;
                d.lastSeen = std::max(d.lastSeen, now);
                return true;
            }
        return false;
    }

    // Lets go of the entries that expired: their texture references are released (called regularly, so the game's textures are not
    // kept alive for longer than kExpiryMs after the last time they were seen).
    void Prune(uint64_t now)
    {
        for (Derived& d : entries_)
            if (d.used && !Alive(d, now))
                d = Derived {};
    }

    size_t Count(uint64_t now) const
    {
        size_t n = 0;
        for (const Derived& d : entries_)
            n += (d.used && Alive(d, now)) ? 1 : 0;
        return n;
    }

    bool Empty(uint64_t now) const { return Count(now) == 0; }

    std::vector<Derived> Live(uint64_t now) const
    {
        std::vector<Derived> out;
        for (const Derived& d : entries_)
            if (d.used && Alive(d, now))
                out.push_back(d);
        return out;
    }

    // Forgets every entry (and releases the references they hold). The copy-tracking switch stays as it is: the `reset` command and a
    // texture change must not undo "copies off".
    void Clear() { entries_ = {}; }
    // A fresh set: no entries and copy tracking on (what switching the probe on or off does).
    void Reset()
    {
        Clear();
        copies_ = true;
    }

private:
    static bool Alive(const Derived& d, uint64_t now) { return now < d.lastSeen || now - d.lastSeen < kExpiryMs; }

    static bool Matches(const Derived& d, const TextureInfo* actual)
    {
        if (d.hold || actual == nullptr)
            return true;
        return d.info.width == actual->width && d.info.height == actual->height && SameFormatFamily(d.info.format, actual->format);
    }

    DeriveResult Insert(uintptr_t dest, const TextureInfo& info, int entry, uint32_t hops, DerivedKind kind, uint64_t now,
                        std::shared_ptr<void> hold)
    {
        // the same texture again (live): keep the shortest chain, refresh the time
        for (Derived& d : entries_)
            if (d.used && d.ptr == dest && Alive(d, now))
            {
                if (hops < d.hops)
                {
                    d.hops = hops;
                    d.sourceEntry = entry;
                }
                d.info = info;
                d.kind = kind;
                d.lastSeen = std::max(d.lastSeen, now);
                if (hold)
                    d.hold = std::move(hold);
                return DeriveResult::Refreshed;
            }

        // a stale record of the same pointer is overwritten (the pointer may be a new texture), then free or expired slots
        Derived* slot = nullptr;
        for (Derived& d : entries_)
            if (d.used && d.ptr == dest)
                slot = &d;
        if (!slot)
            for (Derived& d : entries_)
                if (!d.used || !Alive(d, now))
                {
                    slot = &d;
                    break;
                }
        DeriveResult result = DeriveResult::Added;
        if (!slot)
        {
            slot = &entries_[0];
            for (Derived& d : entries_)
                if (d.lastSeen < slot->lastSeen)
                    slot = &d;
            result = DeriveResult::Replaced;
        }
        slot->ptr = dest;
        slot->info = info;
        slot->sourceEntry = entry;
        slot->hops = hops;
        slot->kind = kind;
        slot->firstSeen = now;
        slot->lastSeen = now;
        slot->used = true;
        slot->hold = std::move(hold); // the previous occupant's reference (if any) is released here
        return result;
    }

    std::array<Derived, kCapacity> entries_ {};
    bool copies_ = true;
};

// ---------------------------------------------------------------------------------------------------------------------
// Log limits
// ---------------------------------------------------------------------------------------------------------------------

// 64-bit key of a case from a few small numbers (FNV-1a over the bytes of each value).
constexpr uint64_t CaseKey(uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t values[5] = { a, b, c, d, e };
    for (uint64_t v : values)
        for (int i = 0; i < 8; ++i)
        {
            h ^= (v >> (i * 8)) & 0xff;
            h *= 1099511628211ull;
        }
    return h;
}

// At most N distinct cases are ever logged; a case is logged only the first time it is seen.
template <size_t N> class CaseLog
{
public:
    // True the first time `key` is offered while fewer than N cases were accepted.
    bool Offer(uint64_t key)
    {
        for (size_t i = 0; i < count_; ++i)
            if (keys_[i] == key)
                return false;
        if (count_ >= N)
        {
            ++refused_;
            return false;
        }
        keys_[count_++] = key;
        return true;
    }

    size_t Count() const { return count_; }
    uint64_t Refused() const { return refused_; } // new cases that were not logged because the cap was reached
    void Clear()
    {
        count_ = 0;
        refused_ = 0;
    }

private:
    std::array<uint64_t, N> keys_ {};
    size_t count_ = 0;
    uint64_t refused_ = 0;
};

// Whether the per-draw scan of the probe / flood test can find anything at all, decided without a single COM call. The scan looks for
// draws that sample a preview output (table entries 0..7) or a followed copy of one.
//   * nothing is on: no;
//   * a followed copy is alive: yes (it may be sampled whatever the renderer does);
//   * a preview output exists in the table (a preview window is open): yes, probe or flood. A preview that stopped re-rendering (a paused
//     window, a stall) is still on screen and its sampler draws must still be found;
//   * otherwise: only while the preview renderer is active (as before) for the probe, never for the flood test alone (nothing exists a
//     draw could sample).
constexpr bool ScanCanFindAnything(bool probeOn, bool floodOn, bool rendererActive, bool followedCopyAlive, bool previewOutputExists)
{
    if (!probeOn && !floodOn)
        return false;
    if (followedCopyAlive || previewOutputExists)
        return true;
    return probeOn && !floodOn ? rendererActive : false;
}

// ---------------------------------------------------------------------------------------------------------------------
// When the image dump is taken
// ---------------------------------------------------------------------------------------------------------------------

// A dump executed at an arbitrary draw can capture the preview between its clear and its finisher. A request is therefore armed when it
// is first seen and executed at the first point where the preview image is complete: right after an accepted finisher draw, or right after
// a consumer swap (all the images of the frame exist then). If no such point comes within kDumpFallbackMs it is executed at any draw. A
// start that the game's state forbids for now (an active predicate) is postponed to the next point, at most kMaxDumpPostponements times.
inline constexpr uint64_t kDumpFallbackMs = 2000;
inline constexpr uint32_t kMaxDumpPostponements = 20;

class DumpTrigger
{
  public:
    enum class Step : uint8_t
    {
        Idle,  // no request is waiting
        Wait,  // armed: waiting for a point where the preview image is complete
        Start, // execute the request now
    };

    // `pending`: a request is waiting. `completePoint`: the preview image is complete right now (not true on the call that arms the
    // request unless the caller says so: a point seen before the request does not count).
    Step Decide(bool pending, bool completePoint, uint64_t now)
    {
        if (!pending)
        {
            _armed = false;
            _postponed = 0;
            return Step::Idle;
        }
        if (!_armed)
        {
            _armed = true;
            _armedAt = now;
        }
        if (completePoint || now < _armedAt || now - _armedAt >= kDumpFallbackMs)
        {
            _armed = false;
            return Step::Start;
        }
        return Step::Wait;
    }

    // The caller could not start the dump now and wants the next point: false when it has been postponed too often (give up).
    bool Postpone(uint64_t now)
    {
        if (++_postponed > kMaxDumpPostponements)
            return false;
        _armed = true;
        _armedAt = now;
        return true;
    }

    bool Armed() const { return _armed; }

  private:
    bool _armed = false;
    uint64_t _armedAt = 0;
    uint32_t _postponed = 0;
};

// The draw-thread side of a waiting dump request: which draws are complete points, and what a postponed start leaves behind. A swap that
// was reported since the request was armed counts as a complete point (every image of that frame exists); the flag is consumed whenever the
// request starts, is postponed or ends, so a postponed request waits for the NEXT complete image instead of retrying at every draw.
class DumpService
{
  public:
    enum class Result : uint8_t
    {
        Nothing,   // no request waits, or it keeps waiting for a complete point
        Postponed, // the start was postponed to the next complete point
        GaveUp,    // postponed too often: the request is over, refused
        Started,   // the start ran (the caller reports its outcome); the request is over
    };

    // A consumer swap was reported: all the images of this frame exist.
    void NoteSwap() { _swapCompleted = true; }

    // `pending`: a request waits. `finisherPoint`: right after an accepted finisher draw. `start` runs the dump and returns true when the
    // game's state forbids it for now (an active predicate), which postpones it.
    template <class StartFn> Result Service(bool pending, bool finisherPoint, uint64_t now, StartFn&& start)
    {
        if (!pending && !_trigger.Armed())
            return Result::Nothing; // the common case: nothing waits
        const bool arming = pending && !_trigger.Armed();
        if (arming)
            _swapCompleted = false; // a swap that happened before the request does not count
        const bool point = finisherPoint || (!arming && _swapCompleted);
        if (_trigger.Decide(pending, point, now) != DumpTrigger::Step::Start)
            return Result::Nothing;
        const bool postponed = start();
        _swapCompleted = false; // consumed by this start, whatever came of it
        if (postponed)
            return _trigger.Postpone(now) ? Result::Postponed : Result::GaveUp;
        return Result::Started;
    }

    bool Armed() const { return _trigger.Armed(); }

  private:
    DumpTrigger _trigger;
    bool _swapCompleted = false;
};

// The facts of a swap are those of the swapped draw only when that draw was the last one scanned (the draw serial advances for every
// dispatched draw while the probe is on, scanned or not).
constexpr bool SameDraw(bool lastDrawValid, uint64_t lastDrawSerial, uint64_t currentSerial)
{
    return lastDrawValid && lastDrawSerial == currentSerial;
}

inline constexpr size_t kCopyCaseCap = 48;
inline constexpr size_t kSamplerCaseCap = 64;

// Events per window of at least one second. Completed windows are queued for the caller to report.
class RateCounter
{
public:
    struct Window
    {
        uint64_t startMs = 0;
        uint64_t durationMs = 0;
        uint32_t count = 0;
    };

    void Add(uint64_t now, uint32_t n = 1)
    {
        Roll(now);
        count_ += n;
        total_ += n;
    }

    // Closes the current window when it is a second old (also without a new event).
    void Roll(uint64_t now)
    {
        if (!started_)
        {
            started_ = true;
            start_ = now;
            return;
        }
        if (now >= start_ && now - start_ >= 1000)
        {
            if (count_ > 0)
            {
                if (queued_ == kQueue)
                {
                    for (size_t i = 1; i < kQueue; ++i)
                        done_[i - 1] = done_[i];
                    --queued_;
                }
                done_[queued_++] = Window { start_, now - start_, count_ };
            }
            start_ = now;
            count_ = 0;
        }
    }

    // Takes the oldest completed window.
    bool Take(Window& out)
    {
        if (queued_ == 0)
            return false;
        out = done_[0];
        for (size_t i = 1; i < queued_; ++i)
            done_[i - 1] = done_[i];
        --queued_;
        return true;
    }

    uint64_t Total() const { return total_; }
    void Clear() { *this = RateCounter(); }

private:
    static constexpr size_t kQueue = 8;
    std::array<Window, kQueue> done_ {};
    size_t queued_ = 0;
    uint64_t start_ = 0;
    uint32_t count_ = 0;
    uint64_t total_ = 0;
    bool started_ = false;
};

// "Swaps per second" lines while flooding: every second with swaps for the first 30, then every 10th second, at most 60 lines.
class ThrottledSwapCounter
{
public:
    static constexpr uint32_t kFirstSeconds = 30;
    static constexpr uint32_t kThenEvery = 10;
    static constexpr uint32_t kMaxLines = 60;

    struct Line
    {
        uint32_t second = 0;          // 1-based number of this second with swaps
        uint32_t perSecond = 0;       // swaps in that window, scaled to one second
        uint64_t total = 0;           // swaps so far
    };

    void Add(uint64_t now, uint32_t n = 1) { rate_.Add(now, n); }

    // The next line that is due, or false. Seconds that the limits skip are consumed silently.
    bool TakeLine(uint64_t now, Line& out)
    {
        rate_.Roll(now);
        RateCounter::Window w;
        while (rate_.Take(w))
        {
            ++seconds_;
            lineTotal_ += w.count;
            const bool due = seconds_ <= kFirstSeconds || ((seconds_ - kFirstSeconds) % kThenEvery) == 0;
            if (!due || lines_ >= kMaxLines)
                continue;
            ++lines_;
            out.second = seconds_;
            out.perSecond = w.durationMs ? static_cast<uint32_t>((static_cast<uint64_t>(w.count) * 1000 + w.durationMs / 2) / w.durationMs) : w.count;
            out.total = lineTotal_;
            return true;
        }
        return false;
    }

    uint32_t Lines() const { return lines_; }
    uint64_t Total() const { return rate_.Total(); }
    void Clear() { *this = ThrottledSwapCounter(); }

private:
    RateCounter rate_;
    uint32_t seconds_ = 0;
    uint32_t lines_ = 0;
    uint64_t lineTotal_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The ResolveSubresource hook
// ---------------------------------------------------------------------------------------------------------------------

// The probe observes ResolveSubresource (a resolve is another way a copy of the preview can reach a texture). The detour is not part of
// the lighting layer's all-or-nothing hook transaction (a failure there would take the shipped features down for a debug aid): the hook
// layer attaches it in a transaction of its own when the probe or the flood test is first switched on, and removes it when both are off
// and when the hooks are removed. This state machine decides when, from "is any debug aid wanted" alone.
class ResolveHookState
{
  public:
    enum class Step : uint8_t
    {
        None,
        Attach, // a debug aid was switched on: attach the detour now
        Detach, // both were switched off and the detour is attached: remove it
    };

    // Asked on every dispatched draw with "the probe or the flood test is on"; acts on the changes only.
    Step Decide(bool wanted)
    {
        if (wanted == _wanted)
            return Step::None;
        _wanted = wanted;
        if (wanted)
            return Step::Attach;
        return _attached ? Step::Detach : Step::None;
    }

    void AttachResult(bool ok) { _attached = ok; }
    void Detached() { _attached = false; }

    bool Attached() const { return _attached; }
    // Copy tracking is incomplete: a debug aid is on but the detour is not there (the attach failed, or no hook layer is installed).
    bool Incomplete() const { return _wanted && !_attached; }

  private:
    bool _wanted = false;
    bool _attached = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// Output budget
// ---------------------------------------------------------------------------------------------------------------------

// Every image is two files (the picture and its alpha channel) and a dump adds state.txt: native, AA, DLSS and NR together are 9 files, so
// 40 files allow four such dumps (five before the NR result existed).
inline constexpr uint32_t kMaxFiles = 40;
inline constexpr uint64_t kMaxBytes = 300ull * 1024 * 1024;

struct OutputBudget
{
    uint32_t files = 0;
    uint64_t bytes = 0;

    // All or nothing: a set of `addFiles` files with `addBytes` bytes in total is allowed only when all of it fits.
    bool Fits(uint32_t addFiles, uint64_t addBytes) const
    {
        if (files > kMaxFiles || bytes > kMaxBytes)
            return false;
        return addFiles <= kMaxFiles - files && addBytes <= kMaxBytes - bytes;
    }

    void Add(uint32_t addFiles, uint64_t addBytes)
    {
        files = addFiles > UINT32_MAX - files ? UINT32_MAX : files + addFiles;
        bytes = addBytes > UINT64_MAX - bytes ? UINT64_MAX : bytes + addBytes;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Live command parser (one command per line of OptiScaler\chara-probe.cmd)
// ---------------------------------------------------------------------------------------------------------------------

inline constexpr size_t kMaxCommandBytes = 4096;
inline constexpr size_t kMaxCommandLines = 32;
inline constexpr size_t kMaxEchoChars = 48;

enum class CommandKind : uint8_t
{
    Empty,   // blank line or comment only
    Tint,    // tint on|off
    Flood,   // flood on|off
    Aa,      // aa on|off
    Dlss,    // dlss on|off
    Upscale, // upscale on|off
    Dump,    // dump: save preview images and state
    State,   // state: save state only
    Copies,  // copies on|off
    Reset,   // reset: forget derived textures and counters
    Help,    // help
    Unknown, // anything else; see Command::note
};

struct Command
{
    CommandKind kind = CommandKind::Empty;
    bool value = false;    // the on/off of a toggle
    bool hasValue = false; // true for a valid toggle
    std::string text;      // the line as it can be echoed: printable ASCII only, at most kMaxEchoChars characters
    std::string note;      // why a line is Unknown
};

inline constexpr const char* kHelpText =
    "commands: tint on|off, flood on|off, aa on|off, dlss on|off, upscale on|off, copies on|off, dump, state, reset, help "
    "(one per line, # starts a comment)";

constexpr const char* CommandName(CommandKind k)
{
    switch (k)
    {
    case CommandKind::Empty: return "empty";
    case CommandKind::Tint: return "tint";
    case CommandKind::Flood: return "flood";
    case CommandKind::Aa: return "aa";
    case CommandKind::Dlss: return "dlss";
    case CommandKind::Upscale: return "upscale";
    case CommandKind::Dump: return "dump";
    case CommandKind::State: return "state";
    case CommandKind::Copies: return "copies";
    case CommandKind::Reset: return "reset";
    case CommandKind::Help: return "help";
    case CommandKind::Unknown: return "unknown";
    }
    return "unknown";
}

namespace detail
{
constexpr bool IsBlank(char c) { return c == ' ' || c == '\t'; }
constexpr bool IsPrintable(char c) { return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7E; }

inline std::string Echo(std::string_view s)
{
    std::string out;
    for (char c : s)
    {
        if (!IsPrintable(c))
            continue; // control bytes, NUL, and everything above 0x7E are dropped
        if (out.size() >= kMaxEchoChars)
            break;
        out.push_back(c);
    }
    return out;
}

inline std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (IsBlank(s.front()) || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (IsBlank(s.back()) || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}
} // namespace detail

inline Command ParseLine(std::string_view line)
{
    Command cmd;
    const size_t hash = line.find('#');
    if (hash != std::string_view::npos)
        line = line.substr(0, hash);
    line = detail::Trim(line);
    if (line.empty())
        return cmd; // Empty

    cmd.text = detail::Echo(line);
    auto unknown = [&cmd](const char* why) {
        cmd.kind = CommandKind::Unknown;
        cmd.value = false;
        cmd.hasValue = false;
        cmd.note = why;
        return cmd;
    };

    for (char c : line)
        if (!detail::IsBlank(c) && !detail::IsPrintable(c))
            return unknown("the line contains bytes that are not printable ASCII");

    // words separated by spaces or tabs
    std::vector<std::string> words;
    size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && detail::IsBlank(line[i]))
            ++i;
        const size_t begin = i;
        while (i < line.size() && !detail::IsBlank(line[i]))
            ++i;
        if (i > begin)
            words.push_back(detail::Lower(line.substr(begin, i - begin)));
    }
    if (words.empty())
        return cmd;

    struct Entry
    {
        const char* name;
        CommandKind kind;
        bool toggle;
    };
    static constexpr Entry table[] = {
        { "tint", CommandKind::Tint, true },       { "flood", CommandKind::Flood, true },
        { "aa", CommandKind::Aa, true },           { "dlss", CommandKind::Dlss, true },
        { "upscale", CommandKind::Upscale, true }, { "copies", CommandKind::Copies, true },
        { "dump", CommandKind::Dump, false },      { "state", CommandKind::State, false },
        { "reset", CommandKind::Reset, false },    { "help", CommandKind::Help, false },
    };
    for (const Entry& e : table)
    {
        if (words[0] != e.name)
            continue;
        if (!e.toggle)
        {
            if (words.size() != 1)
                return unknown("this command takes no argument");
            cmd.kind = e.kind;
            return cmd;
        }
        if (words.size() < 2)
            return unknown("expected on or off");
        if (words.size() > 2)
            return unknown("expected only on or off");
        const std::string& v = words[1];
        if (v == "on" || v == "true" || v == "1")
            cmd.value = true;
        else if (v == "off" || v == "false" || v == "0")
            cmd.value = false;
        else
            return unknown("expected on or off");
        cmd.kind = e.kind;
        cmd.hasValue = true;
        return cmd;
    }
    return unknown("unknown command");
}

struct ParsedFile
{
    std::vector<Command> commands; // non-empty lines only, in order
    bool truncatedBytes = false;   // the input was longer than maxBytes: the rest (and a partial last line) was ignored
    bool truncatedLines = false;   // more than maxLines lines: the rest was ignored
    size_t lines = 0;              // lines examined
};

inline ParsedFile ParseFile(std::string_view text, size_t maxBytes = kMaxCommandBytes, size_t maxLines = kMaxCommandLines)
{
    ParsedFile out;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3); // UTF-8 byte order mark that Windows editors may add
    if (text.size() > maxBytes)
    {
        out.truncatedBytes = true;
        const bool endsOnLine = text[maxBytes] == '\n'; // the cut is exactly at the end of a line
        text = text.substr(0, maxBytes);
        if (!endsOnLine)
        {
            const size_t lastNewline = text.rfind('\n');
            text = lastNewline == std::string_view::npos ? std::string_view() : text.substr(0, lastNewline + 1); // no partial command
        }
    }
    size_t pos = 0;
    while (pos < text.size())
    {
        if (out.lines >= maxLines)
        {
            out.truncatedLines = true;
            break;
        }
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos)
            end = text.size();
        ++out.lines;
        Command c = ParseLine(text.substr(pos, end - pos));
        if (c.kind != CommandKind::Empty)
            out.commands.push_back(std::move(c));
        pos = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// PNG writer: 8-bit RGB or grayscale, zlib "stored" blocks (no compression), one IDAT chunk
// ---------------------------------------------------------------------------------------------------------------------

inline constexpr uint32_t kMaxPngDimension = 16384;

inline uint32_t Adler32(const void* data, size_t size, uint32_t adler = 1)
{
    uint32_t a = adler & 0xffff, b = adler >> 16;
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0)
    {
        const size_t n = std::min<size_t>(size, 5552); // largest block that cannot overflow 32 bits before the modulo
        for (size_t i = 0; i < n; ++i)
        {
            a += p[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        p += n;
        size -= n;
    }
    return (b << 16) | a;
}

// Exact size in bytes of EncodePng's output; 0 when the arguments are not valid.
inline uint64_t PngSize(uint32_t width, uint32_t height, uint32_t channels)
{
    if (width == 0 || height == 0 || width > kMaxPngDimension || height > kMaxPngDimension || (channels != 1 && channels != 3))
        return 0;
    const uint64_t raw = static_cast<uint64_t>(height) * (1 + static_cast<uint64_t>(width) * channels); // a filter byte per row
    const uint64_t blocks = (raw + 65534) / 65535;
    const uint64_t zlib = 2 + blocks * 5 + raw + 4;
    const uint64_t srgb = channels == 3 ? 12 + 1 : 0;
    return 8 + (12 + 13) + srgb + (12 + zlib) + 12;
}

namespace detail
{
inline void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

inline void PutTag(std::vector<uint8_t>& v, const char (&tag)[5]) { v.insert(v.end(), tag, tag + 4); }

// Closes the chunk whose length field starts at `lengthAt`: fills in the length and appends the CRC of type + data.
inline void CloseChunk(std::vector<uint8_t>& v, size_t lengthAt)
{
    const size_t dataEnd = v.size();
    const uint32_t length = static_cast<uint32_t>(dataEnd - (lengthAt + 8));
    v[lengthAt] = static_cast<uint8_t>(length >> 24);
    v[lengthAt + 1] = static_cast<uint8_t>(length >> 16);
    v[lengthAt + 2] = static_cast<uint8_t>(length >> 8);
    v[lengthAt + 3] = static_cast<uint8_t>(length);
    Put32(v, ext::core::Crc32(v.data() + lengthAt + 4, dataEnd - (lengthAt + 4)));
}
} // namespace detail

// `pixels` is row-major, tightly packed, width * height * channels bytes (R,G,B per pixel for channels == 3).
// Colour images (3 channels) carry an sRGB chunk. Appends nothing and returns false for invalid arguments.
inline bool EncodePng(uint32_t width, uint32_t height, uint32_t channels, const uint8_t* pixels, std::vector<uint8_t>& out)
{
    const uint64_t total = PngSize(width, height, channels);
    if (total == 0 || !pixels)
        return false;
    const size_t rowBytes = static_cast<size_t>(width) * channels;
    const uint64_t raw = static_cast<uint64_t>(height) * (1 + rowBytes);

    const size_t base = out.size();
    out.reserve(base + static_cast<size_t>(total));
    static constexpr uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    out.insert(out.end(), signature, signature + 8);

    size_t at = out.size();
    detail::Put32(out, 0);
    detail::PutTag(out, "IHDR");
    detail::Put32(out, width);
    detail::Put32(out, height);
    out.push_back(8);                      // bit depth
    out.push_back(channels == 3 ? 2 : 0);  // colour type: truecolour or grayscale
    out.push_back(0);                      // compression
    out.push_back(0);                      // filter method
    out.push_back(0);                      // no interlace
    detail::CloseChunk(out, at);

    if (channels == 3)
    {
        at = out.size();
        detail::Put32(out, 0);
        detail::PutTag(out, "sRGB");
        out.push_back(0); // perceptual rendering intent
        detail::CloseChunk(out, at);
    }

    at = out.size();
    detail::Put32(out, 0);
    detail::PutTag(out, "IDAT");
    out.push_back(0x78); // zlib header: deflate, 32 KiB window, no dictionary, fastest
    out.push_back(0x01);

    uint32_t adler = 1;
    uint64_t position = 0; // in the raw stream (a zero filter byte, then the row, for each row)
    std::vector<uint8_t> block;
    while (position < raw)
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(65535, raw - position));
        block.resize(n);
        size_t filled = 0;
        while (filled < n)
        {
            const uint64_t at2 = position + filled;
            const uint64_t row = at2 / (1 + rowBytes);
            const size_t col = static_cast<size_t>(at2 % (1 + rowBytes));
            if (col == 0)
            {
                block[filled++] = 0; // filter type "none"
                continue;
            }
            const size_t take = std::min(n - filled, rowBytes - (col - 1));
            std::memcpy(block.data() + filled, pixels + row * rowBytes + (col - 1), take);
            filled += take;
        }
        const bool last = position + n == raw;
        out.push_back(last ? 1 : 0); // BFINAL, BTYPE = stored
        out.push_back(static_cast<uint8_t>(n & 0xff));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(~n & 0xff));
        out.push_back(static_cast<uint8_t>((~n >> 8) & 0xff));
        out.insert(out.end(), block.begin(), block.end());
        adler = Adler32(block.data(), n, adler);
        position += n;
    }
    detail::Put32(out, adler);
    detail::CloseChunk(out, at);

    at = out.size();
    detail::Put32(out, 0);
    detail::PutTag(out, "IEND");
    detail::CloseChunk(out, at);
    return out.size() - base == total;
}
} // namespace ext::ffxiv::chara::probe
