#pragma once
// S10 package A: the draw-time core of the boundary detector (docs/HUDLESS.md section 10), free of OptiScaler
// dependencies (no pch, config, log, files, swapchain access) so the WARP test builds it standalone. The game integration
// is FfxivUiBoundary.cpp; the files are UiDiag.cpp.
//
// Threading: TagShader() may run on any thread (shader creation). Everything else runs on the render thread (the thread of
// the first draw of a frame, the presenting thread inside Present); draws from another thread are counted and ignored. COM objects are
// never retained: a backbuffer or render target is only ever compared by pointer.
//
// Frame life cycle (driven by PresentScope through FfxivUiBoundary.cpp):
//   draws ... -> FreezeFrame() [Present entry] -> draws issued inside Present are counted apart -> CompleteFrame()
//   [Present exit] returns the frame's record and resets the per-frame state.

#include "ext/ffxiv/ui/UiBoundaryRules.h"
#include "ext/ffxiv/ui/UiDiagFormat.h"

#include <d3d11.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ext::ffxiv::ui
{
// One record per distinct pixel shader program (shared by every shader object with the same bytecode CRC), or one per
// shader object created before the hook (no bytecode, so no CRC). Allocated once and never freed.
struct ShaderRec
{
    uint32_t crc = 0, bytes = 0;
    uint8_t stage = kStageNone;
    bool ui = false, hasCrc = false, preHook = false, checked = false;
    uint8_t tagAgree = 0;
    uint32_t instances = 0;
    uint32_t index = 0; // N of "pre#N"
    uint64_t draws = 0, firstFrame = 0;
    dfmt::RtShape rt[dfmt::kMaxRtSet] {};
    uint8_t rtCount = 0;
    uint32_t rtOverflow = 0;
    uint64_t rtSamples = 0, bbSamples = 0;

    std::string Id() const { return hasCrc ? dfmt::Hex32(crc) : "pre#" + std::to_string(index); }
};

struct RtInfo
{
    dfmt::RtKind kind = dfmt::RtKind::None;
    dfmt::RtShape shape {};
    uint32_t samples = 1, arraySize = 1;
};

struct ContextRec
{
    const void* ptr = nullptr;
    D3D11_DEVICE_CONTEXT_TYPE type = D3D11_DEVICE_CONTEXT_IMMEDIATE;
    uint64_t draws = 0, firstFrame = 0;
};

struct RtEvent // one distinct (rule, render target kind, shape) seen at a candidate boundary
{
    uint8_t rule = 0; // 1 = R1, 2 = R2, 3 = R3
    dfmt::RtKind kind = dfmt::RtKind::None;
    dfmt::RtShape shape {};
    uint64_t firstFrame = 0, count = 0;
};

struct Counters
{
    uint64_t frames = 0, gameDraws = 0, presentDraws = 0, otherThreadDraws = 0, noPixelShaderDraws = 0, preHookDraws = 0;
    uint64_t shadersCreatedAfterHook = 0; // TagShader calls (objects), not distinct programs
    uint64_t framesWithScene = 0, framesWithUi = 0;
    uint64_t framesR1 = 0, framesR2 = 0, framesR3 = 0, framesSame = 0, framesR1First = 0, framesR2First = 0, framesNone = 0;
    uint64_t framesUiBeforeScene = 0, framesWithUnknown = 0, framesSuperseded = 0;
    uint64_t rtAt[4][4] {}; // [rule 1..3][RtKind]
    uint64_t renderThreadChanges = 0;
    // R1b (the live anchor: first UI draw into the backbuffer after the last scene stage)
    uint64_t framesR1b = 0;        // frames that have one
    uint64_t framesR1bLater = 0;   // ... and it is later than R1 (UI draws into another render target came first)
    uint64_t framesUiNoR1b = 0;    // scene frames with UI draws after the last scene stage but none into the backbuffer
};

struct FrameOutcome
{
    dfmt::FrameRecord record;
    double hookUs = 0;     // microseconds spent inside the draw hook this frame
    uint32_t r1b = 0;        // S10 E2: the frame's final R1b ordinal (0 = none); not part of the frames.txt record
    bool listWorthy = false; // anomaly flag for the logger (reason text in record.reason is set by the caller)
    const char* anomaly = ""; // "", or a short reason without spaces
};

using BackbufferFn = bool (*)(const void** texture, dfmt::RtShape* shape);

class Core
{
  public:
    static constexpr GUID kStamp { 0x5f3a9c11, 0x7b2e, 0x4d5a, { 0x9e, 0x01, 0xc3, 0x6a, 0x20, 0x55, 0x1d, 0x07 } };

    struct DrawResult
    {
        bool moved = false;     // the R3 candidate changed with this draw: capture opportunity (the draw has not run yet)
        uint32_t ordinal = 0;
        uint8_t stage = kStageNone;
        bool counted = false;   // false: the draw was ignored (other thread, before the first Present)
        uint8_t change = 0;     // candidates this draw changed: bit 0 = R1, bit 1 = R2 (see BoundaryTracker::LastChange)
        Candidates cand {};     // the candidates after this draw
        // S10 E2: R1b, the live anchor (first UI draw into the backbuffer after the last scene stage; UiBoundaryRules.h)
        uint32_t r1b = 0;       // ordinal of the R1b candidate after this draw (0 = none)
        bool r1bChanged = false; // this draw set, moved or cleared it (the draw has not run yet)
    };

    Core()
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        _freq = f.QuadPart > 0 ? f.QuadPart : 1;
        _lines.reserve(96);
        _presentLines.reserve(16);
        // The per-stage records of the always-on creation stamp (StampStage): one per catalog stage, never per shader.
        for (uint8_t stage = 1; stage <= kUiStage; ++stage)
        {
            ShaderRec& r = _stageRecs[stage];
            r.stage = stage;
            r.ui = stage == kUiStage;
        }
    }

    // ---- wiring ---------------------------------------------------------------------------------------------------
    void SetGameStageTag(const GUID* tag) { _gameTag = tag; }
    void SetBackbufferProvider(BackbufferFn fn) { _bbFn = fn; }

    // ---- shader creation (any thread) ----------------------------------------------------------------------------
    // S10 E3: the creation hook, called for EVERY successfully created pixel shader of a matched FFXIV build, whether or not the
    // detector is armed (a Mode switched on after the game loaded must find the stages already stamped: bytecode is not kept, so a
    // shader created unstamped can never be classified later). `registry` = the detector is armed: the D1 bookkeeping (per-program
    // record, counters) runs on top of the stamp. Cost without it: one atomic add, a constexpr CRC lookup and, for the dozen catalog
    // programs only, one SetPrivateData of 8 bytes; no lock, no allocation, no log.
    void ShaderCreated(ID3D11PixelShader* ps, uint32_t crc, uint32_t bytes, bool registry)
    {
        StampStage(ps, crc);
        if (registry)
            TagShader(ps, crc, bytes);
    }

    // Stamps a catalog program (scene stage 1..5, UI set) with its shared stage record; other programs stay unstamped. Any thread.
    // Always bumps the cache epoch: a new object may reuse the address of a freed one that Lookup cached.
    void StampStage(ID3D11PixelShader* ps, uint32_t crc)
    {
        const uint8_t stage = StageOfCrc(crc);
        if (stage >= 1 && stage <= kUiStage)
        {
            ShaderRec* rec = &_stageRecs[stage];
            ps->SetPrivateData(kStamp, sizeof rec, &rec);
        }
        _epoch.fetch_add(1, std::memory_order_release);
    }

    // What Lookup reads from a shader object (the draw-time tracker and the tests use this one function). Null = unstamped.
    static ShaderRec* ReadStamp(ID3D11PixelShader* ps)
    {
        ShaderRec* rec = nullptr;
        UINT size = sizeof rec;
        if (FAILED(ps->GetPrivateData(kStamp, &size, &rec)) || size != sizeof rec)
            return nullptr;
        return rec;
    }
    uint32_t Epoch() const { return _epoch.load(std::memory_order_acquire); }

    // Registers the program (key = CRC) and stamps the shader object with a pointer to its record.
    void TagShader(ID3D11PixelShader* ps, uint32_t crc, uint32_t bytes)
    {
        ShaderRec* rec = nullptr;
        {
            std::lock_guard lock(_mutex);
            auto& slot = _byCrc[crc];
            if (!slot)
            {
                slot = new ShaderRec;
                slot->crc = crc;
                slot->bytes = bytes;
                slot->hasCrc = true;
                slot->stage = StageOfCrc(crc);
                slot->ui = IsUiCrc(crc);
                _all.push_back(slot);
            }
            rec = slot;
            ++rec->instances;
            ++_cnt.shadersCreatedAfterHook;
        }
        ps->SetPrivateData(kStamp, sizeof rec, &rec);
        _epoch.fetch_add(1, std::memory_order_release);
    }

    // ---- draw time (render thread) -------------------------------------------------------------------------------
    DrawResult OnDraw(ID3D11DeviceContext* c)
    {
        DrawResult res;
        const DWORD me = GetCurrentThreadId();
        if (_inPresent)
        {
            // Inside Present only the presenting thread is followed.
            if (me != _presentTid)
            {
                ++_cnt.otherThreadDraws;
                return res;
            }
            res.counted = true;
            OnPresentDraw(c);
            return res;
        }
        // The thread of the first draw of a frame is the render thread of that frame; a draw from any other thread is counted
        // and ignored (the immediate context is single threaded by contract).
        if (_tid == 0)
        {
            _tid = me;
            if (_lastTid != 0 && _lastTid != me)
                ++_cnt.renderThreadChanges;
            _lastTid = me;
        }
        else if (me != _tid)
        {
            ++_cnt.otherThreadDraws;
            return res;
        }
        res.counted = true;

        ++_cnt.gameDraws;
        EnsureBackbuffer();

        ShaderRec* rec = ShaderOf(c);
        const uint8_t stage = rec ? rec->stage : kStageNone;
        if (!rec)
            ++_cnt.noPixelShaderDraws;
        else
        {
            ++rec->draws;
            if (rec->firstFrame == 0)
                rec->firstFrame = _frame;
            if (rec->preHook)
                ++_cnt.preHookDraws;
        }

        const bool tagged = stage >= 1 && stage <= kUiStage;
        const bool sampled = rec && (rec->draws <= 16 || (rec->draws & 15) == 0);
        RtInfo rt;
        const bool looked = tagged || sampled || _tracker.WantsRenderTarget();
        if (looked)
        {
            rt = ClassifyRt(c);
            if (rec)
                NoteShaderRt(*rec, rt);
        }

        const uint32_t ordinal = _tracker.Summary().draws + 1;
        res.ordinal = ordinal;
        res.stage = stage;
        res.moved = _tracker.OnDraw({ stage, rt.kind == dfmt::RtKind::Backbuffer });
        res.change = _tracker.LastChange();
        res.cand = _tracker.Current();
        res.r1b = _tracker.R1b();
        res.r1bChanged = _tracker.R1bChanged();
        List(ordinal, stage, looked ? rt : RtInfo {});
        return res;
    }

    void AddHookTicks(int64_t ticks) { _hookTicks += ticks; }

    // ---- frame boundaries ----------------------------------------------------------------------------------------
    // Present entry: the frame's game draws are complete.
    void FreezeFrame()
    {
        _inPresent = true;
        _frozen = true;
        _presentTid = GetCurrentThreadId();
    }

    // Present exit: returns the record of the frame that just ended and resets per-frame state.
    FrameOutcome CompleteFrame()
    {
        FrameOutcome o;
        auto& r = o.record;
        r.frame = _frame;
        r.summary = _tracker.Summary();
        r.candidates = _tracker.Current();
        r.presentDraws = _presentCount;
        r.omittedDraws = _omitted;
        r.omittedPresent = _presentOmitted;
        r.draws = std::move(_lines);
        r.present = std::move(_presentLines);

        const Candidates& c = r.candidates;
        for (auto& d : r.draws)
        {
            d.r1 = d.ordinal == c.r1;
            d.r2 = d.ordinal == c.r2;
            d.r2any = d.ordinal == c.r2any;
            d.r3 = d.ordinal == c.r3;
        }

        o.hookUs = double(_hookTicks) * 1e6 / double(_freq);
        o.r1b = _tracker.R1b();
        Account(r, o);

        // reset for the next frame
        _tracker.Reset();
        _lines.clear();
        _lines.reserve(96);
        _presentLines.clear();
        _presentLines.reserve(16);
        _sceneListed = _uiListed = _afterAnchorListed = _unknownListed = _extraListed = 0;
        _omitted = _presentOmitted = _presentCount = 0;
        _hookTicks = 0;
        _bbValid = false;
        _inPresent = false;
        _frozen = false;
        _tid = 0;
        ++_frame;
        return o;
    }

    // ---- reading -------------------------------------------------------------------------------------------------
    uint64_t Frame() const { return _frame; }
    bool InPresent() const { return _inPresent; }
    const BoundaryTracker& Tracker() const { return _tracker; }
    const Candidates& CurrentCandidates() const { return _tracker.Current(); }
    const Counters& Stats() const { return _cnt; }
    const dfmt::Rolling& DrawsPerFrame() const { return _drawsPerFrame; }
    const dfmt::Rolling& HookUsPerFrame() const { return _hookUsPerFrame; }
    const dfmt::Rolling& PresentDrawsPerFrame() const { return _presentPerFrame; }
    const std::vector<RtEvent>& RtEvents() const { return _rtEvents; }
    uint64_t BackbufferWidth() const { return _bbShape.width; }
    // S10 E: this frame's swapchain backbuffer (valid after the first counted draw of the frame; width 0 = unknown).
    const dfmt::RtShape& BackbufferShape() const { return _bbShape; }

    // Snapshot of every shader record for shaders.csv (render thread: counters are single-writer).
    std::vector<dfmt::ShaderRow> ShaderRows()
    {
        std::lock_guard lock(_mutex);
        std::vector<dfmt::ShaderRow> rows;
        rows.reserve(_all.size());
        for (const ShaderRec* s : _all)
        {
            dfmt::ShaderRow r;
            r.id = s->Id();
            r.hasCrc = s->hasCrc;
            r.crc = s->crc;
            r.bytes = s->bytes;
            r.stage = s->stage;
            r.ui = s->ui;
            r.preHook = s->preHook;
            r.tagAgree = s->tagAgree;
            r.instances = s->instances;
            r.draws = s->draws;
            r.firstFrame = s->firstFrame;
            r.rtSet.assign(s->rt, s->rt + s->rtCount);
            r.rtOverflow = s->rtOverflow;
            r.rtSamples = s->rtSamples;
            r.bbSamples = s->bbSamples;
            rows.push_back(std::move(r));
        }
        return rows;
    }
    uint32_t PreHookShaders() const { return _preCount; }

    // The program record of the pixel shader bound now (null when none): the developer probe names draws by it.
    ShaderRec* PixelShaderRec(ID3D11DeviceContext* c) { return ShaderOf(c); }

    // The render target currently bound, classified against this frame's backbuffer (also used by the snapshot code).
    RtInfo ClassifyRt(ID3D11DeviceContext* c)
    {
        RtInfo info;
        ID3D11RenderTargetView* rtv = nullptr;
        c->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv)
            return info;
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res)
        {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            {
                D3D11_TEXTURE2D_DESC td {};
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
                info.shape = { td.Width, td.Height, uint32_t(td.Format) };
                info.samples = td.SampleDesc.Count;
                info.arraySize = td.ArraySize;
                if (_bbPtr && res == _bbPtr)
                    info.kind = dfmt::RtKind::Backbuffer;
                else if (_bbPtr && info.shape == _bbShape)
                    info.kind = dfmt::RtKind::SameAsBackbuffer;
                else
                    info.kind = dfmt::RtKind::Other;
            }
            else
                info.kind = dfmt::RtKind::Other;
            res->Release();
        }
        rtv->Release();
        return info;
    }

  private:
    struct CacheSlot
    {
        const void* ps = nullptr;
        ShaderRec* rec = nullptr;
        uint32_t epoch = 0;
    };
    static constexpr size_t kCache = 4096;
    static constexpr size_t kMaxLines = 512; // listed draws per frame record, hard cap

    void EnsureBackbuffer()
    {
        if (_bbValid)
            return;
        _bbValid = true;
        _bbPtr = nullptr;
        _bbShape = {};
        if (_bbFn)
        {
            const void* t = nullptr;
            dfmt::RtShape s {};
            if (_bbFn(&t, &s))
            {
                _bbPtr = t;
                _bbShape = s;
            }
        }
    }

    ShaderRec* ShaderOf(ID3D11DeviceContext* c)
    {
        ID3D11PixelShader* ps = nullptr;
        c->PSGetShader(&ps, nullptr, nullptr);
        if (!ps)
            return nullptr;
        ShaderRec* rec = Lookup(ps);
        ps->Release();
        return rec;
    }

    ShaderRec* Lookup(ID3D11PixelShader* ps)
    {
        const uint32_t epoch = _epoch.load(std::memory_order_acquire);
        CacheSlot& slot = _cache[(reinterpret_cast<uintptr_t>(ps) >> 4) & (kCache - 1)];
        if (slot.ps == ps && slot.epoch == epoch)
            return slot.rec;

        ShaderRec* rec = ReadStamp(ps);
        if (!rec)
            rec = PreHookRec(ps);
        else if (!rec->checked)
            CrossCheck(ps, *rec);
        slot = { ps, rec, epoch };
        return rec;
    }

    // A shader without our stamp was created before the hook was armed: no bytecode, so no CRC and no stage.
    ShaderRec* PreHookRec(ID3D11PixelShader* ps)
    {
        auto it = _pre.find(ps);
        if (it != _pre.end())
            return it->second;
        auto* rec = new ShaderRec;
        rec->preHook = true;
        rec->stage = kStageUnknown;
        rec->instances = 1;
        rec->index = ++_preCount;
        _pre[ps] = rec;
        std::lock_guard lock(_mutex);
        _all.push_back(rec);
        return rec;
    }

    // First sight of a program: does the game's own stage tag (FfxivSceneHdr::shaderTag) say what we say?
    void CrossCheck(ID3D11PixelShader* ps, ShaderRec& rec)
    {
        rec.checked = true;
        if (!_gameTag)
            return;
        UINT stage = 0, size = sizeof stage;
        if (FAILED(ps->GetPrivateData(*_gameTag, &size, &stage)))
            stage = 0;
        rec.tagAgree = stage == rec.stage ? 1 : 2;
    }

    void NoteShaderRt(ShaderRec& rec, const RtInfo& rt)
    {
        ++rec.rtSamples;
        if (rt.kind == dfmt::RtKind::Backbuffer)
            ++rec.bbSamples;
        if (rt.kind == dfmt::RtKind::None)
            return;
        for (uint8_t i = 0; i < rec.rtCount; ++i)
            if (rec.rt[i] == rt.shape)
                return;
        if (rec.rtCount < dfmt::kMaxRtSet)
            rec.rt[rec.rtCount++] = rt.shape;
        else
            ++rec.rtOverflow;
    }

    void OnPresentDraw(ID3D11DeviceContext* c)
    {
        ++_presentCount;
        if (_presentLines.size() >= dfmt::kMaxPresentDraws)
        {
            ++_presentOmitted;
            return;
        }
        EnsureBackbuffer();
        ShaderRec* rec = ShaderOf(c);
        dfmt::PresentDrawLine p;
        p.seq = _presentCount;
        p.id = rec ? rec->Id() : "-";
        p.stage = rec ? rec->stage : kStageNone;
        const RtInfo rt = ClassifyRt(c);
        p.rt = rt.kind;
        p.shape = rt.shape;
        _presentLines.push_back(std::move(p));
    }

    void List(uint32_t ordinal, uint8_t stage, const RtInfo& rt)
    {
        const Candidates& cur = _tracker.Current();
        const bool candidate = cur.r1 == ordinal || cur.r2 == ordinal || cur.r2any == ordinal;
        const bool tagged = (stage >= 1 && stage <= kUiStage) || stage == kStageUnknown;
        bool list = false;

        if (stage >= 1 && stage <= 5)
            list = _sceneListed < 24 ? (++_sceneListed, true) : false;
        else if (stage == kUiStage)
            list = _uiListed < 16 ? (++_uiListed, true) : false;
        else if (stage == kStageUnknown)
            list = _unknownListed < 8 ? (++_unknownListed, true) : false;

        if (stage == 5)
            _afterAnchorListed = 0;
        else if (!list && _tracker.Summary().lastStage5 != 0 && _afterAnchorListed < 12)
        {
            list = true;
            ++_afterAnchorListed;
        }

        if (!list && candidate && _extraListed < 16)
        {
            list = true;
            ++_extraListed;
        }

        // Hard cap: the tracker only resets in CompleteFrame (bridge Present exit). With the feature armed by Wanted() and no bridge
        // Present (frame generation off) the per-frame counters would never reset and the list would grow for ever (S10 E).
        if (list && _lines.size() < kMaxLines)
            _lines.push_back({ ordinal, stage, rt.kind, rt.shape, false, false, false, false });
        else if (tagged || list)
            ++_omitted;
    }

    // Per-frame statistics and the anomaly flag.
    void Account(const dfmt::FrameRecord& r, FrameOutcome& o)
    {
        const auto& s = r.summary;
        const auto& c = r.candidates;
        ++_cnt.frames;
        _cnt.presentDraws += r.presentDraws;
        _drawsPerFrame.Add(double(s.draws));
        _hookUsPerFrame.Add(o.hookUs);
        _presentPerFrame.Add(double(r.presentDraws));
        if (s.sawSceneStage)
            ++_cnt.framesWithScene;
        if (s.uiDraws)
            ++_cnt.framesWithUi;
        if (c.r1)
            ++_cnt.framesR1;
        if (c.r2)
            ++_cnt.framesR2;
        if (c.r3)
            ++_cnt.framesR3;
        if (c.r1 && c.r2)
        {
            if (c.r1 == c.r2)
                ++_cnt.framesSame;
            else if (c.r1 < c.r2)
                ++_cnt.framesR1First;
            else
                ++_cnt.framesR2First;
        }
        if (s.sawSceneStage && !c.r3)
            ++_cnt.framesNone;
        if (o.r1b != 0)
        {
            ++_cnt.framesR1b;
            if (o.r1b != c.r1)
                ++_cnt.framesR1bLater;
        }
        else if (s.sawSceneStage && c.r1 != 0)
            ++_cnt.framesUiNoR1b;
        if (s.uiBeforeLastScene)
            ++_cnt.framesUiBeforeScene;
        if (s.unknownDraws)
            ++_cnt.framesWithUnknown;
        if (s.r1Supersessions || s.r2Supersessions)
            ++_cnt.framesSuperseded;

        // render target at each candidate, from the listed draws
        const uint32_t cands[4] { 0, c.r1, c.r2, c.r3 };
        for (uint8_t rule = 1; rule <= 3; ++rule)
        {
            if (!cands[rule])
                continue;
            for (const auto& d : r.draws)
                if (d.ordinal == cands[rule])
                {
                    ++_cnt.rtAt[rule][size_t(d.rt)];
                    NoteRtEvent(rule, d.rt, d.shape, r.frame);
                    break;
                }
        }

        if (c.r1 && c.r2 && c.r1 != c.r2)
            o.anomaly = "r1r2differ";
        else if (s.sawSceneStage && !c.r3)
            o.anomaly = "no_boundary";
        else if (s.uiBeforeLastScene)
            o.anomaly = "ui_before_scene";
        else if (s.unknownDraws)
            o.anomaly = "unknown_shaders";
        o.listWorthy = o.anomaly[0] != '\0';
    }

    void NoteRtEvent(uint8_t rule, dfmt::RtKind kind, const dfmt::RtShape& shape, uint64_t frame)
    {
        for (auto& e : _rtEvents)
            if (e.rule == rule && e.kind == kind && e.shape == shape)
            {
                ++e.count;
                return;
            }
        if (_rtEvents.size() < 64)
            _rtEvents.push_back({ rule, kind, shape, frame, 1 });
    }

    // construction-time constants and wiring
    int64_t _freq = 1;
    const GUID* _gameTag = nullptr;
    BackbufferFn _bbFn = nullptr;
    DWORD _tid = 0, _lastTid = 0, _presentTid = 0;

    // shader registry
    std::mutex _mutex;
    std::unordered_map<uint32_t, ShaderRec*> _byCrc;
    std::vector<ShaderRec*> _all;
    std::atomic<uint32_t> _epoch { 1 };
    std::array<CacheSlot, kCache> _cache {};
    std::unordered_map<const void*, ShaderRec*> _pre;
    uint32_t _preCount = 0;
    ShaderRec _stageRecs[kUiStage + 1]; // [1..kUiStage]: the always-on stamp's shared records (not in _all / _byCrc)

    // per-frame state (render thread)
    uint64_t _frame = 1;
    bool _inPresent = false, _frozen = false, _bbValid = false;
    const void* _bbPtr = nullptr;
    dfmt::RtShape _bbShape {};
    BoundaryTracker _tracker;
    std::vector<dfmt::DrawLine> _lines;
    std::vector<dfmt::PresentDrawLine> _presentLines;
    uint32_t _sceneListed = 0, _uiListed = 0, _afterAnchorListed = 0, _unknownListed = 0, _extraListed = 0;
    uint32_t _omitted = 0, _presentOmitted = 0, _presentCount = 0;
    int64_t _hookTicks = 0;

    // cumulative
    Counters _cnt;
    dfmt::Rolling _drawsPerFrame, _hookUsPerFrame, _presentPerFrame;
    std::vector<RtEvent> _rtEvents;
};

// Draws that reach the hook from a context other than the latched immediate one (a second immediate context on another
// device, or a deferred context recorded on any thread). Thread safe: deferred contexts record on worker threads.
class ContextTable
{
  public:
    void Note(ID3D11DeviceContext* c, uint64_t frame)
    {
        const D3D11_DEVICE_CONTEXT_TYPE type = c->GetType();
        std::lock_guard lock(_mutex);
        ++_draws[type == D3D11_DEVICE_CONTEXT_DEFERRED ? 1 : 0];
        for (size_t i = 0; i < _count; ++i)
            if (_rec[i].ptr == c)
            {
                ++_rec[i].draws;
                return;
            }
        if (_count < _rec.size())
            _rec[_count++] = { c, type, 1, frame };
    }

    struct Snapshot
    {
        uint64_t immediateDraws = 0, deferredDraws = 0;
        std::vector<ContextRec> contexts;
    };

    Snapshot Get()
    {
        std::lock_guard lock(_mutex);
        Snapshot s;
        s.immediateDraws = _draws[0];
        s.deferredDraws = _draws[1];
        s.contexts.assign(_rec.begin(), _rec.begin() + static_cast<std::ptrdiff_t>(_count));
        return s;
    }

  private:
    std::mutex _mutex;
    uint64_t _draws[2] {};
    std::array<ContextRec, 8> _rec {};
    size_t _count = 0;
};
} // namespace ext::ffxiv::ui
