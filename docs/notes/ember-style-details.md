# Menu style (Ember): palette, shape and rules in full

Material moved out of [FEATURES.md](../FEATURES.md#menu-style-ember) on 2026-10-23; the paragraphs are copied unchanged; internal names are renamed or removed.

**What it does.** A second look for the in-game menu, selected with `[Menu] Style = ember` (the default, `classic`, is upstream's). The classic theme mixes the accent colour into almost everything: a button is 32% accent over near black, its hover 55%, the title bar and the headers the same, so a bright accent such as orange turns into brown and rust. Ember keeps every surface and control neutral and uses the accent only to mark state: the check mark, the slider grab, the overline of the selected tab, the focus ring, the text selection, the resize grips and the plots.

- **Surfaces:** three steps of one charcoal (canvas `#141519`, card `#1A1B20`, field `#212329`) with a lift for hover (`#2A2D35`) and for press (`#33363F`), and hairline borders of white at 9% alpha, so a translucent window shows the game through its edges. Text is `#F4F4F5`, dim text `#A0A0A6`.

- **Shape:** 10 px window corners, 8 px for cards and popups, 6 px for controls, tabs and grabs; a 6 to 8 px scrollbar; wider padding and spacing than ImGui's defaults (window 14 x 12, frame 10 x 5, item 10 x 8).

- **Buttons, headers, title bar:** neutral. Ember has no accent-filled button; ImGui cannot give one button a different border from the others, so the lift on hover is what shows the state.

- **Inputs:** the accent is `[Menu] AccentColorR/G/B`, the window's opacity is `[Menu] BGColorA`; `BGColorR/G/B` is not used, because the classic code mixes at most 12% of it into a fixed grey. A dark accent is lifted towards white so the check mark stays visible. Under HDR output the colours are tone-mapped like the classic ones (the seam runs before the tone mapping), and the window is still forced opaque by `AdjustTheme`.

- **Dark only:** with `LightTheme = true` the menu stays classic.

**Status.** Opt-in, off by default. Played in game on one RTX 4090. The palette and its rules are unit-tested (every slot set, the neutrals identical for every accent and never tinted, the accent only on state slots, contrast of text 12:1, dim text 4.5:1 and the orange accent 3:1 on the card, alpha, metrics), and so are the ImGui adapter (every `ImGuiCol_` the library declares is assigned) and the single upstream line.
