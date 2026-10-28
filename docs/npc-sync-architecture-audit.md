# NPC synchronization architecture audit (2026-10-28)

Historical audit: the findings below describe the starting implementation.
The current checkpoint addresses lifecycle admission, stable simulation,
host-selected native targets, movement, XP/orbs and death recovery.
Integrated mp_cutscene 20261028-161002 passed (132s), including guest quest-card
confirmation, native orb replication and collection, concurrent target selection,
damaging host/guest/host handoff and suppression of the resurrection tutorial.
Unit, mp_locomotion, mp_features and mp_transition passed in preceding runs.
The WaspVictim movement fix was accepted visually by the owner. Its attempted
drift-injection test had invalid preparation and was removed at checkpoint;
the scripted movement binding assertion remains.

Original scope: static audit of implementation and inspection of retained logs.
No runtime scenario was launched, no gameplay code changed, and no new native
behavior was verified in this audit. Existing design documents describe intent;
the implementation is the authority for the observations below.

## Current pipeline

Native presence -> LiveEntityRegistry -> EntityLifecycleReplication ->
host WorldEntityDirectory -> materialization/adoption -> AI/action authority
snapshot -> action, movement and vitals replication.

There are already useful foundations: canonical UID aliases, generations,
map epochs, host canonical records, explicit transfer messages, and immutable
AI decision snapshots. Keep these; a rewrite from zero is unnecessary.

## Findings from code

1. **Map-wide readiness controls individual simulation.**
   EntityMaterializationService::RosterMatches returns false if ANY expected
   creature is missing or mismatched. EntitySimulationAuthority::Refresh uses
   ownerRosterReady before checking a valid local-map entity publisher. Thus a
   pending materialization can fence an otherwise valid assigned NPC. This is
   asymmetric: only the map owner has an ownsLiveMap fallback. This is a static
   mechanism, not proof that every reported combat failure has this cause.
   Extra native actors alone do NOT invalidate readiness in current code.

2. **Lifecycle publication depends on the same readiness loop.**
   EntityLifecycleReplication::Process allows an incremental batch when the
   previous pass was ready (sameOwnedLease && lastOwnedRosterReady_), then clears
   lastOwnedRosterReady_ if the current roster is unready. A prolonged unresolved
   materialization therefore closes publication of further owned changes.
   Admission, reconciliation and simulation should not share one ready boolean.

3. **Simulation reassignment is an immediate grant.**
   AuthorityReplication::AssignEntitySimulation applies a new assignment locally
   and queues a reliable Grant with actionEpoch. There is no prepare/ready/commit
   exchange in this path and no attached current action/target/movement baseline.
   Epoch rejection is valuable but does not make the new peer ready to continue
   an in-flight attack. Audit all downstream epoch fences before changing this.

4. **Nearest-player selection depends on the host's live native population.**
   NearestPlayerSimulationAssigner::Process walks presence_->LiveEntities() and
   ReadPosition uses EntityService::FindByUid on the host. It cannot select or
   reconsider an NPC that exists only in a guest-loaded map. It also returns
   with fewer than two fresh players instead of actively resolving assignments
   in that branch. Ownership/disconnect cleanup elsewhere still needs tracing.
   A host directory plus authoritative transforms is a better selection input.

5. **Script authority can conflict with simulation authority.**
   EntitySimulationAuthority::ShouldSubmitAction gates ordinary native actions
   through CanSimulate; only selected victim reactions explicitly bypass this
   gate there. NearestPlayerSimulationAssigner has no scripted-control exclusion.
   A host quest's action submitted to a guest-owned NPC may therefore be rejected.
   This is a risk requiring a retail script-command trace, not a verified result
   for every quest command. Task 04 remains relevant.

6. **Presence has less meaning than gameplay lifetime.**
   Lifecycle already collapses same-batch same-map unregister/register events
   because Mapwho can rebucket a live Thing. This protection is batch-local.
   Treating cross-batch spatial absence as despawn remains a case to exercise.
   Keep spatial binding, logical life, death, and cross-map transfer distinct.

## Evidence limits

Inspected run 20261027-202921-956-9152 contains roster_ready=false coverage
around world transitions and unloaded maps. Those entries do not establish a
periodic same-map release/reclaim bug. Do not claim a confirmed 10-second timer
from them. Current RosterSweepMilliseconds is 1000. The wasp combat reproduction
and per-UID timeline are still needed to attribute specific owner reports.

## Proposed target design

Host owns logical identity, birth/death, quest consequences and map placement.
A simulation lease owns autonomous AI/attacks; a local binding owns a native
incarnation. A script command is separately authorized by the host. A map lease
owns population admission and initial seeding, not every NPC's runtime health.

Represent these independently:

- Logical life: PendingBirth -> Active -> Dormant / Dead / Retired.
- Local binding: Absent -> Creating -> Bound -> Unloading.
- Simulation: Unassigned -> Preparing -> Active -> Revoking, with its own epoch.
- Map initialization: AwaitingBaseline -> Seeding/Reconstructing -> Established.

After Established, dynamic changes never reset the whole map's simulation.
For a bound Active entity with an unchanged generation and lease, keep its AI
running while another entity is being created. A new NPC waits only for its own
birth and binding. Keep fail-closed behavior for unknown/stale incarnations.

### Dynamic births

A birth proposal carries session/map epoch, creator identity, creator-local
birth token, definition, script/spawn provenance and initial transform. The host
idempotently assigns canonical identity and returns BirthCommitted. Only then
can a replica participate in gameplay. Buffer dependent action/health messages
per entity with bounded count/time and request a baseline if the birth is missing.
Native UID remains a local lookup/alias, not a universal spawn deduplication key.

For quest spawns the host commits the logical birth; for random population the
map population owner proposes it. Audit retail creation side effects before
suppressing duplicate guest quest spawns: never blanket-disable quest actors.

### Handoff

Prepare the target peer with identity, binding revision, transform, health,
current semantic action/target and ownership epoch. Target responds Ready.
Host commits one newer simulation epoch; old source is fenced and target becomes
active. Fail/timeout leaves the old lease active if still valid. Disconnect uses
a host-controlled recovery baseline. This gives a brief bounded freeze when
necessary rather than allowing two valid simulation publishers.

Pin script-controlled phases to host command authority; autonomous AI resumes
under the nearest-player policy after the script releases control. Initial safer
option: pin such NPC simulation to the host while scripting is active, provided
the host has a valid binding. Otherwise command forwarding is required.

Nearest-player remains the policy (including >5m ranged hit / 10s priority), but
should read canonical entity positions and eligible peer bindings. Apply candidate
stability and combat hysteresis before requesting a handoff. Distinguish policy
selection from committing ownership.

### Transfers

Use the existing ObserveTransfer foundation as an explicit transaction:
BeginTransfer(source lease, final state) -> host destination commit -> destination
binding Ready -> activate destination lease. Preserve logical identity; advance
binding/map placement revision. Old source unregister, actions and movement
cannot delete or move the destination incarnation. If destination is unloaded,
keep the entity dormant in the directory until a peer can bind it.

### Reconciliation

Use initial snapshot + committed sequence + deltas. Snapshot completion proves
only that snapshot revision. Absence during an incomplete snapshot is unknown,
not dead. Keep bounded tombstones long enough to reject delayed birth/remove
messages. Existing reliable message ordering does not guarantee ordering across
all independent streams: dependent messages need entity revision prerequisites.

## Incremental implementation order

1. Add per-UID transition reasons and a timeline: native presence, canonical
   revision, binding, roster gate, simulation grant, rejected action reason.
2. Separate Established map initialization from current roster convergence;
   keep valid per-entity simulation during unrelated materialization. Preserve
   initial seeding protection and explicit lease/disconnect revocation.
3. Make birth/retirement and binding reconciliation per entity; bound retry
   queues and distinguish absence from explicit destruction.
4. Centralize simulation selection and add acknowledged handoffs; consume
   canonical positions, enforce peer eligibility and scripted-control policy.
5. Harden transfers and cross-stream prerequisites; retire the old shortcuts
   only after comparable traces prove the replacement.

Each step should be independently reviewable. Do not increase hysteresis alone
and call the lifecycle problem fixed. Do not introduce a universal replacement
AI or serialize the entire retail creature memory.

## Scenarios that decide whether the design works

- New enemy wave during an existing attack: old enemies keep their lease/action.
- Host quest spawns an NPC while guest is nearest: exactly one logical birth,
  both clients bind, script movement completes.
- New owner is loading/disconnected: old valid owner continues or one explicit
  recovery freeze occurs; no dual publisher.
- NPC spatial unregister/register crosses two reconciliation batches: no death
  or duplicate birth.
- NPC transfers A -> B with a delayed A unregister and movement packet: B wins.
- Two players enter an unseeded map together: one initial population authority.
- Late join during a fight: birth/baseline arrives before dependent replay.
- Map unload/reload, native pointer reuse, death and persistent dormant NPCs.

This is a research proposal, not an implemented refactor or a test-pass claim.
