#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// buffer_store_format_xyzw of 16_16_16_16 and 8_8_8_8 elements whose components fill whole dwords:
// at dword-aligned addresses they are written as dwords, elsewhere component by component. Both
// must leave the same bytes, and the bytes around each element untouched.
namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t ThreadBytes = 64;
constexpr std::uint32_t Format16x4UInt = 69;
constexpr std::uint32_t Format8x4UInt = 60;
alignas(256) std::array<std::uint8_t, Threads * ThreadBytes> Output{};

struct Store {
    std::uint32_t offset;
    bool wide;
};
constexpr std::array<Store, 5> Stores{{{0u, true}, {10u, true}, {24u, false}, {33u, false}, {42u, false}}};

// v3 = tid * 64; v4..v7 = tid + 0x100 * (1..4); v8..v11 = tid + 0x10 * (1..4); one store per Stores entry.
alignas(256) constexpr std::array<std::uint32_t, 28> Code{
    0x34060086,
    0x4a0800ff, 0x00000100, 0x4a0a00ff, 0x00000200, 0x4a0c00ff, 0x00000300, 0x4a0e00ff, 0x00000400,
    0x4a1000ff, 0x00000010, 0x4a1200ff, 0x00000020, 0x4a1400ff, 0x00000030, 0x4a1600ff, 0x00000040,
    0xe01c1000, 0x80010403, 0xe01c100a, 0x80010403,
    0xe01c1018, 0x80020803, 0xe01c1021, 0x80020803, 0xe01c102a, 0x80020803,
    0xbf810000,
};

std::array<std::uint32_t, 4> Descriptor(const void* data, std::uint32_t bytes, std::uint32_t format) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, (0x31016facu & ~(0x7fu << 12u)) | (format << 12u)};
}

std::vector<std::uint8_t> Expected() {
    std::vector<std::uint8_t> bytes(Output.size(), 0xaau);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (const auto& store : Stores) {
            for (std::uint32_t component = 0; component < 4u; ++component) {
                const auto size = store.wide ? 2u : 1u;
                const auto value = store.wide ? tid + 0x100u * (component + 1u) : tid + 0x10u * (component + 1u);
                std::memcpy(bytes.data() + tid * ThreadBytes + store.offset + component * size, &value, size);
            }
        }
    }
    return bytes;
}

void Run(AgcDriver::VulkanDevice& device) {
    Output.fill(0xaau);
    std::vector<std::uint32_t> userData(12, 0u);
    const auto wide = Descriptor(Output.data(), static_cast<std::uint32_t>(Output.size()), Format16x4UInt);
    const auto narrow = Descriptor(Output.data(), static_cast<std::uint32_t>(Output.size()), Format8x4UInt);
    std::copy(wide.begin(), wide.end(), userData.begin() + 4);
    std::copy(narrow.begin(), narrow.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check() {
    const auto expected = Expected();
    for (std::size_t index = 0; index < Output.size(); ++index) {
        if (Output[index] == expected[index]) continue;
        char text[160];
        std::snprintf(text, sizeof(text), "formatted store words: thread %zu byte %zu is 0x%02x, expected 0x%02x", index / ThreadBytes, index % ThreadBytes, Output[index], expected[index]);
        Require(false, text);
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        Check();
        std::puts("formatted store words tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
