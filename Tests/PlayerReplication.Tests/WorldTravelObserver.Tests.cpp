#include "Game/World/Travel/Hooks/WorldTravelObserver.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace fable::game::world::travel
{
    // Exercise the production observation boundary without installing game hooks.
    struct WorldTravelObserverTestAccess final
    {
        static void Bind(WorldTravelObserver& observer, HMODULE image,
            native::WorldTravelFunctions::ResolveConnectedThingPointer resolve)
        {
            observer.gameModule_ = image;
            observer.resolveConnectedThing_ = resolve;
        }
        static void Propose(WorldTravelObserver& observer, void* exit)
        {
            observer.ObserveRegionExit(exit);
        }
        static bool Prepare(WorldTravelObserver& observer, void* world,
            const native::NativeNameArray& names)
        {
            return observer.ObservePrepareMapChange(world, &names);
        }
        static void SetOriginal(WorldTravelObserver& observer,
            native::WorldTravelFunctions::PrepareMapChangePointer original)
        {
            observer.originalPrepareMapChange_ = original;
        }
    };
}

namespace
{
    using namespace fable::game::world::travel;
    using Access = WorldTravelObserverTestAccess;
    using Functions = native::WorldTravelFunctions;

    struct Probe final
    {
        unsigned int departures = 0;
        unsigned int preparations = 0;
        unsigned int nativeCalls = 0;
        bool allow = true;
        WorldTravelPreparation departure;
        WorldTravelPreparation preparation;
        static void Depart(void* context, const WorldTravelPreparation& value) noexcept
        {
            auto& probe = *static_cast<Probe*>(context);
            ++probe.departures;
            probe.departure = value;
        }
        static bool Prepare(void* context, const WorldTravelPreparation& value) noexcept
        {
            auto& probe = *static_cast<Probe*>(context);
            ++probe.preparations;
            probe.preparation = value;
            return probe.allow;
        }
    };

    void* __fastcall ResolveTarget(void* reference, void*)
    {
        void* result = nullptr;
        std::memcpy(&result, reference, sizeof(result));
        return result;
    }

    void __fastcall NativePrepare(void* world, void*, const native::NativeNameArray* names)
    {
        auto& probe = *static_cast<Probe*>(world);
        if (names != nullptr && names->count == 1 && names->data[0].index == 0)
            ++probe.nativeCalls;
    }
}

int RunWorldTravelObserverTests()
{
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description)
    {
        if (!condition) { ++failures; std::fprintf(stderr, "travel: %s\n", description); }
    };
    auto* const image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr,
        Functions::NameEntryCountRva + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (image == nullptr) return 1;
    std::array<std::uint8_t, 64> entry{};
    std::memcpy(entry.data() + 0x10, "Destination", sizeof("Destination"));
    void* entries[] = {entry.data()};
    *reinterpret_cast<void***>(image + Functions::NameEntryTableSlotRva) = entries;
    *reinterpret_cast<std::uint32_t*>(image + Functions::NameEntryCountRva) = 1;

    std::array<std::uint8_t, 0xA0> source{}, destination{};
    std::array<std::uint8_t, 0x40> exit{};
    void* sourcePointer = source.data();
    void* destinationPointer = destination.data();
    const std::uint64_t sourceUid = 101, destinationUid = 202;
    const std::uint16_t sourceMap = 7, destinationMap = 8;
    std::memcpy(source.data() + 0x14, &sourceUid, sizeof(sourceUid));
    std::memcpy(destination.data() + 0x14, &destinationUid, sizeof(destinationUid));
    std::memcpy(source.data() + 0x9A, &sourceMap, sizeof(sourceMap));
    std::memcpy(destination.data() + 0x9A, &destinationMap, sizeof(destinationMap));
    std::memcpy(exit.data() + 0x04, &sourcePointer, sizeof(sourcePointer));
    std::memcpy(exit.data() + 0x26, &destinationPointer, sizeof(destinationPointer));

    WorldTravelObserver observer;
    Probe probe;
    Access::Bind(observer, reinterpret_cast<HMODULE>(image),
        reinterpret_cast<Functions::ResolveConnectedThingPointer>(&ResolveTarget));
    observer.SetDepartureSink(&Probe::Depart, &probe);
    observer.SetPreparationSink(&Probe::Prepare, &probe);
    // Retail may show Leave/Stay and return to play without any PrepareMapChange.
    Access::Propose(observer, exit.data());
    Access::Propose(observer, exit.data());
    check(probe.departures == 0 && probe.preparations == 0,
        "cancelled or repeated exit proposals must not retire the source world");
    check(!observer.ResumeDeferredMapChange("Destination", destinationMap),
        "a proposal alone must not queue native loading");

    native::NativeName name;
    native::NativeNameArray names{&name, 1, 1};
    native::NativeNameArray invalid{&name, 2, 1};
    check(Access::Prepare(observer, &probe, invalid), "invalid names pass through");
    check(probe.departures == 0, "invalid names must not confirm departure");
    probe.allow = false;
    Access::SetOriginal(observer,
        reinterpret_cast<Functions::PrepareMapChangePointer>(&NativePrepare));
    check(!Access::Prepare(observer, &probe, names), "confirmed load may wait for baseline");
    check(probe.departures == 1 && probe.preparations == 1,
        "confirmed load publishes departure and preparation once");
    check(probe.departure.sourceMapId == sourceMap &&
        probe.departure.destinationMapId == destinationMap &&
        probe.departure.sourceExitUid == sourceUid &&
        probe.departure.destinationEntranceUid == destinationUid &&
        std::strcmp(probe.preparation.destinationMapName.data(), "Destination") == 0,
        "confirmed departure retains the native route identity");
    check(!observer.ResumeDeferredMapChange("OtherMap", destinationMap),
        "unrelated baseline cannot release held construction");
    check(observer.ResumeDeferredMapChange("Destination", destinationMap),
        "matching baseline releases native loading");
    check(!observer.ResumeDeferredMapChange("Destination", destinationMap) &&
        probe.nativeCalls == 1 && probe.departures == 1,
        "native loading and departure cannot repeat on release");
    observer.Shutdown();
    VirtualFree(image, 0, MEM_RELEASE);
    return failures;
}
