# Distal limb overlay and adult visual proportions

The owner confirmed that hiding Sister's complete torso/legs material also
hides her hands and lower legs. This experiment keeps that section hiding but
adds a second Sister follower containing only selected original triangles.

## Generated local asset

Run from the parent workspace:

```powershell
re-venv/Scripts/python.exe re/tools/build_sister_limb_overlay.py
re-venv/Scripts/python.exe re/tools/verify_sister_limb_overlay.py
```

The generated `test-install/WellingtonGame/CookedPC/OrganizedForCooking/WeSISTER_C1_SF.upk`
uses namespace `wesister_c1`. No original package is overwritten. No retail
package or texture is committed to the source repository. Generator/inspector
and audit manifest remain in the workspace's re/tools and local directories.

Both adult skeletal mesh exports and both LODs retain original skeleton,
vertices, weights, UVs, section layout and material references. Unwanted
triangles become degenerate in the existing index buffer, preserving offsets.
Package compression remains LZO, using verified literal-only streams and rebuilt
chunk tables. Input/output decompression roundtrip and source hash are checked.
Primary selected mesh retains3656/11670 triangles in LOD0 and945/2916 in LOD1.

Selection uses disconnected components plus bounded reference-pose regions,
not a claim that every skin pixel was separated from cloth. Kept geometry:
hands/wrists, original boots/lower legs. Removed: sleeves, torso, skirt, face,
hair. Original boot cuffs remain. There is no reconstructed bare upper arm,
thigh, knee or complete body beneath the outfit; open garments can still show
gaps. Preview: local/theresa-limb-overlay-lod0.png and lod1.png.

Format cross-check: [UEViewer skeletal mesh serializer](https://github.com/gildor2/UEViewer/blob/master/Unreal/UnrealMesh/UnMesh3.cpp).
Actual Fable bounds/records/indices/weights are independently validated against
the local package. No external tool installation is required.

The additional follower uses the same scoped native attachment and parent bone
mapping as the main Sister follower. Show only its torso/leg sections whose
original counterpart is hidden, avoiding coincident geometry when unequipped.
Hide it together with the rest during scenes. Recreate after clothing rebuild.
If the overlay is unavailable, retain the stock outfit rather than lose limbs.

## Visual proportions

Bare-arm follow-up: the previous overlay restores hands, not upper arms or
forearms. Under torso clothing the Hero master now draws only its native
material slots6/8/10/11 (L_forearm/L_upperarm/R_upperarm/R_forearm). Hide its
other14 sections, including head/torso/hands, at allLODs. Without torso clothing
keep master hidden so Sister's sleeves remain. Scenes hide everything; native
clothing rebuild invalidates the mask. This uses Hero-derived skin geometry;
skin tone, seams and fit require manual checks. No full bare body is generated.

Native MassScaling evaluator RVA006DED70 uses scale vectors for pose Scale3D
and parent-scaled translations, not only joint offsets. Reuse the already
signature-checked native named setter RVA006DEA30. Adult Theresa only:

* pelvis1.035; compensate spine and both thighs by1/1.035;
* waist (Spine1)0.97; compensate above it (Spine2) by1/0.97;
* left/right clavicle0.965.

Uniform local scales avoid guessing anatomical axes. All91 bones retain20% of
native scale deviation, clamped to0.98..1.04 before the proportion preset.
Preserve exact native baselines, cache outputs to prevent accumulation, and
reapply when native appearance rebuilding replaces them. Hero stats, experience,
strength/fatness inputs, host appearance and childhood are not changed.
Both local guest and host's guest presentation apply the same visual profile.
Existing Hero weapon bones remain the animation driver; grip/aim/cloth fit
still require visual checks. This is not a new chest morph or a converted
female armour collection.

Acceptance now requires actual overlay binding, complementary hidden-section
flags, exact authored limb materials, and all91 live native scale vectors.
It injects upper-arm visual scale2 and requires runtime damping to1.04 before
restoring original visuals and changing clothes. Owner run135527 passes on both
peers. For appearance inspection use play-theresa.bat (one manual local Hero).
Offline validation and Release compilation do not prove final rendered fit.

2026-11-06 manual rejection of the driver-section approach: arms still absent,
Hero face after level transitions. RefreshVisual hid the master on every call
but only re-enabled its arm sections on an outfit-cache miss. The following
refresh therefore hid the arms again. Run142028-398-18880 confirms successful
Sister attachment after each native driver replacement, so attachment events
alone do not establish correct visible output.

Replacement uses a separate native apparel component holding the already
loaded Hero asset (91 bones, identity ParentBoneMap,18 authored materials).
Only materials6/8/10/11 render on this follower. The Hero master stays hidden
even while Sister loads. Every LOD uses native material remapping before
choosing visible arm sections, preventing a reduced LOD from enabling a face.
Arm visibility is reconciled independently of the outfit cache every500ms;
native section updates skip matching actual flags. Childhood uses no arm
follower. The read-only acceptance probe checks the hidden master, separate
arm follower and its identity bone map. No new RVA or cooked package needed.
Manual appearance and transition acceptance remain pending.

Owner subsequently accepted the arm follower's appearance (task38).
Task39 preserves local wear/unwear native enabled flags after filtering by
owned clothing catalog membership instead of a selected-slot snapshot. Keep
native rebuilds running when the snapshot is transiently unavailable.

Interim chest volume: Spine2 local Y depth1.22, compensated at Spine3 so the
neck/head/shoulders do not inherit the new depth. Offline quaternion composition
of the exact Hero reference skeleton verifies X=height, Y=depth, Z=width.
This has no independent breast shape and does not convert a male armour mesh.
The proposed generic geometry adapter is documented in
theresa-runtime-garment-shape.md; native private mesh/resource cloning is still
unverified and no live vertex-buffer writes have been installed.

Task41 supersedes the older shape values: in-game feedback establishes +Y as
front. Component-local chest morph V2 reverses the previous wrong-side mask,
uses depth4.5/radii12,16, disables the optional GearMorph material-scalar channel,
and preserves native clothing visibility. Scoped SetHidden/ShowMaterialSection
guards keep the Hero driver and unmasked/retired arm copies from reappearing.
Clavicles now0.86 with reciprocal UpperArm-root scale: shoulder origins move
inward while hand/arm size is compensated. Progression damping acceptance now
injects leftForeArm bone43 (identity profile), still expects2->1.04.
No visual acceptance of these corrections is claimed before owner's inspection.
