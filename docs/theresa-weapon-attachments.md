# Theresa weapon attachment experiment

Task30 adds an implementation attempt for existing equipped weapons. Release
Win32 builds; no game or tests were run. Visual fit is unverified.

TheresaWeaponAttachmentHook is integrated with the existing remote ranged
orientation hook. It scopes native named-bone queries to actual CTCWeapon
objects carried by local or remote Theresa presentations. It grants no items
and changes no package, global skeleton, inventory or network schema.

The primary CTCCarrying slot query uses the named-point transform at RVA0199F840
(thiscall, seven stack arguments, RET1C). Its validated seven-byte prologue is
6A FF 68 AC F9 92 02, with relocated SEH handler RVA0252F9AC. The primary query
return address is RVA01955114. Only final-transform callers RVA019537B9 and
RVA01953831 enable adaptation. Direct weapon segment/hit endpoint callers
01B179F0 and01B17D80 stay native. Nested excluded queries clear the scoped
context and restore it on unwind. Indirect effects through shared native state
still require runtime verification.

For WEAPON_FOCUS_01/02, a successful native query receives a bind-frame
correction derived from the offline Sister/Hero skeleton comparison. Rotation
uses inverse(SisterBindQ) * HeroBindQ. Translation uses the inverse Sister bind
rotation and the Hero-minus-Sister bind position, scaled by0.01. Native basis
coordinates are interpreted as X=cross(Forward,Up), Y=Forward, Z=Up. This basis
interpretation and asset-unit conversion are calibration assumptions, not
measured proof of correct runtime placement.

Existing successful WEAPON_SCABBARD_01/02 queries are preserved. If missing,
the hook tries Bip01 Spine3 (then Bip01_Spine3S0) and composes the Hero scabbard
bind transform relative to that parent. Temporary native strings are destroyed
on unwind. Both ordinary and S0-suffixed weapon names are accepted. Failed or
unrelated queries retain native behavior. No direct attachment-cache writes
are added. Adapted transforms skip the generic remote bow forward flip.

The separate solid/aiming path at RVA0199F4E0/019547C0 remains native; it is not
forced through the ordinary bone-query path. Aiming, two-handed alignment,
hand/back placement, animation fit and correspondence with hits remain open.

Bounded diagnostics: MultiplayerTheresaWeaponHookReady/Rejected and
MultiplayerTheresaWeaponAttachmentAdapted/Native. Adaptation reports identify
focus/scabbard corrections and explicitly mark fit=unverified.
