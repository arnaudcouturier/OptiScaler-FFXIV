// The HUD-less UX wiring the ImGui and engine glue cannot be tested for offline: the text pins of the pieces the pure tests prove
// the behaviour of. Each pin is a rule a broken edit would break:
//   - the paste gate of BOTH SetPasteEnabled sites is Core::PasteGate(_tint) (the tint opens it while the guard is tripped; the old
//     PastVisible() call would silently drop the tint again),
//   - the HUD-less card prints the guard status line, keeps the coverage guard under its own sub-heading, prints the guard-limit
//     warning next to the guard slider, logs the settled slider values once, and its Reset tooltip says it resets the guard,
//   - the frame-pacing card registers the mark key in the Keybinds section (slot 19) and its handler writes the "user mark N" line,
//   - the recorder keeps 9 files of 16 MiB (the documented 144 MiB session bound),
//   - the probe interval: its slider, settle log and status line in the card, the engine through the tested pure rules, the skip
//     forwarded by the FFXIV source only while it delivers frames,
//   - the paste images' lifetime: the producer's cleanup read is recorded and the retire collection uses the ring's Idle rule.
#include <cstdio>
#include <fstream>
#include <string>

static int g_failed = 0;
#define CHECK(cond)                                                                                              \
    do                                                                                                           \
    {                                                                                                            \
        if (!(cond))                                                                                             \
        {                                                                                                        \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                  \
            ++g_failed;                                                                                          \
        }                                                                                                        \
    } while (0)

namespace
{
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The text without comments and white space
std::string Squash(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n')
                ++i;
            continue;
        }
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            out += text[i];
    }
    return out;
}

size_t Count(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
        ++n;
    return n;
}

bool Has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void TestEngineGate(const std::string& repo)
{
    const std::string engine = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/HudlessEngine.cpp"));
    CHECK(!engine.empty());
    // Both output-gate sites go through the tested gate with the tint: the tripped-guard presents open it so the tint is drawn
    CHECK(Count(engine, Squash("UiPaste::SetPasteEnabled(_core.PasteGate(_tint));")) == 2);
    CHECK(!Has(engine, Squash("UiPaste::SetPasteEnabled(_core.PasteVisible());")));
    // ... and the tint value is synced once per present, before the gate is asked
    CHECK(Count(engine, Squash("SyncPasteOptions();")) >= 1);
    CHECK(Has(engine, Squash("UiPaste::SetDebugTint(tint);")));
    const size_t sync = engine.find(Squash("voidEngine::SyncPasteOptions()"));
    const size_t firstGate = engine.find(Squash("UiPaste::SetPasteEnabled(_core.PasteGate(_tint));"));
    CHECK(sync != std::string::npos && firstGate != std::string::npos);
    // The warning line quotes the config default: one number, checked by the static assertion in the engine
    CHECK(Has(engine, Squash("static_assert(ext::HudlessConfig::kGuardDefault == kGuardDefaultLimit,")));
}

void TestHudlessCard(const std::string& repo)
{
    const std::string menu = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/HudlessMenu.cpp"));
    CHECK(!menu.empty());
    // The status: the guard in force (limit, release level, why it falls back, worded for the policy) right after the flagged-UI
    // line, then the HUD's own flagged area the limit has to let through, so the user has a number to set the guard against
    CHECK(Count(menu, Squash("GuardStatusLine(status.guardLimit, status.coverageTripped, status.guardPolicy)")) == 1);
    const size_t flagged = menu.find(Squash("Flagged UI area:"));
    const size_t guardLine = menu.find(Squash("GuardStatusLine(status.guardLimit, status.coverageTripped, status.guardPolicy)"));
    const size_t hudArea = menu.find(Squash("HUD's own flagged area:"));
    CHECK(flagged != std::string::npos && guardLine != std::string::npos && flagged < guardLine);
    CHECK(hudArea != std::string::npos && guardLine < hudArea && hudArea < menu.find(Squash("UI paste:")));
    CHECK(Count(menu, Squash("if (status.hudCoverage >= 0.f)")) == 1); // the line is drawn, not only spelled out
    // ... and one short pointer line to the warning while it is on
    CHECK(Count(menu, Squash("The coverage guard limit cannot release over the HUD's own flagged area")) == 1);
    // The coverage guard is its own row under its own heading, after the mask keys: the two 0..1 sliders cannot be confused
    const size_t title = menu.find(Squash("SectionTitle(\"HUD-less (UI-free frame generation)\")"));
    const size_t threshold = menu.find(Squash("SliderFloat(\"Mask threshold\""));
    const size_t guardTitle = menu.find(Squash("SectionTitle(\"Coverage guard\")"));
    const size_t guard = menu.find(Squash("SliderFloat(\"Coverage guard limit\""));
    CHECK(title != std::string::npos && threshold != std::string::npos && guardTitle != std::string::npos &&
          guard != std::string::npos);
    CHECK(title < threshold && threshold < guardTitle && guardTitle < guard);
    CHECK(Has(menu, Squash("this one does not change the mask")));
    // The warning sits next to the guard slider, from the pure text, and names the value it compares; it is drawn in ui_free only,
    // the one mode the guard exists in
    CHECK(Count(menu, Squash("if (const std::string warning = ext::hudless::GuardAdvisoryMenuLine(advisory); !warning.empty())")) == 1);
    CHECK(Count(menu, Squash("ImGui::TextWrapped(\"%s\", warning.c_str());")) == 1);
    CHECK(Has(menu, Squash("EvaluateGuardAdvisory(cfg.CoverageLimit(), status.hudCoverage, status.hudCoverageSamples)")));
    const size_t warning = menu.find(Squash("GuardAdvisoryMenuLine(advisory)"));
    CHECK(guard < warning);
    // (the status block has its own ui_free test; the warning's gate is the second one, right before the warning text)
    CHECK(Count(menu, Squash("if (status.mode == Mode::UiFree)")) == 2);
    const size_t modeGate = menu.find(Squash("if (status.mode == Mode::UiFree){if(conststd::stringwarning"));
    CHECK(modeGate != std::string::npos && modeGate < warning);
    // One log line per settled slider change, for each of the three sliders
    CHECK(Count(menu, Squash("sliderLogs.maskThreshold.Observe(")) == 1);
    CHECK(Count(menu, Squash("sliderLogs.maskDilation.Observe(")) == 1);
    CHECK(Count(menu, Squash("sliderLogs.coverageGuard.Observe(")) == 1);
    // The flush runs on every frame of the card, before the Advanced fold's early return: a change that settled with the fold
    // closed is written instead of waiting for the fold to be opened again
    CHECK(Count(menu, Squash("for (const std::string& line : sliderLogs.Due(GetTickCount64()))")) == 1);
    const size_t due = menu.find(Squash("for (const std::string& line : sliderLogs.Due(GetTickCount64()))"));
    const size_t fold = menu.find(Squash("if (!ImGui::TreeNode(\"Advanced\"))"));
    CHECK(due != std::string::npos && Has(menu, Squash("LOG_INFO(\"{}\", line);")));
    CHECK(fold != std::string::npos && due < fold);
    // "Reset advanced" resets the guard too, logs the values it restored in one line, and drops the sliders' pending lines; its
    // tooltip says so
    CHECK(Has(menu, Squash("cfg.coverageGuard = ext::HudlessConfig::kGuardDefault;")));
    CHECK(Count(menu, Squash("sliderLogs.Reset(cfg.Threshold(), static_cast<int>(cfg.Dilation()), cfg.CoverageLimit())")) == 1);
    CHECK(due < fold && Has(menu, Squash("and the Coverage guard back to 0.60 (the guard is reset with them)")));

    // The probe interval: a slider in the fold after the guard slider and before Reset advanced, its settled value logged by its own
    // timer flushed before the fold's early return, untouched by Reset advanced (the only assignment is the slider's), and the status
    // line that names it is hidden while the blind A/B of the interval runs
    const size_t probe = menu.find(Squash("SliderInt(\"Probe one frame in N while falling back\", &probe, 1, ext::hudless::kProbeIntervalMax)"));
    const size_t reset = menu.find(Squash("if (ImGui::Button(\"Reset advanced\"))"));
    CHECK(probe != std::string::npos && reset != std::string::npos && guard < probe && probe < reset);
    CHECK(Count(menu, Squash("probeLog.Observe(static_cast<float>(cfg.ProbeInterval()), GetTickCount64());")) == 1);
    const size_t probeDue = menu.find(Squash("if (probeLog.Due(GetTickCount64()))"));
    CHECK(probeDue != std::string::npos && probeDue < fold);
    CHECK(Has(menu, Squash("LOG_INFO(\"HudLess: ProbeInterval set to {} from the menu\", static_cast<int>(probeLog.Take()));")));
    CHECK(Count(menu, Squash("cfg.probeInterval =")) == 1 && menu.find(Squash("cfg.probeInterval =")) < reset);
    CHECK(Count(menu, Squash("if (status.coverageTripped && status.probeInterval > 1 && ext::Cfg().hudless.ProbeAbSeconds() <= 0)")) == 1);
}

// The paste images' lifetime goes through the tested ring rule: the producer records its read of the previous image (the cleanup input),
// also when its fence signal failed, and a retired image is destroyed only when ring::Idle says nothing still reads it (a retire right
// after a pass used to destroy the image that pass still read: a GPU fault on a mode switch)
void TestPasteLifetimeWiring(const std::string& repo)
{
    const std::string paste = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/UiPaste.cpp"));
    CHECK(Count(paste, Squash("c.ring.NoteCleanupRead(previous, value);")) == 1);
    CHECK(Count(paste, Squash("c.ring.NoteCleanupRead(previous, ring::kNever);")) == 1);
    CHECK(Count(paste, Squash("if (!done.removed && !ring::Idle(item.meta, done.fences)) return false;")) == 1);
    const size_t endWrite = paste.find(Squash("c.ring.EndWrite(target, value, NowMs());"));
    const size_t note = paste.find(Squash("c.ring.NoteCleanupRead(previous, value);"));
    CHECK(endWrite != std::string::npos && note != std::string::npos && endWrite < note);
    const std::string ringModel = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/UiPasteRing.h"));
    CHECK(Has(ringModel, Squash("if (i == previous || i == newest || !Writable(slot, done)) continue;")));
    // a retire keeps the slots' bookkeeping (the cleanup read included) with the images it retires, under both producer locks
    CHECK(Count(paste, Squash("c.retired.push_back({ std::move(c.images[i]), c.ring.slots[i] });")) == 1);
    const size_t release = paste.find(Squash("void Release() {"));
    CHECK(release != std::string::npos && paste.find(Squash("std::lock_guard producer(c.prodMutex);"), release) <
                                              paste.find(Squash("RetireAllLocked(c);"), release));
    // the collector the engine polls after a switch away from ui_free, and the engine polling it until nothing is left
    CHECK(Has(paste, Squash("bool Collect() {")));
    // an image a paste still holds between pick and submit is not collected (only a race with a pending paste reaches it: the ring model
    // tests the rule, this pins the collector's use of it)
    CHECK(Count(paste, Squash("if (item.meta.readers != 0) return false;")) == 1);
    // a producer pass without a completion token never lets its slot be reused or pasted again
    CHECK(Count(paste, Squash("c.ring.PoisonWrite(target);")) == 1);
    const std::string engine = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/HudlessEngine.cpp"));
    CHECK(Has(engine, Squash("if (_pasteRetired) _pasteRetired = UiPaste::Collect();")));
    CHECK(Has(engine, Squash("if (_retired.empty() && !_pasteRetired)")));
    CHECK(Count(engine, Squash("_pasteRetired = true;")) == 1);
    // the slot keeps the bridge's final image its pass read until the slot's readers completed, and a pass without a completion token
    // keeps its slot for good
    CHECK(Has(engine, Squash("slot.finalRef = finalImage;")) && Has(engine, Squash("keep(slot.finalRef);")));
    CHECK(Has(engine, Squash("AddReader(static_cast<size_t>(_stagedSlot), _produceFence.Get(), UINT64_MAX);")));
    // the cadence line: the window a pause ended is taken before the paste is counted, the full window after it, and the lines are
    // written once the paste's locks are released
    const size_t before = paste.find(Squash("c.cadenceClock.BeforePaste(nowMs, c.status.produced)"));
    const size_t onPaste = paste.find(Squash("c.cadence.OnPaste(cadenceCurrent, cadencePasted, cadencePending, cadenceSteady);"));
    const size_t after = paste.find(Squash("c.cadenceClock.AfterPaste(nowMs, c.status.produced)"));
    CHECK(before != std::string::npos && onPaste != std::string::npos && after != std::string::npos && before < onPaste &&
          onPaste < after);
    const size_t locked = paste.find(Squash("PasteLocked(c, swapChain, presentQueue, cadenceLines);"));
    const size_t written = paste.find(Squash("for (const std::string& line : cadenceLines) LOG_INFO(\"{}\", line);"));
    CHECK(locked != std::string::npos && written != std::string::npos && locked < written);
}

// The engine answers the core's probe interval through the tested pure rules (the tint needs every present, the A/B schedule only
// when it is on), and the FFXIV source forwards a skip to the live source only while it delivers frames
void TestProbeWiring(const std::string& repo)
{
    const std::string engine = Squash(ReadFile(repo + "/OptiScaler/ext/hudless/HudlessEngine.cpp"));
    CHECK(Count(engine, Squash("return EffectiveProbeInterval(cfg.ProbeInterval(), tint);")) == 2);
    CHECK(Count(engine, Squash("return EffectiveProbeInterval(block.interval, tint);")) == 1);
    // the tint the interval rule is asked about is the setting, not a constant: with it on, every present probes
    CHECK(Count(engine, Squash("const bool tint = cfg.showDetectedUi.value_or_default();")) == 1);
    const size_t on = engine.find(Squash("if (!_probeAb.On())"));
    const size_t announce = engine.find(Squash("HudLess: probe A/B on:"));
    CHECK(on != std::string::npos && announce != std::string::npos && on < announce);
    CHECK(Has(engine, Squash("if (block.changed) LOG_INFO(\"{}\", ProbeAbLine(block, seconds));")));
    const std::string boundary = Squash(ReadFile(repo + "/OptiScaler/ext/ffxiv/ui/FfxivUiBoundary.cpp"));
    const size_t skip = boundary.find(Squash("void SkipNextCopy() override"));
    CHECK(skip != std::string::npos);
    CHECK(boundary.find(Squash("if (!Armed() || !ext::hudless::Wanted()) return;"), skip) != std::string::npos);
    CHECK(boundary.find(Squash("Live().SkipNextCopy();"), skip) != std::string::npos);
}

void TestMarkWiring(const std::string& repo)
{
    const std::string card = Squash(ReadFile(repo + "/OptiScaler/ext/present/MenuFramePacing.cpp"));
    CHECK(!card.empty());
    // The mark key: a Keybinds row (slot 19, the next free one), its config key, and the one log line the brief asks for
    CHECK(Count(card, Squash("ext::menu::RegisterShortcut({ \"FG recorder mark\", 19, MarkKeyOption,")) == 1);
    CHECK(Count(card, Squash("void* MarkKeyOption() { return &ext::Cfg().framePacing.fgRecorderMarkKey; }")) == 1);
    CHECK(Count(card, Squash("void MarkPressed(ext::menu::Ctx&)")) == 1);
    CHECK(Has(card, Squash("const unsigned number = ext::fgrec::Mark();")));
    CHECK(Has(card, Squash("LOG_INFO(\"user mark {}\", number);")));
    // The mark fires on key release (the shortcut poll reads released keys), and the card says so
    CHECK(Has(card, Squash("it fires when you release the key")));
    CHECK(!Has(card, Squash("mark (*)")));
    // The card's tooltip states the retention the recorder now keeps
    CHECK(Has(card, Squash("9 kept per session: 144 MiB plus the one")));
    CHECK(Has(card, Squash("queue batch that crossed the limit")));
    CHECK(Has(card, Squash("about 40 minutes at 60 base fps and 2x frame generation")));
}

void TestEarlyAnchorWiring(const std::string& repo)
{
    // A snapshot that cannot be made (no device, or the texture creation failed) must not be copied, and it is its own decline cause.
    // A working device always creates the same-size RGBA8 snapshot of the backbuffer, so no unit test can reach the branch; the decline
    // counters and their summary text are tested with the other causes.
    const std::string live = Squash(ReadFile(repo + "/OptiScaler/ext/ffxiv/ui/UiLive.h"));
    CHECK(!live.empty());
    CHECK(Count(live, Squash("if (!device || !capture::EnsureTexture(device.Get(), bound, false, _snapshot)) return "
                             "decline(EarlyDecline::Texture);")) == 1);

    // The game side (no unit test reaches it: it needs the game's swapchain and draw hook). Each of these, reverted, makes early do nothing
    // in a game while every behavioural test passes: the predictor and the backbuffer fetch handed to the live source, the rule read
    // once per Present, the draws forwarded to it, and the fetched buffer handed out with its reference.
    const std::string hook = Squash(ReadFile(repo + "/OptiScaler/ext/ffxiv/ui/FfxivUiBoundary.cpp"));
    CHECK(!hook.empty());
    CHECK(Count(hook, Squash("env.predictEffects = &ext::reshade::PresetsWouldApply;")) == 1);
    CHECK(Count(hook, Squash("env.backbuffer = &BackbufferTexture;")) == 1);
    CHECK(Count(hook, Squash("Live().SetAnchorRule(ext::Cfg().hudless.GetAnchorRule());")) == 1);
    CHECK(Count(hook, Squash("if (live && (res.r1bChanged || res.r1OffscreenChanged))")) == 1);
    CHECK(Count(hook, Squash("if (live::IsLiveEvent(ev))")) == 1);
    CHECK(Count(hook, Squash("if (keep) *keep = buffer.Detach();")) == 1);
    // the copy source comes from the same chain choice as the identity: all three candidates pass `keep` on
    CHECK(Count(hook, Squash("return BufferOf(st.currentRealSwapchain, game.Get(), texture, shape, keep) || "
                             "BufferOf(st.currentSwapchain, game.Get(), texture, shape, keep) || "
                             "BufferOf(st.currentRealSwapchain, nullptr, texture, shape, keep);")) == 1);
    CHECK(Count(hook, Squash("return BackbufferOf(&identity, &shape, texture) && *texture != nullptr;")) == 1);
}

void TestDocs(const std::string& repo)
{
    // The player- and tool-facing texts say exactly what the tint does while the guard is tripped (it draws the paste, so the
    // flagged region comes from a real frame), what the warning measures and where the slider lines come from.
    const std::string ini = ReadFile(repo + "/OptiScaler/ext/ini/HudLess.ini");
    CHECK(!ini.empty());
    CHECK(Has(ini, "the tint draws the newest"));
    CHECK(Has(ini, "The draw replaces, it does not blend"));
    CHECK(Has(ini, "the output comes from that real frame and not"));
    CHECK(Has(ini, "runs at the real frame rate"));
    CHECK(Has(ini, "the low fifth of the last 512 readbacks"));
    const std::string doc = ReadFile(repo + "/docs/HUDLESS.md");
    CHECK(!doc.empty());
    CHECK(Has(doc, "it is not a generated frame there"));
    CHECK(Has(doc, "the **low fifth** of a recent window"));
    CHECK(Has(doc, "The line is written on the next frame of the card"));
    CHECK(Has(doc, "logs one line with the values it restored"));
}

void TestRecorderBound(const std::string& repo)
{
    const std::string rec = Squash(ReadFile(repo + "/OptiScaler/ext/fgrec/FgRecorder.cpp"));
    CHECK(!rec.empty());
    CHECK(Count(rec, Squash("constexpr int kKeepRotations = 8;")) == 1);
    CHECK(Count(rec, Squash("constexpr uint64_t kRotateBytes = 16ull * 1024 * 1024;")) == 1);
    // The rotation moves every .n to .n + 1 and drops the oldest; a hard-coded .1/.2 pair would lose .3 .. .8
    CHECK(Has(rec, Squash("for (int n = kKeepRotations - 1; n >= 1; --n)")));
    CHECK(Has(rec, Squash("MoveFileW(base.c_str(), (base + L\".1\").c_str())")));
    // An own-pattern file carries any rotation suffix up to the cap, or a .3 .. .8 file would never be swept
    CHECK(Has(rec, Squash("number <= static_cast<unsigned>(kKeepRotations)")));
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    TestEngineGate(repo);
    TestHudlessCard(repo);
    TestProbeWiring(repo);
    TestPasteLifetimeWiring(repo);
    TestEarlyAnchorWiring(repo);
    TestDocs(repo);
    TestMarkWiring(repo);
    TestRecorderBound(repo);
    if (g_failed != 0)
    {
        std::printf("hudless_ux_wiring: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::puts("PASS: hudless/recorder UX wiring pins (paste gate, status, guard sub-heading, warning, slider log, mark key, "
              "rotation bound)");
    return 0;
}
