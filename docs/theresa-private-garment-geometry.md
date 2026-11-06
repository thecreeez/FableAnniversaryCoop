# Theresa private garment geometry

Current backend is [native depth with painted weights](theresa-native-weight-paint.md).
The earlier live-source/baked-position attempt below failed manual acceptance
because the tested source streams were unavailable. THGEOM03 now supplies
complete source records from disk before private GPU resource creation.

## Historical baked-position implementation

The chest handler deep-copies an adult garment through the engine, shapes only
its private vertex geometry, and draws it with ordinary skeletal shaders. It
does not enable GPU morph targets or require bUsedWithMorphTargets materials.
Childhood and clothes already authored for female bodies are excluded.

The original garment name/materials, all UV channels, topology, bone indices,
weights and animation driver are retained. Native parent-bone remapping runs
when binding the copy. Existing per-component material overrides remain native.
The shape expands the front (+Y) with two smooth lobes and a measured geometric
stretch limit. Tangent normals are updated; tangent handedness stays intact.

The same [configuration](../config/theresa-garment-shape.ini) controls the shape.
Default depth6 and radii14/18 produce a broader chest. MaxStretch1.25 can reduce
effective depth on individual garments. Per-garment sections remain supported;
Depth0 disables shaping at launch. Identical UV coordinates do not prevent
physical texture stretching when the surface changes shape; inspect fit manually.

The native duplication archive needs complete GPU streams on PC. A scoped hook
of00724910 enables that serializer's raw-data branch only during these copies.
Packed source positions are serialized through a private unpacked shadow; all
UV/bone/weight bytes are copied verbatim. The original source is never edited.
The native duplicate writer/reader serialize the remaining asset data and own
their arrays. A zero-target native MorphSet holder references the private mesh
for engine GC without activating a morph or storing untracked UObject pointers.

Before editing, verify distinct LOD/vertex allocations and unchanged material
pointers. Native PreEdit releases/waits only the copy's rendering resources.
After changing the retained CPU stream, native InitResources uploads ordinary
skeletal buffers and SetSkeletalMesh binds/rebuilds the parent map. Re-equipping
reuses the matching copy or creates a new component-owned copy, without adding
deformation repeatedly. Unsupported layout/stream/duplication keeps stock shape
and emits MultiplayerTheresaGarmentPrivateMeshRejected. Success emits
MultiplayerTheresaGarmentPrivateMeshAttached, including original/private assets,
fitted depth and strain; this is not proof of the rendered result.

Offline checks pass22garments*16GPUformats, covering packed position conversion,
all1..4half/full UV channels, exact UV/bone/weight/handedness preservation and
the bounded curve. Native engine copy/resource lifecycle and final appearance
still require manual play. No game/autoplay suite was run for this change.

If cooked runtime cloning fails, the alternative is a generated private UPK
with pre-shaped garment meshes using the same ordinary materials. It needs
additional package/reference work; forcing a morph usage flag cannot supply a
missing cooked shader permutation.
