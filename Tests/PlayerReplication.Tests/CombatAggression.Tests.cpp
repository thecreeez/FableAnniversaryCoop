#include "Multiplayer/Combat/CombatAggressionState.h"
#include "Game/Creature/Combat/Native/AiTargetingComponent.h"
#include "Game/Creature/Control/ScriptControl.h"
#include "Game/Entity/Native/ThingComponentAccess.h"

#include <array>
#include <cstring>
#include <iostream>

namespace
{
    using namespace fable::multiplayer;
    using namespace fable::multiplayer::protocol;
    using Native = fable::game::creature::combat::native::AiTargetingComponent;
    using ComponentEntry = fable::game::entity::native::ThingComponentEntry;
    bool scripted = false;
    bool friendly = false;
    int failures = 0, additions = 0, removals = 0;
    void Check(bool condition, const char* expression)
    {
        if (!condition) { ++failures; std::cerr << "combat aggression: " << expression << '\n'; }
    }
#define AGGRESSION_CHECK(value) Check((value), #value)

    struct Node
    {
        Node* next = this;
        Node* previous = this;
        void* vtable = nullptr;
        void** header = nullptr;
    };

    alignas(void*) std::array<unsigned char, 0x40> enemyComponent;
    alignas(void*) std::array<unsigned char, 0x180> npc, guest, host;
    Node sentinel;
    ComponentEntry componentEntry;

    void* FindInList(void* target)
    {
        for (auto* node = sentinel.next; node != &sentinel; node = node->next)
            if (node->header != nullptr && *node->header == target) return target;
        return nullptr;
    }

    void __fastcall AddEnemy(void*, void*, void* target)
    {
        ++additions;
        if (FindInList(target) != nullptr) return;
        auto* node = new Node;
        node->header = new void*(target);
        node->next = &sentinel;
        node->previous = sentinel.previous;
        sentinel.previous->next = node;
        sentinel.previous = node;
    }

    void __fastcall RemoveEnemy(void*, void*, void* target)
    {
        ++removals;
        auto* node = sentinel.next;
        while (node != &sentinel)
        {
            auto* next = node->next;
            if (*node->header == target)
            {
                node->previous->next = next;
                next->previous = node->previous;
                delete node->header;
                delete node;
            }
            node = next;
        }
    }

    bool __cdecl IsEnemy(void*, void* target)
    {
        return !friendly && FindInList(target) != nullptr;
    }

    // Execute the production adapter against a small synthetic native image:
    // validated retail prologues, real x86 cdecl/thiscall entry, stubbed bodies.
    class Image final
    {
    public:
        unsigned char* base = static_cast<unsigned char*>(VirtualAlloc(
            nullptr, 0x01A00000, MEM_RESERVE, PAGE_NOACCESS));
        ~Image() { if (base != nullptr) VirtualFree(base, 0, MEM_RELEASE); }
        bool Write(std::uintptr_t rva, const unsigned char* data, std::size_t size)
        {
            if (base == nullptr || VirtualAlloc(base + (rva & ~0xFFFu),
                    0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == nullptr) return false;
            std::memcpy(base + rva, data, size);
            return FlushInstructionCache(GetCurrentProcess(), base + rva, size) != FALSE;
        }
        bool Mutator(std::uintptr_t rva, std::uint32_t preferredHandlerVa, void* body)
        {
            unsigned char bytes[] = {0x6A,0xFF,0x68,0,0,0,0,0x64,0xA1,0,0,0,0,
                0x83,0xC4,0x08,0xB8,0,0,0,0,0xFF,0xE0};
            // Independent retail disassembly operands are preferred VAs.
            // Relocate them like the PE loader, not like production RVAs.
            const auto address = reinterpret_cast<std::uint32_t>(base) +
                (preferredHandlerVa - 0x00400000);
            std::memcpy(bytes + 3, &address, 4);
            std::memcpy(bytes + 17, &body, 4);
            return Write(rva, bytes, sizeof(bytes));
        }
        bool Initialize()
        {
            unsigned char predicate[] = {0x83,0xEC,0x0C,0x53,0x55,0x56,0x8B,0x74,0x24,0x1C,
                0x5E,0x5D,0x5B,0x83,0xC4,0x0C,0xB8,0,0,0,0,0xFF,0xE0};
            auto* body = reinterpret_cast<void*>(&IsEnemy);
            std::memcpy(predicate + 17, &body, 4);
            const unsigned char weak[] = {0x8B,0x41,0x04,0x85,0xC0,0x74,0x02,0x8B,0x00,0xC3};
            return Mutator(Native::AddEnemyRva, 0x0292DA21, reinterpret_cast<void*>(&AddEnemy)) &&
                Mutator(Native::RemoveEnemyRva, 0x0292DA50, reinterpret_cast<void*>(&RemoveEnemy)) &&
                Write(Native::EnemyRelationRva, predicate, sizeof(predicate)) &&
                Write(0x012E6EA0, weak, sizeof(weak));
        }
    };

    void TestEvidence()
    {
        combat::CombatAggressionState state;
        CombatHitMessage hit;
        hit.phase = CombatHitPhase::Result;
        hit.sourceDomain = CombatActionDomain::Player;
        hit.targetKind = CombatParticipantKind::WorldEntity;
        hit.sourceId = 2002; hit.targetId = 99;
        hit.sourceAuthorityEpoch = 700; hit.sourceGeneration = 2;
        hit.sourceMapEpoch = 3; hit.sourceMapId = 64;
        hit.targetGeneration = 23; hit.targetMapEpoch = 1; hit.targetMapId = 64;
        hit.hostTargetRevision = 1; hit.healthBefore = 40; hit.healthAfter = 38;
        PlayerState attacker;
        attacker.actorId = 2002; attacker.authorityEpoch = 700;
        attacker.actorGeneration = 2; attacker.mapEpoch = 3; attacker.mapId = 64;
        AGGRESSION_CHECK(state.Observe(hit, 100, 123));
        AGGRESSION_CHECK(state.Allows(99, 23, 1, attacker, 101, 123));
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, attacker, 101, 456));
        auto innocent = attacker; innocent.actorId = 1001;
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, innocent, 101, 123));
        AGGRESSION_CHECK(!state.Allows(99, 24, 1, attacker, 101, 123));
        AGGRESSION_CHECK(!state.Allows(99, 23, 2, attacker, 101, 123));
        auto rebound = attacker; ++rebound.actorGeneration;
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, rebound, 101, 123));
        rebound = attacker; ++rebound.authorityEpoch;
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, rebound, 101, 123));
        rebound = attacker; ++rebound.mapEpoch;
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, rebound, 101, 123));
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, attacker, 10'101, 123));
        AGGRESSION_CHECK(!state.Observe(hit, 10'100, 123)); // Echo cannot extend evidence.
        hit.phase = CombatHitPhase::Candidate; hit.hostTargetRevision = 2;
        AGGRESSION_CHECK(!state.Observe(hit, 200, 123));
        hit.phase = CombatHitPhase::Result; hit.healthAfter = hit.healthBefore;
        AGGRESSION_CHECK(!state.Observe(hit, 200, 123));
        hit.healthAfter = 35;
        hit.sourceDomain = CombatActionDomain::WorldEntity;
        AGGRESSION_CHECK(!state.Observe(hit, 200, 123));
        hit.sourceDomain = CombatActionDomain::Player;
        hit.targetKind = CombatParticipantKind::Player;
        AGGRESSION_CHECK(!state.Observe(hit, 200, 123));
        hit.targetKind = CombatParticipantKind::WorldEntity;
        AGGRESSION_CHECK(!state.Observe(hit, 200, 0));
        state.Clear();
        AGGRESSION_CHECK(!state.Allows(99, 23, 1, attacker, 101, 123));
    }

    void TestNativePairOwnership()
    {
        enemyComponent = {}; npc = {}; guest = {}; host = {};
        sentinel.next = sentinel.previous = &sentinel;
        componentEntry = {static_cast<std::int32_t>(
            fable::game::entity::native::ThingComponentType::Enemy),
            enemyComponent.data()};
        *reinterpret_cast<std::uint64_t*>(guest.data() + 0x14) = 2002;
        *reinterpret_cast<std::uint64_t*>(host.data() + 0x14) = 1001;
        *reinterpret_cast<ComponentEntry**>(npc.data() + 0x44) = &componentEntry;
        *reinterpret_cast<ComponentEntry**>(npc.data() + 0x48) = &componentEntry + 1;
        *reinterpret_cast<Node**>(enemyComponent.data() + 0x10) = &sentinel;
        Image image;
        const bool initialized = image.Initialize();
        AGGRESSION_CHECK(initialized);
        if (!initialized) return;
        auto module = reinterpret_cast<HMODULE>(image.base);
        bool added = false, enemy = false;
        std::uint64_t uid = 0;
        const auto healthBefore = guest; // Pair mutation must not apply damage.
        AGGRESSION_CHECK(Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(added && uid == 2002);
        AGGRESSION_CHECK(Native::ReadEnemyRelation(module, npc.data(), guest.data(), enemy) && enemy);
        AGGRESSION_CHECK(Native::ReadEnemyRelation(module, npc.data(), host.data(), enemy) && !enemy);
        AGGRESSION_CHECK(guest == healthBefore);
        bool duplicateAdded = true;
        AGGRESSION_CHECK(Native::EnsureAttackEnemy(module, npc.data(), guest.data(), duplicateAdded, uid));
        AGGRESSION_CHECK(!duplicateAdded && additions == 1);
        AGGRESSION_CHECK(Native::ReleaseAttackEnemy(module, npc.data(), 2002));
        AGGRESSION_CHECK(Native::ReadEnemyRelation(module, npc.data(), guest.data(), enemy) && !enemy);
        // A pre-existing pair remains native-owned.
        AddEnemy(enemyComponent.data(), nullptr, guest.data());
        AGGRESSION_CHECK(Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(!added && FindInList(guest.data()) != nullptr);
        friendly = true;
        AGGRESSION_CHECK(!Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(!added && FindInList(guest.data()) != nullptr);
        friendly = false;
        RemoveEnemy(enemyComponent.data(), nullptr, guest.data());
        friendly = true;
        AGGRESSION_CHECK(!Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(!added && FindInList(guest.data()) == nullptr);
        friendly = false;
        scripted = true;
        AGGRESSION_CHECK(!Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(!added);
        scripted = false;
        // Releasing an expired weak target must not dereference a stale body.
        AddEnemy(enemyComponent.data(), nullptr, guest.data());
        *sentinel.next->header = nullptr;
        const int previousRemovals = removals;
        AGGRESSION_CHECK(Native::ReleaseAttackEnemy(module, npc.data(), 2002));
        AGGRESSION_CHECK(removals == previousRemovals);
        RemoveEnemy(enemyComponent.data(), nullptr, nullptr);
        image.base[Native::AddEnemyRva] = 0x90;
        const int calls = additions;
        AGGRESSION_CHECK(!Native::EnsureAttackEnemy(module, npc.data(), guest.data(), added, uid));
        AGGRESSION_CHECK(!added && additions == calls);
    }
}

namespace fable::game
{
    bool ScriptControl::HasNativeControl(void*) noexcept { return scripted; }
}

int RunCombatAggressionTests()
{
    TestEvidence();
    TestNativePairOwnership();
    return failures;
}
