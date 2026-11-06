# Theresa acceptance scenarios

From the workspace root, with manual game clients closed:

```powershell
powershell -ExecutionPolicy Bypass -File qa/run-theresa.ps1 -Scenario outfit
powershell -ExecutionPolicy Bypass -File qa/run-theresa.ps1 -Scenario checkpoint
powershell -ExecutionPolicy Bypass -File qa/run-theresa.ps1 -Scenario scene
powershell -ExecutionPolicy Bypass -File qa/run-theresa.ps1 -Scenario childhood
```

Run sequentially: childhood/basic locomotion use real foreground input. Profiles
and wrapper logs remain in qa/runs; client events in src/bin/Release/artifacts.

## Assertions

* **outfit**: capture all six actual equipped definitions on local guest and
  host's remote guest; remove the first equipped garment, require the exact
  changed selection on both peers, restore and require the original selection.
  Each phase also requires an actual Sister component, valid parent bone map,
  hidden Hero with animation evaluation enabled and visible Sister primitive.
  This does not establish garment fit, clipping or material correctness.
* **checkpoint**: place guest four metres from host; record each local position,
  invoke host AutoSave, wait for a newer paired commit, move both players away,
  request the native host reload selector. After reload and five seconds of
  settling, compare actual local position with that player's recorded position
  (0.75m horizontal / 1.5m vertical tolerance). Require a visible bound Sister
  and no orphan marked remote-player records. Historical unmarked bandits
  cannot be distinguished safely by this test.
* **scene**: real Wasp intro gather/cancel/rejoin and shared native skip; require
  actual primitive hidden flags during/after the scene. Immediately after it,
  deliberately overlap guest with host and require normal runtime separation
  of at least1.6m. This ends before task26's separate NPC attack acceptance.
* **childhood**: genuine New Game in an empty isolated profile; require native
  CREATURE_HERO_CHILD and naturally active Q_NewOakValeIntro. Wait until the HUD
  reports outside a cutscene; submit normal W input, require input marker and
  at least1m displacement, still-active story quest and native save. Require a
  nonempty AutoSave, derive a new paired fixture, run two-client locomotion
  requiring young Sister's98-bone map on both views with the91-bone Hero driver.
  This is a beginning-of-story smoke fragment, not completion of childhood or
  proof of whole-campaign progression. Existing fixtures remain untouched.

TheresaHeroSkin::InspectForTest only reads; it never attaches/repairs a mesh.
Disabling attachment, clothing replication, scene hiding, post-scene separation
or guest-coordinate projection must fail the relevant scenario. Runtime
feature-disabled mutation runs remain unverified.

## 2026-11-06 results

Baseline qa/runs/20261106-121829: 7/10 pass, fixtures unchanged;
PlayerReplication.Tests passes. Failures:

* mp_locomotion: obsolete Hero modifiers/scales assertions on Sister. Updated
  to require actual Sister attachment on both views, retaining host Hero morph.
* mp_map_stress: seed3465837891, ordinal5, BowerstonePosh to BowerstoneJail,
  native request accepted but transition did not converge within90seconds.
* mp_cutscene: gather/cancel/rejoin/skip succeeded, later enemy phases12/19
  timed out; launcher eventually hit662second suite limit.

Client309sources (/W4 /WX) and launcher compile/link. Both PowerShell wrappers
parse. New childhood and standard locomotion attempts were blocked before menu
by the retail graphics dialog: floating-point render target alpha blending
D3DFMT_A16B16G16R16F. The previous task32 DLL also failed before menu in control
load_fixture (qa/runs/20261106-124151), independently of new driver code.
New runtime and mutation assertions are not accepted yet. Only test-owned
processes were stopped. No commit was made.
