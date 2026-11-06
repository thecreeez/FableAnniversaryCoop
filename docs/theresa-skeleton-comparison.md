# Theresa and Hero: actual package comparison, 2026-11-06

This compares the assets themselves, not just executable strings. No game was
started and no unit/gameplay tests were run. No game package was modified or
external tool installed/downloaded.

Read-only helper: `re/tools/compare_skeletons.py` (workspace root, outside src).
Run with the existing Python environment:

```powershell
re-venv/Scripts/python.exe re/tools/compare_skeletons.py
```

It decompresses the LZO chunks in memory, resolves package imports/exports and
extracts the actual SkeletalMesh reference skeletons. Every accepted bone array
has valid unique names, normalized quaternion, finite translation, backward
parent references and child counts consistent with the complete tree. Material
references are parsed from the native mesh prefix, with its end required to
match the extracted skeleton array offset. It does not export geometry/skin
weights or fit clothing, and does not implement a general UPK importer.

## Inputs and artifacts

- `test-install/WellingtonGame/CookedPC/OrganizedForCooking/WeSISTER_01_SF.upk`:
  MESH_SISTER_01, two package exports with80 bones each.
- `test-install/WellingtonGame/CookedPC/OrganizedForCooking/WeHero_SF.upk`:
  MESH_HERO and MESH_YOUNGHERO_02,91 bones each. Comparison uses MESH_HERO.
- `local/WeSISTER_01_SF-skeleton.json`, `local/WeHero_SF-skeleton.json` contain
  input SHA256, export/array offsets, bone names/parents/quaternions/translations
  and material names.
- `local/theresa-hero-skeleton-comparison.json` contains per-common-bone deltas.

## Skeleton results

| Property | Sister | Hero |
|---|---:|---:|
| Bones |80|91|
| Shared named bones |77|77|
| Parent-name mismatches among shared bones |0|0|
| Material slots in base mesh |4|18|

Sister-only: L_breast_01S0, R_breast_01S0, Stomach_01S0.
Hero-only:12 Finger3/Finger4 bones across both hands, plus
WEAPON_SCABBARD_01S0/WEAPON_SCABBARD_02S0. Bone indices differ; e.g. right weapon
focus is68 in Sister versus80 in Hero. Never reuse Hero numeric bone indices
on Sister without name resolution.

The common hierarchy is compatible by parent names, but bind poses differ:
local translation to L_ForeArm is28.189 versus35.460 asset units, and to L_Hand
23.851 versus21.491. Other joints also differ in length/orientation. Shared
names/hierarchy are favorable evidence for adaptation, not proof of unmodified
animation, skin weights or garment fit.

Both weapon focus bones exist and have the same named hand parents:

| Bone | Local translation delta | Local quaternion rotation delta |
|---|---:|---:|
| WEAPON_FOCUS_01S0 (right hand) |4.101 asset units|176.995 degrees|
| WEAPON_FOCUS_02S0 (left hand) |3.905 asset units|176.008 degrees|

These are bind-pose comparisons, not observed in-game weapon rotation or final
world-space transforms. Per-profile grip correction is a credible next step.
Missing Hero scabbard bones need a substitute named attachment or added socket.
Neither input package exports an explicit USkeletalMeshSocket object; this is
not proof that the engine cannot synthesize or resolve other sockets.

## Clothing implications

Sister base materials: torso, legs, Face, Hair.
Hero base materials: torso_front, hips, torso_back, L_thigh, L_calf, L_foot,
L_forearm, torso_arms, L_upperarm, L_hand, R_upperarm, R_forearm, R_hand,
R_thigh, R_calf, R_foot, Face, Mouth_Inst.

This proves a different base material/part layout. It does NOT prove a stock
wear command must fail. A garment may bring its own mesh or material mapping;
those actual wearable assets and their native definition behavior have not yet
been compared. The legacy mesh dependency index includes real wearable names
such as MESH_HERO_APPRENTICE_SHIRT_01 and MESH_HERO_APPRENTICE_TROUSERS_01,
separate from folded pickup assets.

Revised conclusion: it is premature to require custom remodeling for all
clothes. The shared77-bone tree makes a controlled attempt with one native
wearable a reasonable next experiment. First inspect that garment's bone map,
part-hiding and material targets; then try its native equip with profile-specific
projection rather than blanket enabling the whole Hero rebuild. The current
fixed-outfit prototype intentionally suppresses the final appearance builder,
so simply clicking equip in that build is not a useful compatibility verdict.

No visual-success claim: skeleton comparison cannot establish clipping, hidden
body sections, texture/UV fit or correct rendering on both clients. Those need
an explicitly authorized later runtime check.

Implementation follow-up: task29 now connects native wear projection on both
clients while filtering the body/appearance stages to equipped clothing only.
This removes the previous blanket suppression for that experiment, but no
runtime fit/rendering result has been obtained.

## Format references

The decoder uses classic LZO1X stream rules described in the
[Linux format analysis](https://docs.kernel.org/staging/lzo.html).
UE3 mesh fields were cross-checked against the primary UE Viewer serializers:
[bone records](https://github.com/gildor2/UEViewer/blob/master/Unreal/UnrealMesh/UnMesh.h)
and [mesh serialization](https://github.com/gildor2/UEViewer/blob/master/Unreal/UnrealMesh/UnMesh3.cpp).
All asset-specific numbers above come from the local package reads, not from
these general format references.
