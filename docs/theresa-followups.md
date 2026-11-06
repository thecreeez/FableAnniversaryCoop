# Theresa follow-ups

Owner accepted model snapshot78533f2. These changes are uncommitted and need
manual acceptance. No game or tests launched by the agent.

Build: all307Client sources compiled /W4 /WX /O2 /GL with installed MSVC;
final changed sources recompiled and DLL linked exit0. Copied staged DLL/PDB
to src/bin/Release and verified identical DLL bytes. Diff check passed.

Existing six equipped clothing IDs already replicate and native wear/rebuild
applies them to the Hero driver. Additional mesh visibility now refreshes when
clothes or cutscene state changes, through allocator01C52620/dispatcher01C52B90
on the native graphics thread. No garment weights, UVs or body material edits;
Sister dress overlap and body seams are still possible. Local scene state hides
skin/apparel and restores them afterward, including a child with no inventory.
Remote-pawn hiding continues. Guest-only post-scene separation waits200ms,
targets1.8m, verifies later>=1.6m and stops after3s. Uses native teleport with
guest height/facing; no collision-safe placement claim near walls.

Story childhood selects weyoung_sister_01.MESH_YOUNG_SISTER_01 from
WeYOUNG_SISTER_01_SF.upk. Actual package inspection found98 bones:77 shared and
21 fallback ancestors against both MESH_HERO/MESH_YOUNGHERO_02 (91bones).
Separate adult/child private definition caches retain appropriate Hero drivers.
The child has five base materials; visual fit/transition remain unverified.

Guest checkpoint Prepare previously reused host Ticket.position when projecting
its native Hero save. It now captures local guest position/facing on the same
named map. Generation/slot/map authority stays host-owned; local manifest and
ReloadGo already use that local saved ticket. Wire166bytes unchanged.

Native collection01B51F80/01B52380 tests persistence before creating whole entity
records. Predicates01B2B610/01B2B640 (verified prologues56 8B F1 8B 06 and
51 56 8B F1 8B 06) reject the exact remote-player script marker only to callers
in01B51F80..01B52540. Other callers keep native results and runtime flags remain
set for distance/unload protection. Base CThing text filtering alone was too late
to exclude derived records. Existing marked NPCs lacking live HeroMorph are
pruned after native presence events drain, with UID/native-pointer validation.
No arbitrary bandit purge: older unmarked NPCs cannot be safely identified.
Incorrectly saved historical guest coordinates cannot be recovered blindly.

Acceptance: inspect clothes on both peers, child/adult transition, scenes with
real Sister and separation, save/reload while standing apart and absence of an
extra NPC. Diagnostics: TheresaClothingOverlayQueued, HeroSkinAssetLoaded
(childhood flag), GuestSeparatedAfterCutscene, GuestCheckpointPlacementCaptured,
RemotePlayerSaveEligibilityReady/SaveExcluded and RestoredPlayerNpcRemoved
(all prefixed MultiplayerTheresa or Multiplayer as defined in source).
