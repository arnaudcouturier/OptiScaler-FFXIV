#pragma once
// The processed set of preview windows: at most K preview images get the upscaler and the NR work, the others keep the native
// picture. Pure state with the clock passed in, so every rule is tested without a device.
//
// With the set off (the default) nothing here decides anything: Processed and FollowsPass answer "yes" for every image, the caller
// keeps the behaviour it had (every preview image a UI draw samples may claim a slot, every pass is followed), and no state is kept.
//
// With the set on:
//   * demand: an image a UI draw sampled within 1000 ms (the window the NR liveness uses) wants a place in the set, and an image only
//     counts while it renders: a finisher within the same window is the proof. Among the candidates the most recently sampled wins, then
//     the lower output entry, then the lower texture pointer. A member that is still sampled keeps its place (hysteresis), so two windows
//     that alternate do not trade places. At most one member is exchanged per 250 ms: after a stall every member is idle at the same
//     moment, and one newcomer then takes the place of the one member it displaces instead of the whole set being rebuilt;
//   * identity: which image a render belongs to is learned, not guessed. Each preview window draws with its own camera constant buffer
//     binding (the buffer object, its first constant and the pass size, camera::BlockKey), and every accepted finisher teaches which
//     binding belongs to which image. A pass is followed only when the binding at its first geometry draw names an image the set
//     processes. Nothing is read back from video memory, so naming a pass never waits for the GPU. A binding that was never seen at a
//     finisher names nothing; a binding two images drew with names neither while both were seen within 1000 ms, and it names the survivor
//     again once the other one stopped drawing. A pass that cannot be named runs untouched;
//   * a wrong name is detected at the finisher: the pass remembers the image the binding named, and a finisher of another image proves that
//     the binding cannot tell the two windows apart. That binding then names no image for 30 s (kWindowMismatchMs): every pass drawn with it runs
//     untouched, so no window is shifted for an image it is not and no window loses its composed picture because another window drew with the
//     same binding. A further mismatch inside that hold cannot push its end past one more 30 s window from the first mismatch, so the hold is
//     never renewed endlessly; once it ends the naming rule above applies again. A hold needs a second image that still draws with the binding:
//     a window that took the binding over from one that stopped drawing is named at once, so closing a window does not leave its successor
//     native for 30 s;
//   * two accepted finishers of one image in one game frame mean two windows share one output texture: the caller clears that
//     image's history (the counters of the diagnostics detect the case).
//
// Times are milliseconds from any monotonic clock (GetTickCount64 in the game).

#include <array>
#include <cstddef>
#include <cstdint>

#include "ext/ffxiv/chara/CharaCameraRules.h"

namespace ext::ffxiv::chara
{
inline constexpr uint32_t kDefaultMaxWindows = 2; // [FFXIV] PreviewMaxWindows: the default of the setting
inline constexpr uint32_t kMaxWindows = 8;        // the preview renderer has eight output entries
inline constexpr size_t kMaxKnownImages = 16;     // images remembered for the demand and admission rules
inline constexpr size_t kMaxCameraBlocks = 16;    // camera block bindings remembered as belonging to a preview image
inline constexpr uint64_t kWindowDemandMs = 1000; // a UI draw sampled within this time is demand, and a finisher within it is proof of a render
inline constexpr uint64_t kWindowLiveMs = 250;    // a member idle longer than this may be replaced, and the pause between two exchanges
inline constexpr uint64_t kWindowMismatchMs = 30000; // a binding that mis-named an image does not name it again for this long

// What a camera block binding says about the image a render belongs to.
enum class BlockState : uint8_t
{
    Unknown,   // the binding was never seen at a finisher
    Ambiguous, // two images drew with this binding within the demand window: it names neither of them
    Known,     // the binding belongs to one image
};

struct BlockName
{
    BlockState state = BlockState::Unknown;
    uintptr_t image = 0;  // the image the binding belongs to (Known only)
    bool cleared = false; // an ambiguity ended here: the second image stopped drawing with this binding
};

// Which preview image a render belongs to, from the camera block binding it is drawn with: learned at the finisher (where the renderer's
// output entry is known) and looked up at the render's first geometry draw. The binding keeps the two images that drew with it most
// recently; it is ambiguous only while both were seen within kWindowDemandMs, so closing one of two windows that share a binding lets the
// other one be named again instead of keeping it native for the session. A binding a pass named wrongly finished with a different image is
// held: it names no image for kWindowMismatchMs (Hold), so every pass drawn with it runs untouched. The hold is per binding, not per image, and
// a mismatch inside it cannot renew it past one more window from the first one. It also needs two images that still draw with the binding: a
// finisher that replaced an image which stopped drawing leaves one image on the binding, and that one is named at once.
class ImageIdentity
{
  public:
    struct Learned
    {
        bool block = false;   // a binding was learned for the first time
        bool cleared = false; // an ambiguity ended (the second image stopped drawing with the binding)
    };

    // `block` is the binding the render was drawn with, `key` the preview image it finished into.
    Learned Learn(const camera::BlockKey& block, uintptr_t key, uint64_t now)
    {
        Learned learned;
        if (!block.Valid() || key == 0)
            return learned;
        Entry* e = Find(block);
        if (e == nullptr)
        {
            e = Insert(block);
            if (e == nullptr)
                return learned;
            e->image = key;
            e->imageAt = now;
            learned.block = true;
            return learned;
        }
        if (key == e->image)
        {
            e->imageAt = now;
            return learned;
        }
        if (e->other != 0 && key == e->other)
        {
            e->otherAt = now;
            return learned;
        }
        // A third image, or the second one after a pause. The binding keeps the two images that drew with it last; the older of them is
        // dropped once it has not drawn with it for the demand window, and the binding then names one image again.
        if (e->other == 0)
        {
            if (!Fresh(e->imageAt, now))
            {
                // The image before this one stopped drawing with the binding: it names the new one.
                e->image = key;
                e->imageAt = now;
                return learned;
            }
            e->other = key; // the first time a second image draws with it: it names neither while both draw
            e->otherAt = now;
            return learned;
        }
        if (Fresh(e->otherAt, now))
        {
            // A third image: the two most recent stay.
            e->other = e->image;
            e->otherAt = e->imageAt;
            e->image = key;
            e->imageAt = now;
            return learned;
        }
        e->other = 0;
        e->otherAt = 0;
        e->image = key;
        e->imageAt = now;
        learned.cleared = true;
        return learned;
    }

    // What the binding names now. A second image that has not drawn with it for the demand window no longer makes it ambiguous: the binding
    // names the one that is still drawing. Two images that both stopped drawing leave it ambiguous until one of them draws again. A binding a
    // pass named wrongly finished with another image is held: it names neither image while the hold lasts, so every pass drawn with it stays
    // native and nothing is swapped between the two windows.
    BlockName NameOf(const camera::BlockKey& block, uint64_t now)
    {
        BlockName result;
        if (!block.Valid())
            return result;
        Entry* e = Find(block);
        if (e == nullptr || e->image == 0)
            return result;
        if (Held(*e, now))
        {
            result.state = BlockState::Ambiguous;
            return result;
        }
        if (e->other != 0)
        {
            const bool imageFresh = Fresh(e->imageAt, now);
            const bool otherFresh = Fresh(e->otherAt, now);
            if (imageFresh == otherFresh)
            {
                result.state = BlockState::Ambiguous; // both are drawing with it, or neither is
                return result;
            }
            if (otherFresh)
            {
                // The second image is the one still drawing: it takes the identity over.
                const uintptr_t image = e->image;
                const uint64_t imageAt = e->imageAt;
                e->image = e->other;
                e->imageAt = e->otherAt;
                e->other = image;
                e->otherAt = imageAt;
            }
            e->other = 0;
            e->otherAt = 0;
            result.cleared = true;
        }
        result.state = BlockState::Known;
        result.image = e->image;
        return result;
    }

    // The binding `block` named one image for a pass that finished into another one: the two cannot be told apart by this binding, so it names
    // neither for kWindowMismatchMs and every pass drawn with it runs untouched. A mismatch inside an active hold extends its end by at most one
    // window from the first mismatch of that hold (no endless renewal), and a mismatch after it starts a new hold. A hold needs a second image
    // that still draws with the binding: a finisher of an image that replaced the only image the binding had (a closed window) teaches one image
    // and no partner, so nothing is held and the binding names that image at its next lookup instead of keeping it native for the whole window.
    void Hold(const camera::BlockKey& block, uint64_t now)
    {
        Entry* e = Find(block);
        if (e == nullptr || !Live(*e, now))
            return;
        if (!Held(*e, now))
            e->holdFrom = now; // the first mismatch of this hold
        const uint64_t until = now + kWindowMismatchMs;
        const uint64_t ceiling = e->holdFrom + 2 * kWindowMismatchMs; // one more window from the first mismatch at most
        e->heldUntil = until < ceiling ? until : ceiling;
    }

    // The bindings that learned one image and no second one (the burst line's count; whether one of them may name a pass now is NameOf's answer).
    size_t Known() const
    {
        size_t n = 0;
        for (const Entry& e : _entries)
            n += (e.image != 0 && e.other == 0) ? 1 : 0;
        return n;
    }

    void Reset() { _entries = {}; }

  private:
    struct Entry
    {
        camera::BlockKey block;
        uintptr_t image = 0; // the image that drew with the binding last
        uint64_t imageAt = 0;
        uintptr_t other = 0; // the second image that drew with it, while it still counts
        uint64_t otherAt = 0;
        uint64_t holdFrom = 0;  // the first mismatch of the hold that started here
        uint64_t heldUntil = 0; // the binding names nobody before this time
    };

    static bool Fresh(uint64_t at, uint64_t now) { return at != 0 && now >= at && now - at <= kWindowDemandMs; }

    // Two images that both drew with this binding within the demand window: only then can the binding mis-name a pass. One image that took the
    // binding over from one that stopped drawing leaves the second slot empty (Learn), so a binding a single window draws with names it again.
    static bool Live(const Entry& e, uint64_t now)
    {
        return e.image != 0 && e.other != 0 && Fresh(e.imageAt, now) && Fresh(e.otherAt, now);
    }

    // Whether this binding names nobody at `now` because a pass it named finished into another image.
    static bool Held(const Entry& e, uint64_t now) { return e.heldUntil != 0 && now >= e.holdFrom && now < e.heldUntil; }

    Entry* Find(const camera::BlockKey& block)
    {
        for (Entry& e : _entries)
        {
            if (e.image != 0 && e.block == block)
                return &e;
        }
        return nullptr;
    }

    // A free entry, or the binding that was seen longest ago (the least recently learned one is forgotten first).
    Entry* Insert(const camera::BlockKey& block)
    {
        Entry* target = nullptr;
        for (Entry& e : _entries)
        {
            if (e.image == 0)
            {
                target = &e;
                break;
            }
            if (target == nullptr || e.imageAt < target->imageAt)
                target = &e;
        }
        if (target == nullptr)
            return nullptr;
        *target = Entry {};
        target->block = block;
        return target;
    }

    std::array<Entry, kMaxCameraBlocks> _entries {};
};

class WindowSet
{
  public:
    // What the identity says about the image a pass is drawing.
    enum class Name : uint8_t
    {
        Unknown,   // a usable camera block binding that no finisher has taught yet
        Ambiguous, // two images drew with that binding lately: it names neither
        Outside,   // the binding names an image the set does not process
        Named,     // the binding names a processed image: the pass may be followed
        Invalid,   // nothing that can hold the camera block is bound at the draw's own stage
    };

    struct PassName
    {
        Name name = Name::Unknown;
        uintptr_t image = 0;  // the image the binding named the pass for (Named and Outside); the finisher proves whether it was right
        bool cleared = false; // an ambiguity ended at this lookup
    };

    struct Learned
    {
        bool block = false;   // a camera block binding was learned for the first time
        bool cleared = false; // an ambiguity ended at this finisher
    };

    // The key and its range: `on` false turns every rule off and forgets the set (the caller reads the config before it asks).
    void Configure(bool on, uint32_t maxWindows)
    {
        const uint32_t k = maxWindows < 1 ? 1 : (maxWindows > kMaxWindows ? kMaxWindows : maxWindows);
        if (!on)
        {
            if (_on)
                Reset(); // nothing is remembered while the rule is off, so switching it off once is enough
            _on = false;
            _max = k;
            return;
        }
        if (!_on)
        {
            Reset();
            _on = true;
        }
        _max = k;
        // The setting shrank: the members that were sampled longest ago leave until the set fits.
        while (_count > _max)
        {
            const int weakest = WeakestMember();
            if (weakest < 0)
                break;
            _known[static_cast<size_t>(weakest)].member = false;
            --_count;
        }
    }

    bool On() const { return _on; }
    uint32_t Max() const { return _max; }
    size_t Count() const { return _count; }

    // The preview images the set remembers (at most kMaxKnownImages). With the set off it remembers none: the per-draw call must cost nothing.
    size_t KnownImages() const { return _knownCount; }

    // Members the UI sampled lately: the windows that are really on screen (a member that was closed is only dropped when another image
    // takes its place, so Count alone would keep counting it).
    size_t FreshCount(uint64_t now) const
    {
        size_t n = 0;
        for (const KnownImage& k : _known)
            n += (k.key != 0 && k.member && Fresh(k, now)) ? 1 : 0;
        return n;
    }

    // A UI draw sampled the preview image `key` of `entry`: it wants a place in the set, and it takes one when a place is free or when the
    // weakest member has been idle long enough. Called for every sampled preview image, whatever the other options say. With the set off
    // nothing is remembered (the state stays empty, so a changed key costs nothing).
    void NoteDemand(uintptr_t key, int entry, uint64_t now)
    {
        if (!_on || key == 0)
            return;
        const int at = FindOrInsert(key);
        if (at < 0)
            return;
        KnownImage& k = _known[static_cast<size_t>(at)];
        k.entry = entry;
        k.lastSample = now;
        Admit(now);
    }

    // The image belongs to the processed set. With the set off every image does (fail open).
    bool Processed(uintptr_t key) const
    {
        if (!_on)
            return true;
        const int at = Find(key);
        return at >= 0 && _known[static_cast<size_t>(at)].member;
    }

    // An accepted finisher of `key` that was drawn with the camera block `block`: the binding names that image from now on, and the image is
    // proven to render now (only such an image takes a place from another one). Every finisher teaches, so a window that opened while the
    // set was full is named on its next pass instead of every window losing one render to an empty identity.
    Learned NoteFinisher(uintptr_t key, const camera::BlockKey& block, uint64_t now)
    {
        Learned learned;
        if (!_on || key == 0)
            return learned;
        // A render is the strongest evidence that a window exists: the image is remembered even before a UI draw samples it, so its first
        // sample already knows that it draws.
        if (const int at = FindOrInsert(key); at >= 0)
            _known[static_cast<size_t>(at)].lastRender = now;
        const ImageIdentity::Learned identity = _identity.Learn(block, key, now);
        learned.block = identity.block;
        learned.cleared = identity.cleared;
        return learned;
    }

    // What the camera block binding `block` says about the image the pass is drawing, and whether that image is processed.
    PassName NamePass(const camera::BlockKey& block, uint64_t now)
    {
        PassName result;
        if (!_on)
        {
            result.name = Name::Named; // off: the pass is followed as before
            return result;
        }
        if (!block.Valid())
        {
            result.name = Name::Invalid; // nothing that can hold the camera block is bound at the draw's stage
            return result;
        }
        const BlockName named = _identity.NameOf(block, now);
        result.cleared = named.cleared;
        if (named.state == BlockState::Unknown)
            return result;
        if (named.state == BlockState::Ambiguous)
        {
            result.name = Name::Ambiguous;
            return result;
        }
        result.image = named.image;
        result.name = Processed(named.image) ? Name::Named : Name::Outside;
        return result;
    }

    // The finisher of a pass the binding `block` named `namedImage` for drew the image `finisher` instead. The binding cannot tell the two
    // images apart, so it names neither for kWindowMismatchMs while both still draw with it: every pass drawn with it runs untouched (fail
    // open), and nothing is swapped between the two windows in the meantime. A finisher that took the binding over from an image that stopped
    // drawing holds nothing (Hold): it is the only image the binding has, so it is named at once. Returns true when the finisher was another
    // image than the name said.
    bool NoteMismatch(const camera::BlockKey& block, uintptr_t namedImage, uintptr_t finisher, uint64_t now)
    {
        if (!_on || namedImage == 0 || finisher == 0 || namedImage == finisher)
            return false;
        _identity.Hold(block, now);
        return true;
    }

    // The jitter and the replay follow a pass only when the binding it is drawn with names a processed image (with the set off: always).
    bool FollowsPass(Name name) const { return !_on || name == Name::Named; }

    // The camera block bindings that name an image now (diagnostics).
    size_t KnownBlocks() const { return _identity.Known(); }

    // The set is full and an image that is not a member was sampled lately: a place is wanted, so a closed window's NR model does not
    // need its full linger time.
    bool HasWaitingCandidate(uint64_t now) const
    {
        if (!_on || _count < _max)
            return false;
        for (const KnownImage& k : _known)
        {
            if (k.key != 0 && !k.member && Fresh(k, now))
                return true;
        }
        return false;
    }

    void Reset()
    {
        _known = {};
        _knownCount = 0;
        _count = 0;
        _identity.Reset();
        _swapped = false;
        _lastSwap = 0;
    }

  private:
    struct KnownImage
    {
        uintptr_t key = 0;
        int entry = -1;
        uint64_t lastSample = 0; // the last UI draw that sampled it
        uint64_t lastRender = 0; // the last accepted finisher: proof that the window is drawing, not only sampled
        bool member = false;
    };

    // A UI draw sampled it within the demand window (an image no draw has sampled yet is not on screen).
    static bool Fresh(const KnownImage& k, uint64_t now)
    {
        return k.lastSample != 0 && now >= k.lastSample && now - k.lastSample <= kWindowDemandMs;
    }

    // The image is drawing now: its last finisher is within the demand window.
    static bool Rendering(const KnownImage& k, uint64_t now)
    {
        return k.lastRender != 0 && now >= k.lastRender && now - k.lastRender <= kWindowDemandMs;
    }

    int Find(uintptr_t key) const
    {
        for (size_t i = 0; i < _known.size(); ++i)
        {
            if (_known[i].key == key)
                return static_cast<int>(i);
        }
        return -1;
    }

    // The entry of `key`, or a free one, or the least recently sampled image that is not a member (a member is never dropped).
    int FindOrInsert(uintptr_t key)
    {
        const int found = Find(key);
        if (found >= 0)
            return found;
        for (size_t i = 0; i < _known.size(); ++i)
        {
            if (_known[i].key == 0)
            {
                _known[i] = KnownImage { key, -1, 0, 0, false };
                ++_knownCount;
                return static_cast<int>(i);
            }
        }
        int victim = -1;
        for (size_t i = 0; i < _known.size(); ++i)
        {
            if (_known[i].member)
                continue;
            if (victim < 0 || _known[i].lastSample < _known[static_cast<size_t>(victim)].lastSample)
                victim = static_cast<int>(i);
        }
        if (victim < 0)
            return -1; // every remembered image is processed: nothing to forget
        _known[static_cast<size_t>(victim)] = KnownImage { key, -1, 0, 0, false };
        return victim;
    }

    // The candidate that deserves a place: one that is drawing (a finisher within the demand window, so an image a UI draw only samples does
    // not take a place from a window that renders), the most recently sampled, then the lower output entry, then the lower pointer.
    int BestCandidate(uint64_t now) const
    {
        int best = -1;
        for (size_t i = 0; i < _known.size(); ++i)
        {
            const KnownImage& k = _known[i];
            if (k.key == 0 || k.member || !Fresh(k, now) || !Rendering(k, now))
                continue;
            if (best < 0 || Better(k, _known[static_cast<size_t>(best)]))
                best = static_cast<int>(i);
        }
        return best;
    }

    static bool Better(const KnownImage& a, const KnownImage& b)
    {
        if (a.lastSample != b.lastSample)
            return a.lastSample > b.lastSample;
        const int ea = a.entry >= 0 ? a.entry : static_cast<int>(kMaxWindows);
        const int eb = b.entry >= 0 ? b.entry : static_cast<int>(kMaxWindows);
        if (ea != eb)
            return ea < eb;
        return a.key < b.key;
    }

    int WeakestMember() const
    {
        int weakest = -1;
        for (size_t i = 0; i < _known.size(); ++i)
        {
            if (!_known[i].member)
                continue;
            if (weakest < 0 || _known[i].lastSample < _known[static_cast<size_t>(weakest)].lastSample)
                weakest = static_cast<int>(i);
        }
        return weakest;
    }

    // One exchange per kWindowLiveMs: after a stall (a hitch, an alt-tab, a zone change) every member is idle at the same moment, and
    // the images that come back in one batch must not displace each other. A member then loses its place only to an image that takes
    // it, and only one does per pause.
    bool ExchangeAllowed(uint64_t now) const { return !_swapped || now < _lastSwap || now - _lastSwap > kWindowLiveMs; }

    void Admit(uint64_t now)
    {
        if (_count < _max)
        {
            const int best = BestCandidate(now);
            if (best >= 0)
            {
                _known[static_cast<size_t>(best)].member = true;
                ++_count;
            }
            return;
        }
        if (!ExchangeAllowed(now))
            return;
        const int weakest = WeakestMember();
        if (weakest < 0)
            return;
        const KnownImage& w = _known[static_cast<size_t>(weakest)];
        if (!(now >= w.lastSample && now - w.lastSample > kWindowLiveMs))
            return; // the members are all live: nothing is replaced (hysteresis)
        const int best = BestCandidate(now);
        if (best < 0)
            return;
        _known[static_cast<size_t>(weakest)].member = false;
        _known[static_cast<size_t>(best)].member = true;
        _swapped = true;
        _lastSwap = now;
    }

    std::array<KnownImage, kMaxKnownImages> _known {};
    size_t _knownCount = 0;
    size_t _count = 0;
    ImageIdentity _identity;
    bool _swapped = false; // whether an exchange ever happened, and when the last one did
    uint64_t _lastSwap = 0;
    uint32_t _max = kDefaultMaxWindows;
    bool _on = false;
};

// The set the dispatcher and the NR module share (draw thread only).
inline WindowSet g_windowSet;
} // namespace ext::ffxiv::chara
