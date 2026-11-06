# Experimental Theresa guest appearance

Branch: `experiment/theresa-guest-appearance`, based on `32c37d2`.

Current implementation: [Theresa render skin on Hero](theresa-hero-skin.md).
The owner found a local T-pose and Sister animations on the observer in the
earlier build. Task31 replaces the whole-Sister graphics below with a Hero
driver and a render-only Sister follower. Runtime acceptance remains open.

## Historical implementation (tasks28–30, superseded)

Final Release Win32 solution build passed. No unit tests, automated gameplay
scenarios or manual game runs were executed. Known-name index was regenerated;
full Ghidra export still fails the existing JDK/profile initialization problem.

Implementation scope: substitute the guest's visual presentation with retail
`CREATURE_SISTER` and attempt native equipped-clothing projection. No game or test execution was
authorized for this change. Runtime behavior and animation compatibility are
unverified; a successful build does not establish that the model renders.

## Selection and lifecycle

The experiment automatically selects Theresa for every `PeerRole::Guest` on this
branch. Use the same build on both peers. The local host retains its original
appearance. This prototype also uses Sister's adult visual during the guest's
childhood; the native Hero definition and story life stage remain unchanged.

On the guest, TheresaAppearanceHook intercepts the native player graphic setter
before construction. It looks up Sister's creature definition, reads its graphic
ID/type and passes those to the original player setter. It does not turn the
player into a Sister NPC, change the selected save definition, or replace the
story Sister. An unavailable/unsupported graphic leaves the original graphic
and reports MultiplayerTheresaAppearanceUnavailable.

On observing peers, the existing scoped remote-definition hook creates a
separate private Theresa variant. The normal rival creature remains the remote
actor's database/spawn identity. Sister supplies graphic/eye fields and
CAppearanceDef; Hero supplies the behavior, carrying and morph definitions.
Hero dimensions are retained. Shared retail definitions are never edited.
The private definition is cached separately from adult/child Hero variants and
has the same process-lifetime ownership policy as the existing remote clones.

The existing role field selects the visual profile; no wire schema/version
change or new saved state is introduced. Native reconstruction after travel or
reload re-enters these construction paths rather than retaining an old pawn.

## Native clothing experiment

The initial fixed-outfit experiment suppressed the final Hero appearance
builder. The current implementation instead runs both native body-part selection
and appearance building against a temporary view containing only the actual
equipped clothing definition IDs. This applies to the selected guest player
and remote private Theresa bodies; other actors use their original collections.

Remote clothing selection uses the existing native wear/rebuild adapter and
owner-authored six-slot state. Owner inventory is not granted or overwritten.
Hero morph/bone-scale and generic hair/horn/skin modifier replay remain skipped.
The native collection pointers are restored in __finally, including if native
processing raises. Incomplete clothing/modifier publication defers the attempt;
empty equipment selection still runs the native removal path.

This submits native clothing work; it does not establish compatible geometry,
materials or visible rendering. No clothing meshes/UVs/weights were edited and
voice or animation retargeting was added. Experimental weapon attachment
corrections are documented separately below. Stock
dress/body overlap, unexpected hiding and rendering failures remain unverified.

## Checked native paths

- CThingPlayerCreature::vf11 RVA01B5BD80 reads creature graphic ID+108/type+115
  and calls vf28 before CThingCreatureBase initialization.
- CThingPlayerCreature::vf28 RVA01B5BAF0 accepts thiscall(type, graphic), creates
  the player presentation for type4 and stores it at Thing+A4; RET8 verified.
  Expected SEH handler RVA024AC1F8 (preferred VA028AC1F8).
- Hero attachable composite builder RVA019B4290 reads owner at component+4
  and rebuilds Hero visuals; expected handler RVA025313D8 (VA029313D8).
  Callers include initial vf5, serialized vf1 and dirty refresh019B4A10.
- Body-part builder RVA019B2DB0 is now also scoped to selected clothing.
  Its13-byte prologue includes handler RVA025311D0 (VA029311D0).
- Remote creature definition is at Thing+100; native creature vf11 reads its
  graphic block from there. Local player vtable RVA02B1DBB4.

All three hooks validate the prologue and relocated SEH handler and supply
expected bytes to InlineHook. Shutdown retains hook state if patch restoration
fails. Diagnostics include MultiplayerTheresaAppearanceReady,
MultiplayerTheresaLocalGraphicSelected and the existing private-definition build
event with visual=CREATURE_SISTER. MultiplayerTheresaClothingSubmitted reports
the native stage, selected/matched definition counts and visual=unverified;
MultiplayerTheresaClothingDeferred reports unavailable selection/component data.

Later acceptance must verify the actual model on both clients, locomotion,
combat, transitions/reload and coexistence with the real quest Sister. Check one
ordinary outfit first, then change/unequip it and inspect both clients. Weapon
and garment fitting require runtime acceptance. The [weapon attachment
experiment](theresa-weapon-attachments.md) adds hand bind-frame correction and
missing scabbard fallback; the separate solid/aiming path remains native.
No runtime success is claimed.

Static follow-up: [equipment feasibility](theresa-equipment-feasibility.md).
The follow-up found that body-part selection preceded the suppressed builder;
both stages are now handled by the scoped clothing projection. This change is
an implementation attempt, not in-game acceptance of stock garments on Sister.
