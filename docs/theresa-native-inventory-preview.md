# Clothing Hero preview: native clone, not rotating-item actor

Owner's task53 run confirms no rotating SetMesh callbacks during clothing UI.
WellingtonGame.u retains ScriptText: UI_PageCloth activates RTTGroup's Hero
camera, while SetupRTTActorMesh and rotating actors belong to item previews.
Audit script reads this source into workspace-local output only.

The native clothing component maintains a data clone at+1B8 and owns separate
render handles in its+2CC/+2D0 vector. Its builder019FE990, tick019F67C8,
clothing rebuild019FE6F3, close019FC45B and release019F4DC9 establish lifetime.
The adapter runs after native clothing tick and binds the first ready91bone
Hero graphic. It passes that graphic explicitly to resolution/shape routines,
instead of assuming that the data clone's Thing+A4 is the displayed graphic.

The existing Sister/arm/limb attachment machinery follows the menu's own driver,
retains menu animations/rotation, and uses canonical INI proportions. Coverage
comes from actual visible UI apparel so preview selections can differ from the
world outfit. Empty slots retain Sister's standard outfit. No world component
is moved or re-parented. Close/release clears entries before native destruction.
Extra layers receive native menu lighting via01C49C90/01C47990 (02000001).

Expected events: MultiplayerTheresaInventoryReady on install, CloneBound when
the data/handle list is published, GraphicBound once the model is ready, and
Applied after visibility/UI lighting reconcile. A Ready event alone does not
prove that a rendered preview was changed.

Manual acceptance: open equipment, rotate, preview/wear/remove different pieces,
close and reopen, transition then reopen. Confirm Sister's face, default outfit,
Bandit skin coverage and current proportions; check that the world appearance
stays accepted. Offline checks do not establish rendered acceptance.
# Follow-up: menu beard and selected outfit (task55)

Task57 overrides task56 publication changes: owner reported invisible menu
character despite InventoryApplied logs. Restore native component and eye
attachment; remove AddSkin synchronous visibility reconciliation. Prepare only
the completed ordinary apparel slot, request next-tick bone update, and retain
the existing periodic visibility path. Open female torso policy remains.
Model visibility awaits manual verification; eye/beard flash remains open.

Task56 adds pre-attachment garment weight preparation at AActor attachment,
next-tick bone refresh and immediate reconciliation after apparel commands.
The separate Hero eye socket attachment call019FF0EB->019F4F10 is skipped
only for Theresa owners. Native cleanup of the offscreen eye handles remains.
Open dress/Lady Grey/Whisper torso layers retain underlying front/back/shoulder
skin. Offline/Release validation passes; visual acceptance is pending.

The owner now sees the blindfold, confirming the actual clone path applies.
Run20261107-012347-372-11972 shows a separate visible Hero beard asset in the
menu actor. Hide Hero beard/hair layers, including later native unhide calls.

Menu outfit coverage now reads enabled modifiers of its data clone, matched
against the owner's clothing catalog, rather than equipped world state or
only residual visible meshes. Static019B2310 confirms the low enabled byte;
019F7921 selects the preview. Empty slots suppress stale garment meshes and
restore Sister stock outfit. New diagnostic: MultiplayerTheresaInventoryOutfit.
Build/offline checks pass; visual result awaits manual playtest.
