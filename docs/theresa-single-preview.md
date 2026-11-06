# Manual one-client Theresa preview

From the parent workspace, double-click `play-theresa.bat`.
Optional: `play-theresa.bat -FixtureSet wasp-mp -Save AutoSave`.

One game process, one native local Hero with Theresa appearance. No network
session, host/guest pair, automated wardrobe changes or automatic shutdown.
Normal movement, inventory, equipment and camera remain under manual control.
Appearance keys are **disabled by default** as requested on 2026-11-07.
Edit `src/bin/Release/theresa-garment-shape.ini` before launch; the source
template is `src/config/theresa-garment-shape.ini`. The owner's F11 bookmark
from run `20261107-004506-711-6504` at 00:47:34 local is now the default:

```ini
[Chest]
BoneDepthScale=1.50
[Body]
Hips=0.75
Buttocks=0.65
ShoulderNarrowing=0.50
[Controls]
EnableShapeHotkeys=0
```

INI changes are read at launch; selected settings persist across runs. An
initial `MultiplayerTheresaBodyProfile` event records the loaded values and
whether hotkeys are enabled. Disabled appearance controls neither poll nor
reserve F1/F2/F3/F4/F6/F7/F9/F10/F11. F5/F8 retain their unrelated functions.

The following optional controls work only with `EnableShapeHotkeys=1`:
F1 increases chest depth and F2 decreases it. Each press
changes the native scale by 0.05; holding repeats after 350 ms, then every
100 ms. The range is 1.0 (neutral) to 2.0. Both keys together cancel the repeat.
Only the focused local game responds. Changes last for this process, including
location and outfit changes; restarting restores the INI value. Nothing is
written to saves or synchronized to other clients. Clothing needs the existing
painted chest weights for a visible effect; childhood is excluded.
F3/F4 increase/decrease hip width; F6/F7 increase/decrease buttock volume.
These two independent amounts range from 0 (no additional change to the
existing Theresa profile) to 1.5, starting at the INI values. Step/hold/focus/session rules
match chest controls. F5 retains its script-reload function. These are native
pelvis/upper-leg proportions, not separate buttock bones: depth can also affect
the front of the pelvis. Thigh length and calf/foot scale are unchanged; fit
and animation on actual outfits still require manual inspection.
F9 narrows shoulders; F10 reverses that change. The narrowing amount is
0..1.5 (0 = the existing 0.86 clavicle profile); the same .05/350/100 ms
repeat applies. Clavicle scale is 0.86 - 0.12 * amount, bounded 0.68..0.86;
only the clavicle's longitudinal X axis changes. Upper-arm scale stays 1.0,
with no reciprocal compensation. Actual shoulder/arm outfit fit is manual.

All adjustments emit `MultiplayerTheresaBodySizeChanged` events. On first
activation and after every change, `MultiplayerTheresaBodyProfile` records all
four settings together, the profile version, PID and derived scale factors.
Profile version 2 fixes the old inverse arm scaling that lengthened the arms;
version 1 bookmarks have different arm/clavicle semantics.
Press **F11** once to mark a preferred profile (`reason=bookmark_F11`), then
tell the agent that this marked state is good. A held F11 does not spam records.
The bookmark is a log entry, not persistent configuration; a restart restores
the starting settings. Chest depth in the record is requested depth, since
actual chest activation also requires a visible weighted garment. Logs are in
the current run under `src/bin/Release/artifacts/`.
The selected fixture is copied into a fresh artifacts/run/theresa/Documents;
source fixtures and personal saves are untouched. Changes made in this preview
stay in that run's isolated save. Each BAT launch starts from the fixture again.
Existing game processes must be closed first; the helper never kills them.

Launcher uses the explicit theresa_preview scenario to install local appearance
hooks without starting multiplayer. It retains Hero gameplay/animations, reuses
Sister/limb followers and all91-bone softened progression, handles clothing
rebuilds, and chooses young Sister for an actual child Hero. Selected-save load
is automatic; the game remains open for visual inspection.

`qa/play-theresa.ps1 -Check` validates required files without starting the game.
`--automation theresa_preview --dry-run` validates launcher paths/mode isolation.
Structural acceptance is not a substitute for manual clothing-fit evaluation.

2026-11-07 visibility follow-ups are installed for manual verification: local
chest-animation visibility, exposed body coverage, rotating equipment preview
and hair suppression under headwear. See [checks and implementation limits](theresa-next-steps.md).

The clothing-screen Hero uses a separate native clone. Task54 now binds its
actual graphics handles; rotating actors only cover item previews. Details and
manual acceptance: [native inventory preview](theresa-native-inventory-preview.md).
# Current controls (task58)

The final owner F11 profile (2026-11-07 02:02:21 local) is now the default;
appearance hotkeys are disabled again. Profile values and optional controls:
[Live controls](theresa-waist-controls.md). Earlier control states are historical.
