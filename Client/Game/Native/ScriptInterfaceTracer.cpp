#include "ScriptInterfaceTracer.h"

#include "Game/Native/Addresses.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // Script-interface functions the mod itself validates by comparing the
    // vtable entry with this address; those slots must stay untouched or the
    // mod refuses to run (it never becomes ready, so doors and travel stall).
    constexpr std::uintptr_t ValidatedFunctionRvas[] = {
        0x01885870, 0x01889940, 0x0188ADF0, 0x0188AE90, 0x0188B320, 0x0188B410,
        0x0188C510, 0x0188E6A0, 0x0188E720, 0x0188E790, 0x0188E890, 0x0188EA20,
        0x0188ED10, 0x0188FFC0, 0x018901C0, 0x018903A0, 0x01890460, 0x01890660,
        0x018916A0, 0x01891D90, 0x01891E50, 0x01891E60, 0x01891E70, 0x01891E80,
        0x01897BF0, 0x01898200, 0x0189A230, 0x0189DF10, 0x0189EC00, 0x0189EE20,
        0x018A1780, 0x018A17F0, 0x018AF5F0, 0x018AF800};

    bool IsValidatedByMod(const std::uintptr_t base, const void* const function) noexcept
    {
        for (const std::uintptr_t rva : ValidatedFunctionRvas)
        {
            if (reinterpret_cast<std::uintptr_t>(function) == base + rva) return true;
        }
        return false;
    }
}

namespace fable::game::native
{
    std::array<std::atomic_uint32_t, ScriptInterfaceTracer::MaximumSlots> ScriptInterfaceTracer::counts_ = {};

    void __cdecl ScriptInterfaceTracer::Count(const std::uint32_t slot) noexcept
    {
        if (slot < MaximumSlots) counts_[slot].fetch_add(1, std::memory_order_relaxed);
    }

    bool ScriptInterfaceTracer::Install(const HMODULE gameModule, const core::Diagnostics& diagnostics) noexcept
    {
#if !defined(_M_IX86)
        (void)gameModule;
        (void)diagnostics;
        return false;
#else
        char flag[8] = {};
        const DWORD length = GetEnvironmentVariableA("ALBIONTOGETHER_TRACE_SCRIPT_INTERFACE", flag, sizeof(flag));
        if (length == 0 || length >= sizeof(flag) || flag[0] != '1' || gameModule == nullptr || vtable_ != nullptr)
        {
            return false;
        }
        diagnostics_ = diagnostics;
        const auto base = reinterpret_cast<std::uintptr_t>(gameModule);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
        auto** const vtable = reinterpret_cast<void**>(base + rva::GameScriptInterfaceVtable);
        std::size_t count = 0;
        while (count < MaximumSlots)
        {
            const auto entry = reinterpret_cast<std::uintptr_t>(vtable[count]);
            if (entry < base + 0x1000 || entry >= end) break;
            ++count;
        }
        if (count == 0) return false;
        thunks_ = static_cast<std::uint8_t*>(VirtualAlloc(
            nullptr, count * ThunkBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (thunks_ == nullptr) return false;
        for (std::size_t slot = 0; slot < count; ++slot)
        {
            originals_[slot] = vtable[slot];
            // pushad; pushfd; push slot; call Count; add esp,4; popfd; popad;
            // jmp [originals_[slot]]   (registers and stack arguments intact)
            std::uint8_t* t = thunks_ + slot * ThunkBytes;
            std::size_t at = 0;
            t[at++] = 0x60;
            t[at++] = 0x9C;
            t[at++] = 0x68;
            const auto slotValue = static_cast<std::uint32_t>(slot);
            std::memcpy(t + at, &slotValue, 4); at += 4;
            t[at++] = 0xE8;
            const auto call = static_cast<std::int32_t>(
                reinterpret_cast<std::uintptr_t>(&Count) - (reinterpret_cast<std::uintptr_t>(t) + at + 4));
            std::memcpy(t + at, &call, 4); at += 4;
            t[at++] = 0x83; t[at++] = 0xC4; t[at++] = 0x04;
            t[at++] = 0x9D;
            t[at++] = 0x61;
            t[at++] = 0xFF; t[at++] = 0x25;
            const auto target = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&originals_[slot]));
            std::memcpy(t + at, &target, 4); at += 4;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(vtable, count * sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            VirtualFree(thunks_, 0, MEM_RELEASE);
            thunks_ = nullptr;
            return false;
        }
        std::size_t skipped = 0;
        for (std::size_t slot = 0; slot < count; ++slot)
        {
            if (IsValidatedByMod(base, originals_[slot]))
            {
                ++skipped;
                continue;
            }
            vtable[slot] = thunks_ + slot * ThunkBytes;
        }
        DWORD ignored = 0;
        VirtualProtect(vtable, count * sizeof(void*), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), thunks_, count * ThunkBytes);
        vtable_ = vtable;
        slotCount_ = count;
        char detail[96] = {};
        std::snprintf(detail, sizeof(detail), "slots=%zu skipped_validated=%zu", count, skipped);
        diagnostics_.Event("ScriptInterfaceTracerReady", detail);
        return true;
#endif
    }

    void ScriptInterfaceTracer::Shutdown() noexcept
    {
        if (vtable_ == nullptr) return;
        DWORD oldProtect = 0;
        if (VirtualProtect(vtable_, slotCount_ * sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            for (std::size_t slot = 0; slot < slotCount_; ++slot) vtable_[slot] = originals_[slot];
            DWORD ignored = 0;
            VirtualProtect(vtable_, slotCount_ * sizeof(void*), oldProtect, &ignored);
        }
        // Thunks stay allocated: a script thread may still be inside one.
        vtable_ = nullptr;
        slotCount_ = 0;
    }

    void ScriptInterfaceTracer::Process() noexcept
    {
        if (vtable_ == nullptr) return;
        const std::uint64_t now = GetTickCount64();
        if (now < nextReportAt_) return;
        nextReportAt_ = now + ReportIntervalMilliseconds;
        std::string detail;
        try
        {
            for (std::size_t slot = 0; slot < slotCount_; ++slot)
            {
                const std::uint32_t total = counts_[slot].load(std::memory_order_relaxed);
                const std::uint32_t delta = total - reported_[slot];
                if (delta == 0) continue;
                reported_[slot] = total;
                char item[24] = {};
                std::snprintf(item, sizeof(item), "%zu:%u ", slot, delta);
                detail += item;
            }
        }
        catch (...)
        {
            return;
        }
        if (!detail.empty()) diagnostics_.Event("ScriptInterfaceCalls", detail.c_str());
    }
}
