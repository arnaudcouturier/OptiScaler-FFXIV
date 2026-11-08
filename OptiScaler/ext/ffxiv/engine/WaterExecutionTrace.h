#pragma once

// Bounded, opt-in water draw execution trace. All D3D calls are on the
// immediate-context draw thread; pending statistics are polled before a new
// candidate starts, never between a private pass and its native draw.
#include "ext/ffxiv/chara/CharaProbeDump.h"
#include "ext/core/D3D11DrawDispatch.h"

#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <optional>
#include <thread>
#include <atomic>

namespace ext::ffxiv::engine::water_trace
{
inline constexpr uint32_t kQueryCount = 64;
inline constexpr uint32_t kRowLimit = 65536;
inline constexpr auto kDuration = std::chrono::seconds(30);

struct Row
{
    struct Entry { uintptr_t context, literal, current, chosen; uint32_t api, slot; bool trampoline, fresh; uint8_t phase; };
    D3D11_QUERY_DATA_PIPELINE_STATISTICS original {};
    uint64_t id = 0;
    uint64_t frame = 0;
    uint64_t timestampUtcMs = 0;
    uintptr_t context = 0;
    size_t thread = 0;
    uint32_t mode = 0;
    uint64_t privateDraws = 0;
    uint32_t callbackCalls = 0;
    bool readerBound = false;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS gpu {};
    std::array<Entry, 16> entries {};
    uint32_t entryCount = 0;
    uint32_t privateEvents = 0, originalEvents = 0, otherPrivateEvents = 0, innerBegins = 0;
    bool entryOverflow = false;
    uint32_t complete = 0;
};

struct Slot { Microsoft::WRL::ComPtr<ID3D11Query> query, privateQuery; Row row; bool pending = false; bool hasPrivate = false; };
struct State
{
    std::array<Slot, kQueryCount> slots;
    ID3D11DeviceContext* context = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    std::thread::id thread;
    std::chrono::steady_clock::time_point start;
    uint64_t next = 0;
    uint32_t eligible = 0, samples = 0, completed = 0, dropped = 0, pending = 0, predicateUnavailable = 0;
    std::ofstream csv;
    bool active = false, finished = false;
};
inline State& S() { static State* s = new State; return *s; }
inline void Unavailable(State& s, const char* reason, HRESULT hr = S_OK)
{
    s.finished = true;
    if (s.csv)
    {
        s.csv << "#schema=water-execution-v2\n#summary,query_unavailable=" << reason << ",hr=" << static_cast<uint32_t>(hr) << '\n';
        s.csv.close();
    }
    LOG_INFO("Water execution trace unavailable: reason={}, hr=0x{:08X}", reason, static_cast<uint32_t>(hr));
}
inline void ObserveForward(void* user, const ext::core::draw_dispatch::ForwardEvent& e) noexcept
{
    auto& row = *static_cast<Row*>(user);
    if (e.phase == 1) ++row.originalEvents;
    else if (e.phase == 2) ++row.privateEvents;
    else ++row.otherPrivateEvents;
    if (row.entryCount >= row.entries.size()) { row.entryOverflow = true; return; }
    row.entries[row.entryCount++] = { reinterpret_cast<uintptr_t>(e.context), reinterpret_cast<uintptr_t>(e.literal),
        reinterpret_cast<uintptr_t>(e.current), reinterpret_cast<uintptr_t>(e.chosen), uint32_t(e.api), e.slot, e.trampoline, e.freshReentry, e.phase };
}
inline thread_local Slot* activeSlot = nullptr;
inline thread_local ID3D11DeviceContext* activeContext = nullptr;
inline std::atomic<bool> disarmRequested { false };
inline std::atomic<uint64_t> nextGeneration { 0 };
inline std::atomic<size_t> nextThreadToken { 1 };
inline std::atomic<size_t> ownerThreadToken { 0 };
inline size_t ThreadToken() noexcept
{
    thread_local const size_t token = nextThreadToken.fetch_add(1, std::memory_order_relaxed);
    return token;
}
inline void BeginPrivateDraw(ID3D11DeviceContext* context) noexcept
{
    if (!activeSlot || context != activeContext || activeSlot->hasPrivate) return;
    context->Begin(activeSlot->privateQuery.Get());
    activeSlot->hasPrivate = true;
    ++activeSlot->row.innerBegins;
}
inline void EndPrivateDraw(ID3D11DeviceContext* context) noexcept
{
    if (!activeSlot || context != activeContext || !activeSlot->hasPrivate) return;
    context->End(activeSlot->privateQuery.Get());
    activeSlot->hasPrivate = false;
}
class PrivateDrawScope
{
public:
    explicit PrivateDrawScope(ID3D11DeviceContext* context) noexcept : _context(context), _phase(2) { BeginPrivateDraw(context); }
    ~PrivateDrawScope() noexcept { EndPrivateDraw(_context); }
    PrivateDrawScope(const PrivateDrawScope&) = delete;
    PrivateDrawScope& operator=(const PrivateDrawScope&) = delete;
private:
    ID3D11DeviceContext* _context;
    ext::core::draw_dispatch::ForwardPhaseScope _phase;
};

inline void FlushReady(State& s)
{
    if (!s.context) return;
    for (auto& slot : s.slots)
    {
        if (!slot.pending) continue;
        const HRESULT hr = s.context->GetData(slot.query.Get(), &slot.row.gpu, sizeof(slot.row.gpu), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) continue;
        D3D11_QUERY_DATA_PIPELINE_STATISTICS inner {};
        const HRESULT innerHr = slot.row.innerBegins
            ? s.context->GetData(slot.privateQuery.Get(), &inner, sizeof(inner), D3D11_ASYNC_GETDATA_DONOTFLUSH) : S_OK;
        if (innerHr == S_FALSE) continue;
        slot.pending = false;
        --s.pending;
        if (SUCCEEDED(hr) && SUCCEEDED(innerHr))
        {
            ++s.completed;
            slot.row.complete = 1;
            auto& r = slot.row;
            const bool subtractionOk = !r.innerBegins ||
                (r.gpu.IAVertices >= inner.IAVertices && r.gpu.IAPrimitives >= inner.IAPrimitives &&
                 r.gpu.VSInvocations >= inner.VSInvocations && r.gpu.GSInvocations >= inner.GSInvocations &&
                 r.gpu.GSPrimitives >= inner.GSPrimitives && r.gpu.CInvocations >= inner.CInvocations &&
                 r.gpu.CPrimitives >= inner.CPrimitives && r.gpu.PSInvocations >= inner.PSInvocations &&
                 r.gpu.HSInvocations >= inner.HSInvocations && r.gpu.DSInvocations >= inner.DSInvocations &&
                 r.gpu.CSInvocations >= inner.CSInvocations);
            const bool attributionExact = subtractionOk && !r.entryOverflow && r.callbackCalls == 1 && r.otherPrivateEvents == 0 &&
                ((r.innerBegins == 0 && r.privateDraws == 0) || (r.innerBegins == 1 && r.privateDraws == 1));
            if (attributionExact)
            {
                r.original = r.gpu;
                if (r.innerBegins)
                {
#define WATER_SUB(field) r.original.field -= inner.field
                    WATER_SUB(IAVertices); WATER_SUB(IAPrimitives); WATER_SUB(VSInvocations); WATER_SUB(GSInvocations);
                    WATER_SUB(GSPrimitives); WATER_SUB(CInvocations); WATER_SUB(CPrimitives); WATER_SUB(PSInvocations);
                    WATER_SUB(HSInvocations); WATER_SUB(DSInvocations); WATER_SUB(CSInvocations);
#undef WATER_SUB
                }
            }
            s.csv << r.id << ',' << r.timestampUtcMs << ',' << r.frame << ',' << r.context << ',' << r.thread << ',' << r.mode << ',' << r.readerBound << ',' << r.callbackCalls << ',' << r.privateDraws << ','
                  << r.gpu.IAVertices << ',' << r.gpu.IAPrimitives << ',' << r.gpu.VSInvocations << ',' << r.gpu.GSInvocations << ','
                  << r.gpu.GSPrimitives << ',' << r.gpu.CInvocations << ',' << r.gpu.CPrimitives << ',' << r.gpu.PSInvocations << ','
                  << r.gpu.HSInvocations << ',' << r.gpu.DSInvocations << ',' << r.gpu.CSInvocations << ','
                  << inner.IAVertices << ',' << inner.IAPrimitives << ',' << inner.VSInvocations << ',' << inner.PSInvocations << ','
                  << r.original.IAVertices << ',' << r.original.IAPrimitives << ',' << r.original.VSInvocations << ',' << r.original.PSInvocations << ','
                  << r.innerBegins << ',' << r.privateEvents << ',' << r.originalEvents << ',' << r.otherPrivateEvents << ','
                  << (attributionExact ? "exact" : (subtractionOk ? "ambiguous" : "underflow")) << ',' << r.entryOverflow << ",\"";
            for (uint32_t i = 0; i < r.entryCount; ++i)
            {
                const auto& e = r.entries[i];
                if (i) s.csv << '|';
                s.csv << e.context << ':' << e.api << ':' << e.slot << ':' << e.literal << ':' << e.current << ':' << e.chosen << ':'
                      << e.trampoline << ':' << e.fresh << ':' << unsigned(e.phase);
            }
            const bool zeroWork = r.gpu.IAVertices == 0 && r.gpu.IAPrimitives == 0 && r.gpu.VSInvocations == 0 && r.gpu.PSInvocations == 0;
            const bool originalZero = r.original.IAVertices == 0 && r.original.VSInvocations == 0 && r.original.PSInvocations == 0;
            s.csv << "\"," << zeroWork << ',' << (attributionExact ? (originalZero ? "true" : "false") : "unknown") << ','
                  << r.complete << ",complete;overlap_allowed\n";
        }
        else ++s.dropped;
    }
}

inline void Finish(State& s)
{
    if (s.finished) return;
    if (s.active) FlushReady(s); // persist every result already available before reporting the remaining incomplete rows
    s.finished = true;
    s.active = false;
    if (s.csv)
    {
        s.csv.flush();
        LOG_INFO("Water execution trace completed: samples={}, completed={}, dropped={}, incomplete={}, predicate_unavailable={}",
                 s.samples, s.completed, s.dropped, s.pending, s.predicateUnavailable);
        s.csv << "#summary,samples=" << s.samples << ",completed=" << s.completed << ",dropped=" << s.dropped << ",incomplete=" << s.pending
              << ",predicate_unavailable=" << s.predicateUnavailable << ",same_type_overlap=allowed_by_D3D11_3_spec\n";
        s.csv.close();
    }
    for (auto& slot : s.slots) { slot.pending = false; slot.query.Reset(); slot.privateQuery.Reset(); }
    s.device.Reset();
    s.context = nullptr;
    ownerThreadToken.store(0, std::memory_order_release);
}

inline void Disarm()
{
    State& s = S();
    if (s.active) Finish(s);
    else if (!s.finished) return;
    s = State {};
}

// Publish/Release may run away from the immediate-context owner. They only request
// cleanup; the next owner-thread water candidate performs the D3D releases.
inline void RequestDisarm() noexcept
{
    disarmRequested.store(true, std::memory_order_release);
    if (ThreadToken() == ownerThreadToken.load(std::memory_order_acquire) && activeSlot == nullptr)
    {
        disarmRequested.store(false, std::memory_order_release);
        Disarm();
    }
}

class Scope
{
public:
    Scope(ID3D11DeviceContext* context, bool enabled, uint64_t frame, uint64_t privateDraws, uint32_t mode)
        : Scope(context, true, enabled, frame, privateDraws, mode) {}

    Scope(ID3D11DeviceContext* context, bool candidate, bool enabled, uint64_t frame, uint64_t privateDraws, uint32_t mode)
        : _s(S())
    {
        const size_t currentThreadToken = ThreadToken();
        const size_t owner = ownerThreadToken.load(std::memory_order_acquire);
        if (owner != 0 && owner != currentThreadToken)
        {
            disarmRequested.store(true, std::memory_order_release);
            return; // never query or use the immediate context from a nonowner thread
        }
        // A nested capture must not consume a pending parent disarm or touch its
        // query slot/observer. The active owner's destructor services the request.
        if (activeSlot != nullptr) return;
        if (disarmRequested.exchange(false, std::memory_order_acq_rel)) Disarm();
        if (!candidate) return;
        if (!enabled) return;
        if (!context || _s.finished) return;
        if (_s.active && _s.context != context)
        {
            Finish(_s); // preserve this generation; a changed context never truncates/resumes it
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11Device> incomingDevice;
        context->GetDevice(&incomingDevice);
        if (_s.active && (_s.thread != std::this_thread::get_id() || _s.device.Get() != incomingDevice.Get()))
        {
            Finish(_s);
            return;
        }
        if (!_s.active)
        {
            const D3D11_DEVICE_CONTEXT_TYPE type = context->GetType();
            if (type != D3D11_DEVICE_CONTEXT_IMMEDIATE) { Unavailable(_s, "not-immediate-context"); return; }
            Microsoft::WRL::ComPtr<ID3D11Device> device = incomingDevice;
            if (!device) { Unavailable(_s, "missing-device"); return; }
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
            device->GetImmediateContext(&immediate);
            if (immediate.Get() != context) { Unavailable(_s, "not-device-immediate-context"); return; }
            auto root = ext::ffxiv::chara::probe::dump::OutputRoot();
            if (root.empty()) { Unavailable(_s, "output-root-unavailable"); return; }
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            if (ec) { Unavailable(_s, "create-output-root-failed", HRESULT_FROM_WIN32(ec.value())); return; }
            const uint64_t generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64_t startedUtcNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const auto path = root / ("water-execution-trace-" + std::to_string(startedUtcNs) + "-" + std::to_string(generation) + ".csv");
            _s.csv.open(path, std::ios::out | std::ios::trunc);
            if (!_s.csv) { Unavailable(_s, "open-output-failed"); return; }
            for (auto& slot : _s.slots)
            {
                D3D11_QUERY_DESC desc { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
                HRESULT hr = device->CreateQuery(&desc, &slot.query);
                if (SUCCEEDED(hr)) hr = device->CreateQuery(&desc, &slot.privateQuery);
                if (FAILED(hr))
                {
                    for (auto& partial : _s.slots) { partial.query.Reset(); partial.privateQuery.Reset(); }
                    Unavailable(_s, "create-pipeline-statistics-query-failed", hr);
                    return;
                }
            }
            _s.context = context;
            _s.device = device;
            _s.thread = std::this_thread::get_id();
            ownerThreadToken.store(currentThreadToken, std::memory_order_release);
            _s.start = std::chrono::steady_clock::now();
            _s.csv << "#schema=water-execution-v4\nrow,timestamp_utc_ms,frame,context,thread,ssr_mode,ssr_reader_bound,callback_calls,private_draws,outer_ia_vertices,outer_ia_primitives,outer_vs_invocations,outer_gs_invocations,outer_gs_primitives,outer_c_invocations,outer_c_primitives,outer_ps_invocations,outer_hs_invocations,outer_ds_invocations,outer_cs_invocations,private_ia_vertices,private_ia_primitives,private_vs_invocations,private_ps_invocations,original_ia_vertices,original_ia_primitives,original_vs_invocations,original_ps_invocations,inner_begins,private_forward_events,original_forward_events,other_private_forward_events,attribution,entry_overflow,entry_events,outer_zero_work,original_zero_work,complete,query_validity\n";
            _s.active = true;
            LOG_INFO("Water execution trace armed: {}", path.string());
        }
        FlushReady(_s); // query polling is outside the private-pass/native-draw interval
        if (_s.eligible >= kRowLimit || std::chrono::steady_clock::now() - _s.start >= kDuration)
        { Finish(_s); return; }
        ++_s.eligible;
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
        BOOL predicateValue = FALSE;
        context->GetPredication(&predicate, &predicateValue);
        if (predicate) { ++_s.predicateUnavailable; return; }
        Slot* free = nullptr;
        for (auto& slot : _s.slots) if (!slot.pending) { free = &slot; break; }
        if (!free) { ++_s.dropped; return; }
        free->row = Row {};
        free->row.id = ++_s.next;
        free->row.frame = frame;
        free->row.timestampUtcMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        free->row.privateDraws = privateDraws;
        free->row.context = reinterpret_cast<uintptr_t>(context);
        free->row.thread = std::hash<std::thread::id> {}(std::this_thread::get_id());
        free->row.mode = mode;
        _s.context->Begin(free->query.Get());
        _slot = free;
        activeSlot = free;
        activeContext = context;
        _observer.emplace(&_slot->row, &ObserveForward);
        ++_s.samples;
    }

    bool Active() const noexcept { return _slot != nullptr; }
    void NativeCallback() noexcept { if (_slot) ++_slot->row.callbackCalls; }
    void SetReaderBound(bool bound) noexcept { if (_slot) _slot->row.readerBound = bound; }
    void SetPrivateDraws(uint64_t count) noexcept { if (_slot) _slot->row.privateDraws = count; }
    ~Scope()
    {
        if (_slot)
        {
            _observer.reset();
            if (_slot->hasPrivate) EndPrivateDraw(_s.context);
            activeSlot = nullptr;
            activeContext = nullptr;
            _s.context->End(_slot->query.Get());
            _slot->pending = true;
            ++_s.pending;
            if (disarmRequested.exchange(false, std::memory_order_acq_rel))
                Disarm(); // a disable requested mid-draw is released on the owner thread as soon as the scope unwinds
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    State& _s;
    Slot* _slot = nullptr;
    std::optional<ext::core::draw_dispatch::ForwardObserverScope> _observer;
};
} // namespace ext::ffxiv::engine::water_trace
