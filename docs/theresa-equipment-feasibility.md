# Theresa weapons and clothing: static feasibility, 2026-11-06

Follow-up: [actual skeleton/material comparison](theresa-skeleton-comparison.md)
now finds77 common bones with identical parent-name hierarchy,80 Sister bones
versus91 Hero, and4 versus18 base materials. Native clothing compatibility is
still untested; mandatory garment remodeling has NOT been established.

Task29 subsequently implements a native clothing attempt with scoped equipped
item filtering in both body-part and final appearance builders. The fixed-outfit
limitations below describe the earlier task28 prototype, not that later path.

Scope: read-only implementation research. No native code changes, build, game
launch or test execution. Findings below are static, not retail acceptance.

## Weapons

Carrying is independent of clothing. The existing native attachment entry is
RVA01956650, with slot definitions resolved through01719230. The current mod
already retains local Hero inventory/weapons and remote equipment projection.
Replacing the skin therefore does not inherently require a new weapon inventory
or network format.

The render bridge has a separate PutThingInSocket handler at01C4B6C0. It resolves
the carried mesh and dispatches through01C48250 with a socket name plus position
and rotation offsets. This establishes a native path for rigid attachments with
per-model grip/stow adjustments. It does not establish that Sister has the Hero
socket names or compatible attack animations.

Next implementation: inspect actual Sister reference skeleton and sockets,
map hand/back/hip attachment slots, and apply profile-specific offsets through
the native attachment path. Validate sword first, then stow/draw, bow/two-hand
grips and magic. Skeleton data and actual visual results remain unknown.

## Clothing is more than attaching an object

The actor state already replicates six equipped clothing category definitions.
That solves inventory/state selection, not garment geometry fitting.

The native Hero appearance update includes distinct stages:

-019B2DB0 clears/reapplies body-part selection through01C47370/01C497C0.
-01C497C0 queues01C474A0, which resolves a part name in pawn+5B8/+5BC and calls
 0070A450 on the skeletal component to alter its per-material visibility state.
 This is selection of existing mesh parts, not an importer of arbitrary clothes.
-019B4290/019B4000 apply appearance/material modifiers through queued bridges
 01C473A0,01C47410 and01C4AE20.
-01C4AE20 ->01C49820 ->01C48380/01C2EF20 manages named texture/material layers;
 01C2EF20 uses DiffuseAlpha/DiffuseTexture parameters. Hero material names/UVs
 cannot be assumed to match Sister.

Both meshes and their texture/part conventions matter. Even compatible bone
names alone do not prove that an unmodified Hero outfit fits Sister. Resources
WeSISTER_01_SF.upk and FXTHERESA_SF.upk exist in the test install, but no package
export, reference-skeleton comparison, mesh inspection or outfit fit was done.
Files named HERO_FOLDED_* are inventory/world assets and are not evidence of a
wearable garment's skeleton compatibility.

## Implementation options

1. Keep Sister's full stock model and adapt selected wearable meshes to it.
   Requires asset/skeleton/skin-weight inspection; removal or hiding of the
   stock dress must be supported by the actual mesh. Exact asset work is unknown.
2. Preserve Sister's recognizable head/hair and use a Hero-compatible body for
   clothing. This is an alternative visual design, not full-body Sister. It may
   reuse more of the existing outfit system but requires neck/seam, proportions,
   material and animation work; no drop-in compatibility claim.
3. Provide a small set of complete adapted outfits with explicit asset mapping.
   This avoids promising compatibility with every retail clothing combination.

## Correction to the first prototype's fixed-outfit description

The current Theresa hook bypasses019B4290 only. Native019B4A10 and the initial
vf5/serialized vf1 also call019B2DB0 BEFORE that builder. Thus body-part visibility
selection is still active for local and remote bodies. Skipping the final
builder alone does not prove the entire stock outfit is protected. Future
changes must handle body-part selection and material rebuilding separately;
do not simply re-enable all native Hero appearance work for Theresa.

Conclusion: native weapon attachment support is a concrete implementation path.
Clothing changes are feasible as adaptation work, but universal unchanged Hero
outfits on the full Sister model are unproven. The next useful research output
is an actual bone/socket/mesh-section/material comparison, not a blanket claim
that shared humanoid animations guarantee outfit compatibility.
