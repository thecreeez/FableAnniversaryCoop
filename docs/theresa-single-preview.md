# Manual one-client Theresa preview

From the parent workspace, double-click `play-theresa.bat`.
Optional: `play-theresa.bat -FixtureSet wasp-mp -Save AutoSave`.

One game process, one native local Hero with Theresa appearance. No network
session, host/guest pair, automated wardrobe changes or automatic shutdown.
Normal movement, inventory, equipment and camera remain under manual control.
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
