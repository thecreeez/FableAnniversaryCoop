# Theresa chest: native bone depth and garment weight paint

Owner accepted the current manual preview and requested this checkpoint commit.
All22 garment weight invariants passed offline; this is not a claim that every
outfit, childhood sequence or multiplayer scenario was newly tested in game.

The current backend uses ordinary skeletal rendering with a private copy of
the garment. It redistributes the existing Spine2/Spine3 vertex influences into
two smooth front chest regions. Spine2 supplies native depth; neutral Spine3
supplies the remaining region/back. It doesn't add breast bones to Hero or
activate GPU morph targets. Other bone influences remain unchanged.

The full original garment GPU stream is read from a generated THGEOM03 cache,
not from possibly unavailable live CPU buffers. The cache extractor verifies
each GPU influence against the authored CPU vertex and chunk palette. During
private native duplication, a scoped serializer injects the painted stream
before the engine uploads GPU resources. It leaves the shared original asset
alone. Native reader marks private chunks as supporting4 influences. Private
garment names/materials, animation bone maps and original UV/position/tangent
data are preserved; only influence indices/weights change.

`[Chest] BoneDepthScale=1.35` in theresa-garment-shape.ini sets the native localY
depth scale. Reference pose axes confirm this axis corresponds to model depth.
Depth0 disables the garment handler at launch. HalfWidth/Height/RadiusX/RadiusZ
define the paired weight-paint region. RigidFactor/MaxStretch are retained for
historical geometry checks and do not set this backend's weight amplitude.
Changes are read at launch. Native scale activates only while a weighted
garment is visible and attached; removing/changing it restores the baseline
profile. Gameplay strength, inventory and global BNCFG files are unchanged.

Spine3 is neutral rather than inversely scaled. The game's MassBoneScaling
evaluator emits the current bone's scale separately from the parent's effect
on translation. Reciprocal scaling would shrink the neutral bone. Source
positions remain unchanged; the normal native skinning path creates volume.

Offline checks pass for22garments: both chest regions, weights sum255,
at most4 influences, unrelated weights conserved, back excluded, exact UV/
position/tangent byte preservation and repeat application without accumulation.
These prove data invariants, not rendered anatomy. Actual shape, clothing
bending during combat and load/transition acceptance remain manual checks.

Restore DLL/PDB/INI AND matching cache together when rolling back to task45;
that older binary uses THGEOM02. The newer cache is THGEOM03.
