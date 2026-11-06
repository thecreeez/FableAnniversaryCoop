# Theresa render skin driven by Hero

This supersedes the whole-Sister graphic substitution and Sister weapon
correction from tasks28–30. The owner observed a local T-pose and Sister
animations on the observing host. The new implementation retains the local
player's stock graphic arguments and the remote Hero appearance/graphic block.
Sister is only an additional render mesh. No game or tests were run for this
implementation; animation, deformation and clothing fit remain unverified.

Follow-up fix after the owner saw only the bald Hero: the original short-name
request produced Queued/Deferred, with no additional skin component on either
live pawn. The code now loads WeSISTER_01_SF.upk explicitly and uses the verified
export70 outer path wesister_01.MESH_SISTER_01, including a class-checked native
StaticLoadObject preflight. This avoids assuming the level has already loaded
Sister's content or populated a legacy short-name package mapping. Acceptance of
the corrected loader is pending; the old Deferred event was not a bone-map
success and its hardcoded common77/fallback3 fields were misleading.

New native calls: UObject::LoadPackage RVA001E37B0 (cdecl outer/filename/flags,
as called by AWellingtonPawn::vf332) and StaticLoadObject RVA001E3000 (seven
cdecl arguments, as used by the native mesh command). Both ten-byte prologues
are checked with relocated SEH handlers0239D432/0239D3B4. Loading and attachment
run on the graphics command thread. AssetLoaded/AssetFailed reports isolate
package/object failures from component creation and map rejection; attachment
events now report actual skin/driver/map counts and failure stage.

Second observed failure: run20261106-015306-222-25500 reports AssetLoaded on
both peers and then component-missing. Explicit loading was insufficient. The
command still ran through a legacy actor-name stream/defer gate and its own
asset-name resolution. Two scoped call-site shims now remove those dependencies
for the already-loaded skin request: RVA01C4E897 (CALL01C4B060, five cdecl
arguments, AL boolean) returns false; RVA01C4E990 (CALL001E3000, seven cdecl
arguments) returns the preloaded mesh. Expected E8 bytes and exact original
relative targets are checked by CodePatch. All other native apparel requests
forward to the original functions with unchanged arguments. This does not
establish which of the two legacy checks caused the observed early return;
it makes neither check necessary for a known-loaded request.

The request asset scope is thread-local and restored with __finally. Nested
unrelated additional-mesh commands clear it. Component construction, materials,
attachment, bone-map validation and native ownership remain unchanged. The
corrected path still needs owner runtime acceptance.

Confirmed root cause from the third owner run: native additional-mesh creation
is AI-only. CALL01C4E93F invokes01C46130, whose class getter01C318D0 constructs
AIPawn. It returns null for AHeroPawn before any mesh allocation. Read-only
inspection of the live guest's actual class chain confirmed that exclusion.
Earlier documentation incorrectly assumed this command accepted HeroPawn.

The third scoped CALL shim replaces this cast with existing WellingtonPawn
cast01C46010 only while the target skin asset scope is active. All used pawn
fields lie in WellingtonPawn, and material callback slot328 resolves to the
same01C30400 on WellingtonPawn, AIPawn and HeroPawn. Unrelated calls retain the
native AI cast. Validate the original CALL to01C46130 and the new cast helper's
seven-byte prologue56 8B 74 24 08 85 F6. PawnAccepted/PawnRejected distinguishes
this gate from subsequent mesh/map outcomes. Client DLL was rebuilt and
installed after the owner closed the clients; runtime acceptance remains open.

TheresaHeroSkin submits MESH_SISTER_01 through the existing additional-mesh
command used by CTCRandomAppearanceMorph. The native command creates the mesh
component, assigns ParentAnimComponent to the main Hero component, initializes
materials, adds it to the pawn's additional-mesh array and attaches it. Hero
remains the source of animation, named weapon bones and gameplay identity.
No Sister animation set/tree is copied. The old TheresaWeaponAttachmentHook
is no longer installed, so Hero's weapon rig receives no Sister bind correction.

The render mesh keeps its own geometry, materials, skin weights and inverse
bind data. Its parent map resolves77 bones to Hero by FName, never raw Sister
indices. Three unmatched bones follow the nearest mapped parent; they have no
independent breast/stomach animation. This is runtime pose following, not an
offline vertex rebind or a replacement cooked91-bone mesh. Different reference
poses and proportions can still deform the rendered body. Whether this produces
an acceptable equivalent of putting Sister's skin on Hero must be checked in
game; compilation and a valid map are insufficient.

Native tracing and checks:
- RVA01C50170: additional mesh submission, three stack arguments, RET0C;
  prologue55 8B 6C 24 08 85 ED. Queue/ring allocation stays native.
- RVA01C4E830: additional mesh command, cdecl(command); seven-byte SEH prologue
  6A FF 68 with relocated handler RVA025690B8. Command+0 is the graphic handle,
  +4 is destination slot, +20 is a copied null-terminated mesh name.
- The graphic handle+8 owns the UE3 pawn. Pawn+384 is the main mesh; +4F8/+4FC
  holds additional components. Append at execution time; do not reserve sparse
  slots or overwrite a garment's index. Native streaming replays pass the hook.
- Component+29C is ParentAnimComponent; +2A0/+2A4 is ParentBoneMap. Native
  RVA00701190 rebuilds it by FName. Six-byte prologue83 EC 08 55 56 57 checked.
  The scoped hook repairs missing ancestors whenever this native map rebuilds.
- Mesh asset+94/+98 is the reference skeleton; stride50 and parent+40 verified
  from RVA006FA680 (GetParentBoneName), including disassembly. Require80/91
  bones and77 direct matches/3 fallbacks before publishing a corrected map.
- RVA0034F570 is native component SetHidden, validated prologue8B 81 28 01 00 00.
  Hide only the driver mesh after a valid skin component is attached. Set
  component+3B0 bits20/80 to retain off-screen pose evaluation and animation-node
  ticking, respectively, as used by the branches in RVA0070B5B0. Keeping pose
  evaluation alone would allow the hidden driver's animation clock to stop.
  The skin receives no independent tree.

Lifecycle: bounded registrations record the live Thing/graphic/master. Local
ReleaseHero and remote Unbind forget them. Native pawn/component ownership
handles destruction; the DLL owns no duplicate UObject or global asset arrays.
A native clothing rebuild can remove additional meshes; the next update checks
array membership without dereferencing the old skin and submits a replacement.
Streaming owns pending retries, preventing duplicate submissions every frame.
An unexpected mesh/map retains the original visible Hero and hides the failed
follower. Hot-unloading this experiment from a live pawn is not an acceptance
target; shutdown follows the existing session/pawn retirement lifecycle.

Diagnostics distinguish Ready, Queued, Deferred and Attached. Attached means
the native component/map was found; its event still says fit=unverified. Use
the same build on both clients. Garment hiding, dress overlap, eye placement,
local/remote animation, aiming, hit alignment, childhood and reload need manual
acceptance. No stock package or quest Sister instance is modified.

Serialization background used for the earlier offline skeleton comparison:
[UE Viewer skeletal mesh implementation](https://github.com/gildor2/UEViewer/blob/master/Unreal/UnrealMesh/UnMesh3.cpp).
Runtime offsets and calling conventions above come from the local retail
decompilation and disassembly, not from that generic serializer.

Build verification: all307 Client sources compiled with the existing Release
/W4 /WX /O2 /GL options; final helper changes recompiled and DLL relinked, exit0.
The solution's MSBuild FileTracker fails with E_ACCESSDENIED before compilation,
so local/build-theresa-skin.py used the installed MSVC/SDK and existing native
third-party libraries. Output: src/bin/Release/AlbionTogether.Client.dll (x86).
No tests or game launches. Diff check passed; changes remain uncommitted.
