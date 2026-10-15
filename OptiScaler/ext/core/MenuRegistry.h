#pragma once
// Fork cards in the OptiScaler menu (docs/SEAMS.md section 3.6).
//
// A feature registers a card from its own directory (static AutoRegister, or Register() from an init function that
// runs before the first menu frame):
//
//   static void DrawNrCard(ext::menu::Ctx& c) { ext::menu::SectionTitle("DLSS NR"); ... }
//   static bool NrVisible(const ext::menu::Ctx& c) { return ext::Active(ext::F::Nr); }
//   static ext::menu::AutoRegister reg { { "nr_settings", "Upscaling", "DLSS Neural Rendering", DrawNrCard,
//                                         "output_scaling" /* after */, NrVisible } };
//
// The draw callback runs inside an upstream ScopedCard, so tab filtering and Custom Tab pinning work like for any
// upstream card. `tab` is the label of an existing upstream tab. `after` is the id of the upstream (or earlier fork)
// card to follow; when it is null or unknown the card goes after the last card of its tab. Cards with the same
// anchor keep their registration order. At most kMaxCards cards.

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class State;
class Config;
class IFeature;
struct ImGuiIO;

namespace ext::menu
{
inline constexpr size_t kMaxCards = 32;

// Narrow view of upstream's RenderMenuContext. Built from the singletons because RenderMenuContext is defined in
// menu_common.cpp; fields are added here (with their source) when a card needs them.
struct Ctx
{
    State& state;
    Config* config;
    ImGuiIO& io;
    IFeature* currentFeature; // state.currentFeature, like upstream's ctx
    float menuResScale = 1.0f; // upstream's ctx.menuResScale (the template seams copy it from the real context)
    void* raw = nullptr;       // upstream RenderMenuContext*, opaque; only ext::Access may use it
};

// Context of the seams below that take upstream's RenderMenuContext: a template deduced from the real type, so no fork
// code names it (same trick as MergeBoxes).
template <class RenderContext> Ctx MakeCtx(RenderContext& rc)
{
    return Ctx { rc.state, rc.config, rc.io, rc.currentFeature, rc.menuResScale, &rc };
}

struct Card
{
    const char* id;   // saved to the config (pinning), unique
    const char* tab;  // label of the upstream tab: "Upscaling", "Frame Gen", "Latency & FPS", "Textures",
                      // "Compatibility", "Menu", "Quirks / ASI", "Help / Status"
    const char* name; // shown in the Custom Tab picker
    void (*draw)(Ctx&);
    const char* after = nullptr;                 // id of the card to follow
    bool (*visible)(const Ctx&) = nullptr;       // null = always visible
    bool pinnable = true;
};

void Register(const Card& card);

struct AutoRegister
{
    explicit AutoRegister(const Card& card) { Register(card); }
};

// ---- Shortcuts, keybind rows, overlays and shader-time rows (seams of menu_common.cpp, catalog group `menu`) ---------
// A feature registers these from its own directory like a card (static initialisation or init code). The seams call
// them: PollShortcuts (UpdateManualInput), HandleShortcuts (HandleMenuShortcuts), KeybindRows (RenderKeybindSettings),
// WantsFrame / DrawOverlays (BeginMenuFrameIfNeeded / RenderMenu), ShaderTimeRows (RenderShaderTimes).

// A key that toggles something: polled with the menu's own CheckShortcut, handled in the shortcut pass, with a row in
// the Keybinds card. `slot` is the persisted Keybind id: keep it stable (NR toggle 14, HDR screenshot 15).
struct Shortcut
{
    const char* name;     // row label in the Keybinds card
    int slot;             // Keybind id
    void* (*option)();    // the config key, a CustomOptional<int>* (upstream's type, not named here)
    const char* pollLog;  // logged when the key was seen
    void (*onPress)(Ctx&); // runs once per press, in the shortcut pass of the menu frame
    bool pressed = false; // set by PollShortcuts
};
inline constexpr size_t kMaxShortcuts = 8;
void RegisterShortcut(const Shortcut& shortcut);

// An overlay drawn every menu frame (also while the menu is closed). `wantsFrame` keeps the menu frame alive for it.
struct Overlay
{
    void (*draw)(Ctx&);
    bool (*wantsFrame)() = nullptr;
};
inline constexpr size_t kMaxOverlays = 8;
void RegisterOverlay(const Overlay& overlay);

// A row of the "Per shader GPU times" table. `ms` is empty when there is nothing to show.
struct TimeRow
{
    const char* label;
    std::optional<double> (*ms)();
};
inline constexpr size_t kMaxTimeRows = 4;
void RegisterTimeRow(const TimeRow& row);

namespace detail
{
// Registered cards in registration order
const Card* Cards(size_t& count);
// Draw slot `index` (trampoline target); `menuResScale` is the context's
void RunCard(size_t index, void* rawContext, float menuResScale);

Shortcut* Shortcuts(size_t& count);
const Overlay* Overlays(size_t& count);

// Argument type of a member function taking one reference (Keybind::Render(CustomOptional<int>&))
template <class F> struct RenderArg;
template <class C, class A> struct RenderArg<void (C::*)(A&)>
{
    using type = A;
};

// UpdateManualInput helper of PollShortcuts: `check(fn, vk, flag, log)` forwards to the menu's CheckShortcut lambda
void PollShortcuts(void (*check)(void* fn, int vk, bool& flag, const char* log), void* fn);

template <class F> struct DrawArg;
template <class R> struct DrawArg<void (*)(R&)>
{
    using type = R;
};
} // namespace detail

// The seam in MenuCommon::GetMenuBoxes (declared through ext/Seams.h). MenuBox and RenderMenuContext are private to
// menu_common.cpp, so this is a template deduced from the real array there: no fork code names those types.
// Returns `base` while no card is registered, otherwise a merged array that lives for the whole process.
template <class Box> const Box* MergeBoxes(const Box* base, size_t& count)
{
    using Ctx = typename detail::DrawArg<decltype(Box::draw)>::type;
    using DrawFn = decltype(Box::draw);

    static std::mutex mutex;
    static std::vector<Box> merged;
    static bool done = false;

    std::scoped_lock lock(mutex);

    if (!done)
    {
        size_t cardCount = 0;
        const Card* cards = detail::Cards(cardCount);

        if (cardCount == 0)
            return base; // registration may still come, try again on the next call

        // MenuBox::draw has no state: one non-capturing trampoline per card slot
        static const auto slots = []<size_t... I>(std::index_sequence<I...>)
        {
            return std::array<DrawFn, sizeof...(I)> { (+[](Ctx& c) { detail::RunCard(I, &c, c.menuResScale); })... };
        }(std::make_index_sequence<kMaxCards> {});

        struct Entry
        {
            Box box;
            const char* after; // anchor of a fork card
            bool fork;
        };

        std::vector<Entry> entries;
        for (size_t i = 0; i < count; i++)
            entries.push_back({ base[i], nullptr, false });

        for (size_t c = 0; c < cardCount && c < slots.size(); c++)
        {
            const Card& card = cards[c];
            Entry e { Box { card.id, card.tab, card.name, slots[c], nullptr, card.pinnable }, card.after, true };

            size_t pos = SIZE_MAX; // insertion index

            // after the anchor, behind fork cards that already follow the same anchor
            if (card.after != nullptr)
            {
                for (size_t i = 0; i < entries.size(); i++)
                {
                    if (strcmp(entries[i].box.id, card.after) != 0)
                        continue;

                    pos = i + 1;
                    while (pos < entries.size() && entries[pos].fork && entries[pos].after != nullptr &&
                           strcmp(entries[pos].after, card.after) == 0)
                        pos++;
                    break;
                }
            }

            // unknown or no anchor: after the last card of the tab, else at the end
            if (pos == SIZE_MAX)
            {
                pos = entries.size();
                for (size_t i = entries.size(); i > 0; i--)
                {
                    if (strcmp(entries[i - 1].box.tab, card.tab) == 0)
                    {
                        pos = i;
                        break;
                    }
                }
            }

            entries.insert(entries.begin() + pos, e);
        }

        for (const auto& e : entries)
            merged.push_back(e.box);

        done = true;
    }

    count = merged.size();
    return merged.data();
}

// UpdateManualInput, inside the `!capturingKey && canAcceptInputs` block: `check(vk, flag, logMessage)` is the menu's
// own CheckShortcut lambda.
template <class CheckFn> void PollShortcuts(CheckFn& check)
{
    detail::PollShortcuts(
        [](void* fn, int vk, bool& flag, const char* log) { (*static_cast<CheckFn*>(fn))(vk, flag, log); }, &check);
}

// HandleMenuShortcuts: runs the handler of every shortcut pressed since the last frame.
template <class RenderContext> void HandleShortcuts(RenderContext& rc)
{
    size_t count = 0;
    Shortcut* shortcuts = detail::Shortcuts(count);
    if (count == 0)
        return;

    Ctx ctx = MakeCtx(rc);
    for (size_t i = 0; i < count; i++)
    {
        if (!shortcuts[i].pressed)
            continue;

        shortcuts[i].pressed = false;
        shortcuts[i].onPress(ctx);
    }
}

// RenderKeybindSettings, after the last upstream row. `KeybindRow` is menu_common.cpp's TU-local Keybind class; the
// rows live in a function-local static so their capture state survives between frames.
template <class KeybindRow> void KeybindRows()
{
    static std::vector<KeybindRow> rows;

    size_t count = 0;
    Shortcut* shortcuts = detail::Shortcuts(count);
    while (rows.size() < count)
        rows.emplace_back(shortcuts[rows.size()].name, shortcuts[rows.size()].slot);

    using Option = typename detail::RenderArg<decltype(&KeybindRow::Render)>::type; // upstream's CustomOptional<int>
    for (size_t i = 0; i < count; i++)
        rows[i].Render(*static_cast<Option*>(shortcuts[i].option()));
}

// BeginMenuFrameIfNeeded (OR term): true when a registered overlay needs the frame even with the menu closed.
bool WantsFrame();

// RenderMenu, right after RenderPerformanceOverlay.
template <class RenderContext> void DrawOverlays(RenderContext& rc)
{
    size_t count = 0;
    const Overlay* overlays = detail::Overlays(count);
    if (count == 0)
        return;

    Ctx ctx = MakeCtx(rc);
    for (size_t i = 0; i < count; i++)
        overlays[i].draw(ctx);
}

// RenderShaderTimes: the registered rows that have a value this frame. A non-empty result makes the extra-shaders block
// appear (`hasExtra || !rows.empty()`); DrawShaderTimeRows adds them to the table, after upstream's extra shaders.
struct ExtraTimes
{
    struct Row
    {
        const char* label;
        double ms;
    };
    std::array<Row, kMaxTimeRows> rows {};
    size_t count = 0;

    bool empty() const { return count == 0; }
};
ExtraTimes ShaderTimeRows();
void DrawShaderTimeRows(const ExtraTimes& times);
} // namespace ext::menu
