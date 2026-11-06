# Runtime garment geometry adaptation for Theresa

Current backend: native bone depth with component-private painted garment weights;
see [native weight paint](theresa-native-weight-paint.md). The previous
morph-shader backend below is historical and no longer activated by Apply.

## Historical morph backend (disabled)

Implemented by Native/TheresaGarmentShape.cpp, called on the native graphics
command thread from TheresaHeroSkin::RefreshVisual for adult Theresa only.
It uses native per-component morph targets, which have their own delta GPU
buffers. This avoids cloning or changing the shared garment mesh entirely.
The earlier Spine2 depth1.22 workaround is removed; other body proportions and
progression damping are retained. Visual game acceptance is pending.

One algorithm uses the authored Spine1/2/3 weights and front-facing chest
region to calculate two compact smooth volumes, tapering toward sternum,
waist and sleeves. It computes position and tangent-Z normal deltas (inverse
transpose Jacobian), preserving topology, UVs, weights and authored hard edges.
V2 uses +model Y as the front, established by owner's in-game observation
(V1's negative-Y assumption incorrectly deformed the back). Default depth4.5,
height143, half-width7.5, radii12/16; rigid armour depth*0.75. The normal Jacobian
uses the same positive displacement. MaterialIndex=-1 and scalar FName=None
explicitly disable Fable's GearMorph material-scalar path006FAB20; this target
must never change material projection. UVs stay unchanged; final fit/texture
appearance requires manual inspection.
Original vertices are immutable. CPU deltas are cached per garment/profile;
each live component gets native-owned copies. Re-equipping cannot accumulate
deformation. Lady Grey/dress/Whisper garments keep their authored female shape.

Native path: UClass getters004BB570/00497500, StaticConstructObject001E1050
(cdecl10 arguments, including FName pair and GWarn), engine reallocator0016B890.
UMorphTarget has LOD models at+3C (stride16), FMorphTargetVertex delta stride20
(float3 position, packed tangent-Z delta, SourceIdx). UMorphTargetSet +3C targets,
+48 BaseSkelMesh. Append set to component+2C4; manual pointer/weight at+2DC,
active list+2D0. Native InitMorphTargets0070BC00 builds the lookup; UpdateActiveMorphs
00704530 merges manual targets. Slave-update bit3B4:10 and evaluation3B0:20 keep
native morph evaluation enabled. Native SetHidden toggles schedule deferred
scene-resource refresh. GPU update00743A90 lazily creates private morph resources
when active targets exist and sends updates through native render commands;
engine object/render destructors own retirement. No DLL callbacks or stale
component pointers are retained. Native signatures are checked before use.

On a new garment component, asset FName, bone count, every LOD count/vertex
count must match the cache. Unknown layouts stay unchanged. A native set marker
prevents duplicate targets; later refreshes repair a lost manual/active entry.
Host clothes, weapons, arm follower, Sister body and childhood are excluded.
Native hidden/detached clothing is preserved, not force-revealed. Only live,
actor-owned, registered visible clothing receives a morph. Old private render
helpers are hidden, so a refresh cannot resurrect a full Hero arm copy or an
obsolete clothing layer. Scene exit restores only clothes visible before it.
Scoped native visibility and section guards reject Hero-driver reveals even
when a transition has replaced its mesh before simulation rebinding. An arm
copy may be revealed only with all non-arm sections already hidden at allLODs.
Source skeletal mesh CPU/GPU vertex buffers are never modified.

Offline discovery: equipped Hero garments live in CookedPC/Startup.upk,
not the WeHERO_FOLDED_* item packages (those contain StaticMesh inventory props).
Startup includes SkeletalMesh exports153666/153667 apprentice shirts,
153673 assassin,153680 bandit,153695 Bowerstone,153701 chainmail,
153716 guard,153722 guildkeeper, and other shirt/armour variants.
Generate baselines with:
`re-venv/Scripts/python.exe re/tools/build_theresa_garment_cache.py`.
It reads Startup, validates skeleton/chunks/indices/weights, confirms every
CPU position and tangent-Z against the independent serialized GPU stream
(multiple UV channel strides supported), and writes only
src/bin/Release/theresa-garment-geometry.bin plus local audit manifest.
Source Startup SHA256 is checked unchanged. Current cache25 shirts,22 eligible,
2349876 bytes, schemaTHGEOM02 with triangle indices. Every available LOD is
processed (these shipped meshes have1). CPU/GPU UV channels are now independently
verified as well as positions and normals; no UV values are changed.

Settings: src/bin/Release/theresa-garment-shape.ini, read at launch.
The tracked template is config/theresa-garment-shape.ini. [Chest] is global;
an optional [MESH_HERO_PLATEARMOUR_SHIRT_01] section can override Depth,
HalfWidth, Height, RadiusX, RadiusZ, RigidFactor, MaxStretch. Depth0 disables deformation.
Restart after settings changes; no live GPU-buffer edits are used for tuning.

MaxStretch defaults1.20. The curve solves a per-garment amplitude bound using
all original triangle edges (length>=0.1 model units). Both stretch and compression
are bounded by reciprocal ratios; it cannot keep texture density exactly constant
when changing shape, but prevents extreme deformation. Ranger's unconstrained
factor1.3357 becomes<=1.20 (depth4.5 reduced to2.714); every eligible garment's
budget passes offline. Normals are recalculated for the reduced amplitude.

Native audit00741370 confirms a separate16-byte-per-vertex morph buffer without
UV fields. Declaration builder002A2950 calls base002A24D0 for original UV0..3,
adds only morph TEXCOORD6/7. This establishes no direct UV-buffer overwrite;
it does not establish correctness of every final material/shader combination.
Runtime logs include actual depth and measured/requested strain factors.

Offline verifier Tests/TheresaGarmentShape.Offline.cpp uses the production
deformation against all25 baselines:22 eligible,6520/45730 vertices displaced;
symmetry, bounded displacement and excluded back/limbs/waist pass. This does
not instantiate native objects, render, inject or launch the game.

Acceptance requires manual views of two different garments, front/side and
motion/weapon use, wear/unwear, a level transition, and unchanged host armour.
Structural bone/section/morph attachment checks cannot prove the rendered
shape or material shader compatibility. Events GarmentMorphReady, Attached,
Unavailable, LayoutRejected, Rejected provide diagnostics, not visual proof.

Head follow-up: hide/guard both APawn mesh384 and Wellington render mesh460.
Native factory01C4EDB0 initially assigns both; material projection uses460.
Sister primary face/hair source materials are restored on attachment and repaired
after native appearance projection. RenderBindings logs both mesh pointers.
These corrections still require manual transition/appearance confirmation.
# Runtime compatibility correction (task43)

Owner follow-up confirms working wardrobe and correct materials, with no chest
volume. A manual clothing change removes the post-transition Hero head. Task44
therefore re-publishes equipped modifiers and performs native dirty appearance
refresh once after world-ready/new render binding (1500ms settling, max3
readiness attempts, cutscenes deferred, childhood skipped). It verifies selected
slots remain unchanged and never calls item wear/unwear or inserts inventory.
The observed manual repair is the rationale; automatic head acceptance pending.

Manual task42 acceptance failed: mixed purple/shifted armour, persistent Hero
head and no visible chest. Geometry-only offline checks do not validate shading.
The handler now requires effective material morph usage (UMaterial +644 bit3,
or special engine material +640 bit18) before activating a generated morph.
Unsupported garments keep their original geometry/material. Breast shaping for
those garments remains open; forcing a flag cannot supply a missing cooked shader.

Each garment now has a distinct generated set and target name. Reusing one
component after a clothing change must not reuse the previous asset's deltas.
The handler verifies BaseSkelMesh and retires only its previous manual morphs,
preserving unrelated native morphs. Objects remain under native GC ownership.

Hero render suppression now includes all driver material sections at all LODs
and full Hero copies in the actor's registered component array, not just apparel
slots. Bounded registered-component diagnostics include asset names and visibility.
Manual head/texture acceptance remains required; no game was launched by the agent.
