#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

// Buffer atomics through a V# known at compile time go through the descriptor-bound storage buffer:
// the recompile binds no BDA pagetable, and the atomics keep their results, returned values and the
// rule that the two low address bits of a dword atomic are ignored. Run as wave32 and wave64.
namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Words = 1024;
constexpr std::uint32_t Add = 0x000u / 4u;
constexpr std::uint32_t CompareSwap = 0x200u / 4u;
constexpr std::uint32_t Misaligned = 0x400u / 4u;
constexpr std::uint32_t Minimum = 0x700u / 4u;
constexpr std::uint32_t Maximum = 0x704u / 4u;
constexpr std::uint32_t AddReturned = 0x800u / 4u;
constexpr std::uint32_t CompareSwapReturned = 0x900u / 4u;
constexpr std::uint32_t MisalignedReturned = 0xa00u / 4u;
alignas(256) std::array<std::uint32_t, Words> Buffer{};

// v1 = tid * 4
// buffer_atomic_add v2 = tid + 1 at v1 glc; buffer_atomic_smin tid - 16 and buffer_atomic_smax tid * 3 at one dword each;
// buffer_atomic_cmpswap v[6:7] = {tid + 0x1000, 0x500 + (tid & ~1)} at v1 + 0x200 glc;
// buffer_atomic_add v8 = 0x10000 at v1 + 0x402 glc (two bytes past its dword); the returned values are stored.
alignas(256) constexpr std::array<std::uint32_t, 32> Code{
    0x34020082,
    0x4a040081, 0xe0c85000, 0x80010201,
    0x4a0600ff, 0xfffffff0, 0xe0d40700, 0x80010300,
    0x160a0083, 0xe0dc0704, 0x80010500,
    0x4a0c00ff, 0x00001000, 0x360e00ff, 0xfffffffe, 0x4a0e0eff, 0x00000500, 0xe0c45200, 0x80010601,
    0x7e1002ff, 0x00010000, 0xe0c85402, 0x80010801,
    0xbf8c3f70,
    0xe0701800, 0x80010201, 0xe0701900, 0x80010601, 0xe0701a00, 0x80010801,
    0xbf8c3f70, 0xbf810000,
};

std::array<std::uint32_t, 4> Descriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Expect(std::uint32_t actual, std::uint32_t expected, const std::string& what) {
    Require(actual == expected, "buffer atomics descriptor: " + what + " is " + Hex(actual) + ", expected " + Hex(expected));
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
    Buffer.fill(0u);
    for (std::uint32_t tid = 0; tid < waveSize; ++tid) {
        Buffer[Add + tid] = 0x100u * tid;
        Buffer[CompareSwap + tid] = 0x500u + tid;
        Buffer[Misaligned + tid] = 0x20u + tid;
    }
    Buffer[Minimum] = 5u;
    Buffer[Maximum] = 7u;
    std::vector<std::uint32_t> userData(8, 0u);
    const auto buffer = Descriptor(Buffer.data(), static_cast<std::uint32_t>(Buffer.size() * 4u));
    std::copy(buffer.begin(), buffer.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{waveSize, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    const bool addressBased = std::any_of(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable; });
    Require(!addressBased, "buffer atomics descriptor: atomics through a known V# bound the BDA pagetable");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint32_t waveSize) {
    const auto wave = " (wave" + std::to_string(waveSize) + ")";
    for (std::uint32_t tid = 0; tid < waveSize; ++tid) {
        const auto lane = "lane " + std::to_string(tid) + " ";
        Expect(Buffer[Add + tid], 0x100u * tid + tid + 1u, lane + "add result" + wave);
        Expect(Buffer[AddReturned + tid], 0x100u * tid, lane + "add returned value" + wave);
        const bool swapped = (tid & 1u) == 0u;
        Expect(Buffer[CompareSwap + tid], swapped ? tid + 0x1000u : 0x500u + tid, lane + "cmpswap result" + wave);
        Expect(Buffer[CompareSwapReturned + tid], 0x500u + tid, lane + "cmpswap returned value" + wave);
        Expect(Buffer[Misaligned + tid], 0x20u + tid + 0x10000u, lane + "add two bytes past its dword" + wave);
        Expect(Buffer[MisalignedReturned + tid], 0x20u + tid, lane + "returned value of the add two bytes past its dword" + wave);
    }
    Expect(Buffer[Minimum], 0xfffffff0u, "smin of every lane" + wave);
    Expect(Buffer[Maximum], 3u * (waveSize - 1u), "smax of every lane" + wave);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const std::uint32_t waveSize : {32u, 64u}) {
            Run(*device, waveSize);
            Check(waveSize);
        }
        std::puts("buffer atomics descriptor tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
