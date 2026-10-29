# Co-op validation — 2026-10-30

Owner acceptance of the current protocol50 build:
- Local weapons remain visible after leaving town; the reported owner/observer weapon mismatch is fixed.
- Checkpoint loading no longer produces the reported crash in the owner's follow-up playtest.

The latest changes restore local weapons through the retail inventory path, suspend native presentation and background movement for the complete checkpoint barrier, and retire native movement routes/references before loading the world. Build: Release Win32 solution passed. PlayerReplication.Tests passed. The full in-game suite was not rerun for this snapshot.

## Open: NPC attacks intermittently fail to damage the guest

Owner reports that some NPC attacks still do not deal damage to the guest. Attacks not originating from NPCs consistently get through, according to the owner. This is an open defect; the weapon/reload acceptance does not establish correct NPC hit delivery.

Investigate source-owned NPC hit capture and action-ledger attribution, host admission, target-owner damage application, and the block/reaction outcome. Correlate the same landed hit across host and guest before changing damage or authority rules. Do not assume every missing hit is caused by blocking, animation playback, or transport loss.

## Snapshot scope

This commit records the accumulated shared-world/checkpoint, clock/weather, cutscene, presentation/effect and combat replication integration required by the current build. Owner acceptance above is limited to the specifically reported weapon and reload regressions; it does not mark every integrated feature or all combat behavior as verified.

The supplied earlier crash dump identifies an invalid pending-component list in CThing::vf22 at RVA01B2E373. The precise corrupting write was not established. Follow-up playtest acceptance confirms that the reported crash stopped after the lifetime/barrier changes, without proving that every possible reload crash is eliminated.
