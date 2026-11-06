# Theresa appearance: next priorities

Owner-reported issues, 2026-11-06/07. Implementation added 2026-11-07;
offline checks/build recorded in task50. All rendered acceptance remains open.

Owner follow-up / task53: no clothes must restore Sister's standard outfit,
not expose naked Hero geometry. That behavior supersedes the earlier body
fallback below. Default Sister torso/legs are restored per empty slot; helper
body is restricted to exposed hands/arms/feet under actual clothes, plus the
front chest/shoulder skin needed by BANDITCAMP_SHIRT. BANDITCAMP_HEAD is hidden
visually and Sister hair retained; inventory/statistics stay unchanged.
Body visibility guards now enforce the complete intended material mask during
native reset/unhide, and publish each LOD's full mask in one update to avoid
flashes. Rotating preview matching compares all91 Hero bone names rather than
requiring source asset pointer identity; it also permits copied material arrays.
Hero stays hidden during asynchronous widget reload. Candidate logging was
added because the owner's prior run had no EquipmentPreviewApplied events;
the new UI path still needs manual verification, not a claim of visual success.
The F11 profile is now in the INI and appearance keys default off; see
[preview/configuration instructions](theresa-single-preview.md).

Added 2026-11-07: **Character disappears when opening a chest.** Owner reports
the character vanishes during chest opening, leaving only the sword visible.
Owner confirms visibility returns after the chest-opening animation. Not yet
reproduced by the agent; whether this affects both clients is unknown. Trace the native interaction
visibility changes and Theresa follower restoration. Check before/during/after
opening, repeat interactions, and camera/control return; restore the complete
character without revealing Hero or leaving only attached weapons visible.

1. **Unclothed body visibility.** Removing clothing leaves Theresa's body
   transparent/missing except the arms. Provide continuous torso/legs/feet
   coverage through undress/equip transitions. Verify without clothing, with
   partial outfits, gloves and boots, and after location changes. Distinguish
   hidden body sections from material transparency before choosing a fix.
2. **Equipment-screen preview.** The rotating equipment mannequin still shows
   Hero. Identify its separate native pawn/component lifetime and apply Theresa
   appearance, current outfit and body proportions to that preview. Opening,
   changing items, closing and reopening must not affect the world character.
3. **Optional: hood/hair fit.** Theresa's larger hairstyle clips through hoods.
   Inspect separate hair sections and hood-equipped state. Prefer per-hood hair
   hiding/alternate hair or a local hood-fit adjustment. Preserve uncovered
   hairstyles and restore them after removing the hood; check different hoods.

Implemented behavior:

- Local Theresa no longer follows the remote-sister cutscene hide flag. This
  keeps her visible during chest animations; remote cutscene hiding remains.
- The existing independent Hero arm follower now supplies every non-head body
  material. Torso/legs/hands/feet are hidden only under their actual equipped
  categories; Hero face/mouth remain hidden. This reuses native body geometry,
  not a newly authored Theresa nude model. The old Sister stock torso/legs and
  limb-only helper are hidden when the body follower is live, avoiding overlap.
  Missing followers retain the previous Sister outfit fallback.
- Positive `OBJECT_HERO_NO_*` and `HERO_SUIT_NAKED` definitions are resolved
  from the native catalog and excluded from coverage. They previously caused
  bare slots to be treated as clothing.
- The rotating skeletal UI actor's native SetMesh/stream completion is hooked
  only for the local Hero asset. Actor-owned render components mirror visible
  Sister/body/clothing layers, material/LOD masks and the current local pose/
  bone profile. No world component is moved. Reconciliation updates outfits;
  switching to item meshes restores native rendering. Destructor tracking and
  parent detachment on Hero retirement bound component lifetime.
- Hair material3 is hidden under equipped headwear and restored after removal;
  face material2 remains enabled. This conservative policy also applies to
  hats/helmets, not only hoods. Hood geometry is not reshaped.

Manual acceptance (agent has not launched the game):

- Open a chest, watch Theresa before/during/after the animation, repeat.
- Remove torso/legs/gloves/boots separately and all together; check front/back
  body coverage, equip again, transition locations. Check armor seams manually.
- Open equipment, rotate, change garments, close/reopen, switch to item preview.
  Confirm Theresa/current clothing and that the world character is unchanged.
- Equip/remove different hoods and hats; verify hair toggles and face stays.
- Check childhood and two-client local/remote cutscenes separately. New UI/body
  rendering and animation compatibility are not proven by offline policy tests.
