// Source-text pins for the NR hotkey wiring: the three config keys are bound exactly like the existing
// toggle key (hex when positive, unbound by default), each has its ini fragment line, its Keybinds row
// (a RegisterShortcut with a free slot, the key option and the press handler) and its press handler writes
// the setting once. The handlers share the step and toggle rules with the menu (NrHotkeys.h), so the key
// and the control can never drift apart; the 500 ms settle in the pass owns the rebuild, so the menu adds
// no second debounce path.
// The pins compare whole statements with the whitespace removed, anchored to the function they live in, so a
// changed sign, direction, slot, option or handler fails them (a pin on a name alone would not).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string ReadRaw(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Read(const std::string& path) { return Squash(ReadRaw(path)); }

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The braced body of the function whose (whitespace-free) signature is `signature`, braces included; empty when the
// signature is missing or not unique. String literals are skipped so a "{}" format string does not unbalance it.
std::string Body(const std::string& text, const std::string& signature)
{
    if (Count(text, signature) != 1)
        return {};
    size_t at = text.find('{', text.find(signature));
    if (at == std::string::npos)
        return {};
    int depth = 0;
    for (size_t i = at; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '"')
        {
            for (++i; i < text.size() && text[i] != '"'; ++i)
                if (text[i] == '\\')
                    ++i;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0)
            return text.substr(at, i - at + 1);
    }
    return {};
}

// True when every needle is in `text` and they appear in the order given.
bool InOrder(const std::string& text, const std::vector<std::string>& needles)
{
    size_t from = 0;
    for (const std::string& needle : needles)
    {
        const size_t at = text.find(needle, from);
        if (at == std::string::npos)
            return false;
        from = at + needle.size();
    }
    return true;
}

// A line of the ini fragment, exactly (the squashed text would also match inside a longer key name).
size_t IniLines(const std::string& rawIni, const std::string& line)
{
    return Count(rawIni, "\n" + line + "\r\n") + Count(rawIni, "\n" + line + "\n");
}

// Every integer slot a call `RegisterShortcut({ "name", <slot>, ...` passes, under `dir`.
std::vector<int> ShortcutSlots(const std::string& dir)
{
    std::vector<int> slots;
    const std::regex call(R"(RegisterShortcut\(\{\s*"[^"]*"\s*,\s*(\d+)\s*,)");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h"))
            continue;
        const std::string text = ReadRaw(entry.path().string());
        for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it)
            slots.push_back(std::stoi((*it)[1].str()));
    }
    return slots;
}
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string cfg = Read(repo + "/OptiScaler/ext/nr/NrConfig.cpp");
    const std::string cfgHeader = Read(repo + "/OptiScaler/ext/nr/NrConfig.h");
    const std::string rawIni = ReadRaw(repo + "/OptiScaler/ext/ini/DlssNr.ini");
    const std::string menu = Read(repo + "/OptiScaler/ext/nr/menu/NrMenu.cpp");
    const std::string card = Read(repo + "/OptiScaler/ext/nr/menu/DlssNr_Menu.cpp");
    const std::string pass = Read(repo + "/OptiScaler/ext/nr/pass/DlssNr_Dx12.cpp");
    const std::string registry = Read(repo + "/OptiScaler/ext/core/MenuRegistry.h");
    CHECK(cfg.size() > 1000 && cfgHeader.size() > 1000 && rawIni.size() > 1000 && menu.size() > 1000 &&
          card.size() > 10000 && pass.size() > 10000 && registry.size() > 1000);

    // ---- the keys are bound exactly like the existing toggle key: hex when positive, unbound by default -------------
    CHECK(Count(cfg, "b.Add(\"DlssNr\",\"ToggleKey\",toggleKey,true);") == 1);
    CHECK(Count(cfg, "b.Add(\"DlssNr\",\"ScaleUpKey\",scaleUpKey,true);") == 1);
    CHECK(Count(cfg, "b.Add(\"DlssNr\",\"ScaleDownKey\",scaleDownKey,true);") == 1);
    CHECK(Count(cfg, "b.Add(\"DlssNr\",\"TransferToggleKey\",transferToggleKey,true);") == 1);
    // ... and directly after it, so the key block stays together
    CHECK(InOrder(cfg, { "b.Add(\"DlssNr\",\"ToggleKey\",toggleKey,true);", "b.Add(\"DlssNr\",\"ScaleUpKey\",scaleUpKey,true);",
                         "b.Add(\"DlssNr\",\"ScaleDownKey\",scaleDownKey,true);",
                         "b.Add(\"DlssNr\",\"TransferToggleKey\",transferToggleKey,true);" }));
    // unbound by default: today's behaviour is that nothing happens
    CHECK(Count(cfgHeader, "CustomOptional<int>toggleKey{UnboundKey};") == 1);
    CHECK(Count(cfgHeader, "CustomOptional<int>scaleUpKey{UnboundKey};") == 1);
    CHECK(Count(cfgHeader, "CustomOptional<int>scaleDownKey{UnboundKey};") == 1);
    CHECK(Count(cfgHeader, "CustomOptional<int>transferToggleKey{UnboundKey};") == 1);

    // ---- each key has its own ini fragment line, shipped as auto ----------------------------------------------------------
    CHECK(IniLines(rawIni, "ToggleKey=auto") == 1);
    CHECK(IniLines(rawIni, "ScaleUpKey=auto") == 1 && IniLines(rawIni, "ScaleDownKey=auto") == 1 &&
          IniLines(rawIni, "TransferToggleKey=auto") == 1);

    // ---- each key has its Keybinds row: name, free slot, key option, poll log, press handler ------------------------------
    CHECK(Count(menu, "ext::menu::RegisterShortcut({\"NeuralRendering\",14,ToggleKeyOption,"
                      "\"NeuralRenderingkeypressed,willbetogglingthepass\",OnTogglePressed});") == 1);
    CHECK(Count(menu, "ext::menu::RegisterShortcut({\"NRmodelresolutionup\",16,ScaleUpKeyOption,"
                      "\"NRmodelresolutionupkeypressed\",OnScaleUpPressed});") == 1);
    CHECK(Count(menu, "ext::menu::RegisterShortcut({\"NRmodelresolutiondown\",17,ScaleDownKeyOption,"
                      "\"NRmodelresolutiondownkeypressed\",OnScaleDownPressed});") == 1);
    CHECK(Count(menu, "ext::menu::RegisterShortcut({\"NRenlargementmode\",18,TransferToggleKeyOption,"
                      "\"NRenlargementtogglekeypressed\",OnTransferTogglePressed});") == 1);
    CHECK(Count(menu, "ext::menu::RegisterShortcut({") == 4); // the toggle plus the three new rows
    // each row is bound to its own key: the option getter names the member, exactly like the toggle row does
    CHECK(Count(menu, "void*ToggleKeyOption(){return&ext::Cfg().nr.toggleKey;}") == 1);
    CHECK(Count(menu, "void*ScaleUpKeyOption(){return&ext::Cfg().nr.scaleUpKey;}") == 1);
    CHECK(Count(menu, "void*ScaleDownKeyOption(){return&ext::Cfg().nr.scaleDownKey;}") == 1);
    CHECK(Count(menu, "void*TransferToggleKeyOption(){return&ext::Cfg().nr.transferToggleKey;}") == 1);

    // ---- the slots: every shortcut slot in the fork is distinct, apart from upstream's four rows (10 to 13), and the
    // registry has room for all of them ---------------------------------------------------------------------------------------
    {
        const std::vector<int> slots = ShortcutSlots(repo + "/OptiScaler/ext");
        const std::set<int> distinct(slots.begin(), slots.end());
        CHECK(slots.size() == 5 && distinct.size() == slots.size()); // the NR toggle and its three new keys, and the HDR screenshot
        CHECK(distinct.count(14) == 1 && distinct.count(15) == 1 && distinct.count(16) == 1 && distinct.count(17) == 1 &&
              distinct.count(18) == 1);
        const std::string upstream = ReadRaw(repo + "/OptiScaler/menu/menu_common.cpp");
        std::set<int> upstreamSlots;
        const std::regex row(R"(Keybind\(\s*"[^"]*"\s*,\s*(\d+)\s*\))");
        for (std::sregex_iterator it(upstream.begin(), upstream.end(), row), end; it != end; ++it)
            upstreamSlots.insert(std::stoi((*it)[1].str()));
        CHECK(upstreamSlots == std::set<int>({ 10, 11, 12, 13 }));
        for (int slot : slots)
            CHECK(upstreamSlots.count(slot) == 0);
        CHECK(Count(registry, "inlineconstexprsize_tkMaxShortcuts=8;") == 1 && slots.size() <= 8);
    }

    // ---- each press writes its setting once, through the shared rule ----------------------------------------------------
    {
        // the two scale keys are thin wrappers over ONE stepping function, one direction each
        CHECK(Body(menu, "voidOnScaleUpPressed(ext::menu::Ctx&)") == "{StepModelResolution(true);}");
        CHECK(Body(menu, "voidOnScaleDownPressed(ext::menu::Ctx&)") == "{StepModelResolution(false);}");
        const std::string step = Body(menu, "voidStepModelResolution(boolup)");
        CHECK(!step.empty());
        CHECK(Count(step, "nr.workingScale=DlssNr::Hotkeys::StepWorkingScale(nr.workingScale.value_or_default(),up);") == 1);
        CHECK(Count(step, "nr.workingScale=") == 1 && Count(menu, "nr.workingScale=") == 1); // one write, no second path
        CHECK(Count(menu, "nr.workingScaleX=") == 0 && Count(menu, "nr.workingScaleY=") == 0); // per-axis keys are never touched
        CHECK(InOrder(step, { "nr.workingScale=DlssNr::Hotkeys::StepWorkingScale(",
                              "constintpercent=(int)std::lround(nr.workingScale.value_or_default()*100.0f);",
                              "std::stringcontent=std::format(\"Modelresolution{}%\",percent);",
                              "if(PerAxisOverrides())content+=\"(per-axiskeysoverride)\";",
                              "ShowHotkeyToast(\"DLSSNeuralRendering\",content);" }));
        CHECK(Body(menu, "boolPerAxisOverrides()") ==
              "{constauto&nr=ext::Cfg().nr;constfloatx=nr.workingScaleX.value_or_default(),y=nr.workingScaleY.value_or_default();"
              "return(std::isfinite(x)&&x>0.0f)||(std::isfinite(y)&&y>0.0f);}");

        // the enlargement key reads the same numbers the menu greys the combo from, returns early while it is greyed, and
        // otherwise writes the toggled value once
        const std::string transfer = Body(menu, "voidOnTransferTogglePressed(ext::menu::Ctx&)");
        CHECK(!transfer.empty());
        CHECK(InOrder(transfer, { "constDlssNr::AnamorphicLatchlatch=DlssNr::AnamorphicLatchState();",
                                  "DlssNr::EffectiveScales(latch,nr.workingScaleX.value_or_default(),nr.workingScaleY.value_or_default(),"
                                  "nr.workingScale.value_or_default());",
                                  "constbooltoggleable=DlssNr::Hotkeys::TransferToggleable(scales.x,scales.y);",
                                  "if(!toggleable)return;",
                                  "nr.transfer=DlssNr::Hotkeys::ToggleTransfer(nr.transfer.value_or_default(),toggleable);",
                                  "ShowHotkeyToast(\"DLSSNeuralRendering\",nr.transfer.value_or_default()==1?"
                                  "\"Enlargement:Matchedresidual\":\"Enlargement:Classic\");" }));
        CHECK(Count(transfer, "nr.transfer=") == 1 && Count(menu, "nr.transfer=") == 1);
        CHECK(Count(menu, "if(!toggleable)return;") == 1);
        CHECK(!Has(menu, "nr.transfer=1") && !Has(menu, "nr.transfer=0")); // the value comes from the shared toggle rule only
    }
    // the menu owns no rebuild of its own: the pass reads the keys every frame and its settle rebuilds once
    CHECK(!Has(menu, "Settle::") && !Has(menu, "ParkWorkSizeScratch") && !Has(menu, "passNeedsReset"));
    CHECK(Count(pass, "constfloatscaleKeyUniform=nr.workingScale.value_or_default();") == 1);
    CHECK(Count(pass, "DlssNr::Settle::Decide(g_nr.settle,appliedId,") == 1);

    // ---- each press shows the same kind of toast the toggle key shows, with the new value ---------------------------------
    // The toggle builds its toast inline; the three new presses share one helper that builds the same toast
    // (kind, duration, title from the caller, content), so each press is pinned at its call with the shared title.
    CHECK(Count(menu, "toast.setTitle(\"DLSSNeuralRendering\");") == 1);
    CHECK(Body(menu, "voidShowHotkeyToast(constchar*title,conststd::string&content)") ==
          "{ImGuiToasttoast{ImGuiToastType::Info,2000};toast.setTitle(title);toast.setContent(content.c_str());"
          "ImGui::InsertNotification(toast);}");
    CHECK(Count(menu, "ShowHotkeyToast(\"DLSSNeuralRendering\",") == 2);

    // ---- the menu combo and the press handler share the one toggle rule -----------------------------------------------------
    CHECK(Count(card, "constboolreduced=DlssNr::Hotkeys::TransferToggleable(effectiveScaleX,effectiveScaleY);") == 1);
    CHECK(!Has(card, "effectiveScaleX<0.999f")); // the menu has no grey-out rule of its own any more
    CHECK(Count(card, "if(!reduced)ImGui::BeginDisabled();") >= 1);

    // The hotkey writes the same value the Model resolution slider commits: a whole percent over 100.
    {
        const std::string rules = Read(repo + "/OptiScaler/ext/nr/NrHotkeys.h");
        CHECK(Count(card, "nr.workingScale=std::clamp(pendingScale,25,200)/100.0f;") == 1);
        CHECK(Count(rules, "return(float)stepped/100.0f;") == 1);
    }

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: NR hotkey wiring (bound keys, ini lines, Keybinds rows and free slots, single-write press handlers, shared toggle rule)");
    return 0;
}
