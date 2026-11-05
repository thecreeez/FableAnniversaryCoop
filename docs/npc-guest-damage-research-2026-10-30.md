# NPC damage to the guest — research, 2026-10-30

Scope: investigate the owner's remaining report; no gameplay code changed or new game launched. Current source baseline: e253283. Evidence: manual session20261030-012827-245-7228.

## Matched NPC hit results

Match host and guest `MultiplayerCombatHitApplied` by source UID, target actor, actor generation, map epoch and target revision:
- Host:35 distinct NPC hit results; guest:35 matching results; no missing or unexpected result keys.
- 26 target the guest actor1596911083478076323 (hex16295E2B6C984BA3). All26 reach the guest through `target-owner-result`.
- First guest applications:22:32:04.738Z reduces actual health20->18;22:32:06.672Z gives8;22:32:07.398Z gives0. Later native resurrection/phial events are present.
- 16 of26 applications end at actual health0; additional hits cannot further lower already-zero HP. This does not mean16 hits failed to deliver, nor prove every other visible swing landed on the simulator.

Thus these35 recorded results do not demonstrate network loss. Hits missing before candidate construction remain outside this count; current logs cap raw native OnHit observations and do not identify every candidate-builder rejection.

## Confirmed stale local vitals cache defect

`EntityVitalsReplication::AuthorPlayer` updates `latestPlayers_[localActorId_]` on the host only. A guest caches its own host-echoed health through `AcceptAuthoritative`. `PublishPending` then drops a queued player message whenever its lifecycle differs from the cached latest message, without distinguishing a newer local incarnation from an older one. `RetirePlayer` explicitly skips the local actor, so a map transition does not remove this old self-echo.

Reproduction from source:
1. Guest generation1 health is published, host accepts and echoes it; guest caches generation1.
2. Local owner channel reopens as generation2/mapEpoch2 after travel.
3. Guest authors new health; the previous self-echo still describes generation1.
4. `latestMismatch` is true; `PublishPending` removes the new outgoing message and returns success. `AuthorPlayer` nevertheless logs `MultiplayerEntityVitalsPublished`.
5. The same check discards all later local incarnations. Host code refreshes its cache before publishing, so it does not suffer this specific guest-only failure.

Observed: guest logs32 player vitals publications across generations1..8. Host accepts exactly one, at22:29:06.439Z (health20/max20); subsequent publications are not accepted or rejected on the host. At22:32 the host proxy still uses default RivalHero maximum45 and retained45, while guest health applications use actual maximum20 and reach0. This is consistent with outgoing messages discarded before transport, not a host rejection.

Consequences: the simulator can retain a living, incorrectly scaled proxy while the actual guest is dead/recovering. It computes hit deltas against that proxy and can keep attacking it. This is a proven synchronization defect and a plausible contributor to the reported symptom, not proof of the cause of every missing NPC hit.

Next fix: validate locally authored messages against the current owner channel, not an older echoed self-cache; retain generation/session rejection for stale remote baselines. Add a real producer regression covering self-echo generation1 -> local map transition generation2 -> outgoing baseline/damage/recovery, plus rejection of genuinely old queued messages. Compare actual transport output, not the Published diagnostic alone.

### Follow-up implementation

`PublishPending` now fences local player messages against the current owner lifecycle only; an older authoritative self echo no longer vetoes the new incarnation. Missing owner state and genuinely mismatched queued owner generations/map epochs still discard the publication. Remote baselines retain their authoritative-cache fence.

Regression tests exercise the real producer and UDP transport with a retained generation1 self echo, generation2/3 baseline publication, death and recovery mutations. A separate backpressure test queues generation1 health behind a full actor stream, changes the owner lifecycle, frees transport capacity and verifies that the stale publication does not occupy the fresh queue. NPC side-strike presentation remains open; this change does not establish complete in-game damage acceptance.

Validation: Release Win32 solution build, full PlayerReplication.Tests and diff whitespace check passed. Restoring the old queue condition makes all six new-generation transport receipt checks fail (focused regression exit1); final sources were restored and rebuilt afterward. No game clients launched or commit made. The owner still needs to verify NPC damage after town travel and checkpoint reload.

## Incomplete guard side-strike presentation

Host guardFFFFFF0000000CF1 performs `CCreatureAction_BodgeSideStrike` (clip1199) as well as MidAttack/NearAttack. `ReadAttackTarget` only handles the latter two; the side strike is serialized without its target and replayed as a bare animation by `ReplayAuthoritativeNativeAction`.

Static: BodgeSideStrike::vf12 RVA0179FC90 selects STRIKE_MEDIUM_SIDE and chooses side from target geometry. Disassembly VA01B9FCC1..CC9 resolves target weak pointer at action+B4; clone017A1160 uses the shared combat-base copy016EA2F0. The target layout is therefore statically established. Native replay suppression means this guest-side animation cannot independently produce authoritative damage. Missing target/direction replay can make its visible contact differ from the actual host collision. Only one side-strike Begin for this guard occurs in the selected session, so this cannot explain all missing-hit reports by itself.

## Native checks and limits

- Player ModifyCombatHealth override01B5A520 rounds the delta then calls shared01B59CB0; it is not a separate unobserved NPC damage implementation.
- Local hit application is revision-protected and does not re-enter OnHit to generate another collision. Cosmetic reaction failure is consumed separately.
- Replica-health rejection and replay-hit suppression are intentional on observer bodies; they do not by themselves establish lost authoritative damage.
- No damage scaling, resurrection immunity, target ownership or NPC hit acceptance policy was changed during research. No build/tests were needed for documentation-only findings.

## Owner isolation: host aggression enables retaliation

Owner follow-up: the host had not offended the guard; after the host struck it, NPC damage started reaching the guest. This is an important new reproduction axis, not an in-game acceptance of the vitals fix as a complete damage fix.

Session `20261030-215327-230-4360` supports the timing: guest strikes against UID `FFFFFF000001D808` start18:55:07.106Z and produce39 results before the first host strike at18:56:15.603Z. The first host-recorded NPC result then occurs18:56:23.003Z, against the host. Later guest-owned NPC hits are present too. Compare full peer logs before treating this as proof of a particular native relationship flag; results from different peers may arrive at different times.

Static gap: `EnemyTargetDistributor::ApplyPlans` calls `AiTargetingComponent::BindCombatTarget`, which overrides selected enemy getter31 only. It deliberately leaves `CTCAIEnemy::CurrentEnemy` intact so SeekEnemy can request its normal transition. This is target selection, not replication of crime/hostility state. `CombatHitApplicator::Apply` mirrors accepted remote player hits by owner health delta and victim reaction; it intentionally does not replay retail OnHit. Therefore the simulator can receive a guest attack without running the same native aggression/crime side effects that a local host attack runs. Whether an omitted side effect is the precise gate still needs tracing.

Relevant native path: SeekEnemy's transition wrapper RVA016DF890 delegates to016DF650; this path includes creature-component and target eligibility checks before transition. Do not globally turn every guard hostile or bypass native damage policy. Next research/fix should identify the specific actor-scoped hostility/attacked event omitted when a guest attacks an NPC, preserve innocent players and quest-controlled NPCs, and test guest-only aggression with a passive host, then host aggression as a control. No new gameplay edits were made for this owner observation.

### Native relationship trace and diagnostic follow-up

- Confirmed native enemy predicate RVA01988480 (cdecl, two CThing pointers, plain RET). It resolves both Enemy components, respects FriendsWithEverything and delegates the directional decision to01987AC0. This is used by AI targeting and both player/AI OnHit overrides, not merely UI colouring.
- Directional predicate01987AC0 checks explicit ally list(+1C) before explicit enemy list(+10). Its Hero-type branch checks village state at+108 (`IsEnemyBecauseOfCrime`, identified by CTCVillage::vf1 RVA01B10337). Verified VA01D87C47 reads+108 and01D87C74..CB2 can return false without it. This is conditional on additional creature-definition/weapon predicates; do not claim every guard always follows this branch.
- SeekEnemy01759A70 calls eligibility016DFAC0 before transitioning. This also checks village+108 when its candidate has native type2. Thus selected-target override alone is not a complete enemy relation.
- Native explicit pair insertion01988130 and removal019882D0 are called by script slots616/617; village helper01B0E2E0 uses them for the active local Hero. These are a possible narrowly scoped repair mechanism, not yet invoked by this patch. Native paired removal must not erase a relationship that predated the mod.
- OnHit01B47090 builds `CEventHitBy` via01B45BC0/01989710 and queues it with01989060; its payload owns a copied hit-parameter object. CThingAICreature::vf71 also performs crime/attack side effects before delegating to the hooked base OnHit. Replaying a whole OnHit would duplicate health, crime and reaction work; manufacturing only a raw event without its lifetime protocol is unsafe.

Added observation only: `MultiplayerEnemyTargetRelation` on the NPC simulator logs the retail pair predicate against the selected actor and the simulator's local Hero, on assignment/relation changes. Values1/0 mean enemy/not-enemy; -1 means unavailable or signature/read failure. The native cdecl ABI/prologue/RET were verified with fre. No new hook or relationship mutation, no protocol change. At the next manual run, keep host passive, strike guard as guest, then strike as host: correlate this event with EnemyTargetPlanned/NativeEnemyTargetRead and CombatHitApplied. This distinguishes neutral-target filtering from a separate hit/action gate before choosing a repair.

Validation: Release Win32 solution build, full PlayerReplication.Tests and diff check passed. The new native observation requires the manual reproduction above; unit tests do not execute this retail predicate. Regenerated known-names.tsv (EnemyRelation at01988480); attempted tools/ghidra-export.ps1, but Ghidra LaunchSupport cannot write the profile java_home.save and fails JDK selection. Existing decomp and fre disassembly were sufficient for this trace; full renamed export remains pending. No game launch, commit or changes to hostility policy in this follow-up.

### Manual diagnostic result: 20261030-220802-474-26144

The new observation reproduces the native relation defect on the host:

- 19:08:48.331Z: guest hit guardFFFFFF000001D808; host applies health40->38 and victim reaction.
- 19:08:48.492Z: host assigns that guard to guest at1.1m; native predicate reports target_enemy=0/local_enemy=0.
- 19:08:48.992Z: native seek getter consumed that exact guest target. Thus the target plan is not merely missing or unread.
- 19:08:55.548Z native host hit (result55.555Z) reduces same guard38->36.
- 19:08:55.991Z: with the selected target still the guest, relation switches to target_enemy=1/local_enemy=1. Subsequent target switches preserve enemy relation.
- Recorded NPC results then reach the guest, including guard damage129->121->113->105 and further8HP steps down to49. Snapshot contains22 result keys on each peer with zero missing/extra,16 NPC results (11 targeting guest,5 targeting host).

This confirms that guest hit plus native target selection leaves the host's retail enemy predicate neutral, whereas a host hit changes it for both actors. It does not isolate which of the village/explicit-relation subconditions changed: the diagnostic records the final predicate only. Next repair must carry actor-scoped aggression into the simulator's relationship path, with a passive-host regression, without globally copying host guilt or replaying damage twice. No source changes/build/game intervention during this log inspection.

## Actor-specific aggression repair

Implemented in EnemyTargetDistributor and AiTargetingComponent after the diagnostic reproduction above:

- Only applied, host-approved damaging Player->NPC Results admit aggression evidence. It is fenced by the NPC generation/map epoch, player authority/generation/map/map ID and current transport connection nonce. Echoes cannot refresh evidence; candidates, non-damaging outcomes and other participant kinds do not admit it. Evidence may create a relation for10 seconds after the hit; an already created relation lasts with the active native target assignment.
- The NPC simulator invokes retail CTCEnemy add01988130 for the actual selected attacker. Existing hostile/native explicit entries remain native-owned. Neutral innocent players are excluded from target selection unless they have their own fresh attack evidence. Neither village crime state nor host guilt is changed.
- The adapter verifies both add/remove prologues and rebased SEH handler pointers before mutation. Enemy list traversal follows the verified sentinel/intelligent-pointer layout used by01987690, and only records ownership for a newly added pair. A native veto after insertion rolls back that pair. Whole OnHit/health/attack events are not replayed.
- Native assignments now retain NPC local incarnation and player lifecycle/session. Target/body/map/authority/session changes or invalid/dormant/dead/script-controlled/unowned NPC assignments release only mod-added entries through019882D0. Removal resolves the current native weak-list target by UID rather than dereferencing a retained player body. Existing native relationships are preserved.
- Diagnostics MultiplayerAttackEnemyAdded/Released/ReleaseFailed complement the existing relation observation. With a passive innocent host and a guest-only strike, expect target_enemy=1/local_enemy=0 and NPC damage delivered to guest. If both independently attack, both may be legitimate enemies. Target change/retirement must remove only the scoped mod addition.

Tests include the actual production native adapter against a synthetic x86 image with the retail prologues/cdecl/thiscall entry and instrumented native bodies: pair insertion, neutral host, no duplicate insertion/damage, native ownership preservation, friendliness rollback, script-control refusal, expired weak target and signature mismatch. Pure evidence tests cover innocent host, NPC/player map or incarnation changes, replacement connection, expiry, duplicate results and non-authoritative/non-damaging traffic. These tests do not substitute for the two-client retail reproduction. Protocol remains50.

Manual acceptance still required: clean fixture -> host passive -> guest strikes the neutral guard -> guard damages guest without any host strike; host remains neutral. Repeat after travel/checkpoint reload, guest reconnect, and NPC simulation handoff. Repeat both actors attacking, and a naturally hostile creature. No game clients were launched or stopped for this implementation.

Validation: Release Win32 build and full PlayerReplication.Tests passed after the session-fence tests; final NPC-availability guard also rebuilt successfully. Diff check passed. Known names regenerated (604 labels at535 addresses). Full Ghidra export was attempted again and still fails JDK/profile initialization; existing decomp and fre verified the add/remove ABI, paired lifetime and list layout. No commit/push. Retail acceptance above is still open.

### Failed first repair: SEH handler VA/RVA confusion

Owner reports no improvement in run `20261030-222652-546-19392`. Log inspection shows zero AttackEnemyAdded events, guest target still neutral after its first hit, then enemy relation changes only after host hit. Snapshot39 result keys match on each peer; delivery of recorded results is not the failing stage.

Found an implementation error: native mutator signature validation treated the disassembler's SEH handler VAs0292DA21/0292DA50 as RVAs. The correct RVAs are0252DA21/0252DA50. Direct PE reads from the test-install EXE independently confirm ImageBase00400000 and prologues `6AFF6821DA920264A100000000` / `6AFF6850DA920264A100000000`. The incorrect validation refused the add call before mutation, so the intended repair never ran in that session.

Corrected all three production checks and repaired the synthetic fixture to relocate the independent preferred-VA operands by subtracting the PE preferred base. Previously the fixture repeated the same incorrect assumption. The corrected focused tests pass; restoring only the old production handler values makes eight assertions fail (exit1). Fixed sources and Release binaries were then restored/rebuilt. Added AttackEnemyBlocked diagnostic for a refused insertion, and `PlayerReplication.Tests.exe --combat-aggression` for focused verification. Retail acceptance remains open; no claim of a successful guard repair in game yet.

Final validation after restoration: full PlayerReplication.Tests, Release Win32 solution build and diff check passed. Known names regenerated606 labels/537 addresses.

### Retail confirmation found during continuation, 2026-11-05

Previously unrecorded run `20261030-223242-150-2464` contains the corrected adapter's successful passive-host case. Times below are UTC:

- 19:33:27.853: guest hit reduces guard D808 to38HP.
- 19:33:28.211: MultiplayerAttackEnemyAdded reports relation_ready=true; the predicate reports target_enemy=1/local_enemy=0 (guest hostile, simulator-local host neutral).
- 19:33:34.957 through46.620: guest applies eight D808 results through target-owner-result, with health129,128,127,126,118,110,102,101 out of130.
- 19:33:47.079: first logged host Player->NPC hit, after all eight guest damage results. At47.216 the mod relation is released and the selected host has native enemy relation1.

Compared all applied-result keys using source/target kinds and IDs, generation, map epoch, revision, health and maximum:19 on each peer, zero differences. No AttackEnemyBlocked/ReleaseFailed or ClientFailed host events were found in this snapshot. The relation release event alone does not prove native removal independently of its diagnostics.

This provides retail evidence for the original guest-only aggression defect being repaired in this session. It does not verify travel, checkpoint reload, reconnect, simulation handoff, held block or all possible missed contacts. Existing broader task26 acceptance remains open. Review only: no game launch, source mutation, build or commit.
