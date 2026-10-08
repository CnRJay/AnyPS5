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

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 8;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 58> Code{
    0x34020084, 0x34060085, 0x34040083, 0x360c0081, 0xd7460007, 0x04090506, 0xe0301000, 0x80000401,
    0xe0301004, 0x80000501, 0xe0301008, 0x80000801, 0xe030100c, 0x80000901, 0xbf8c3f70, 0x7e140280,
    0x7e160280, 0xd9340000, 0x00000a0a, 0xd9340010, 0x00000402, 0xbf8cc07f, 0xbf8a0000, 0xd9f80010,
    0x0c000807, 0xbf8cc07f, 0xd9d80010, 0x0e000002, 0x382000ff, 0x80000000, 0x4a2200ff, 0x00000064,
    0x382222ff, 0x80000000, 0xd9f80000, 0x1200100a, 0xbf8cc07f, 0xbf8a0000, 0xd9d80000, 0x1400000a,
    0xbf8cc07f, 0xe0701000, 0x80010c03, 0xe0701004, 0x80010d03, 0xe0701008, 0x80010e03, 0xe070100c,
    0x80010f03, 0xe0701010, 0x80011203, 0xe0701014, 0x80011303, 0xe0701018, 0x80011403, 0xe070101c,
    0x80011503, 0xbf810000,
};

constexpr std::uint32_t Sign = 0x80000000u;

void Fill(std::uint32_t tid, std::uint32_t* words) {
    const std::uint64_t seed = (tid + 1u) * 0x9e3779b97f4a7c15ull;
    words[0] = static_cast<std::uint32_t>(seed);
    words[1] = static_cast<std::uint32_t>(seed >> 32u);
    const auto data = seed * 0xbf58476d1ce4e5b9ull;
    words[2] = (static_cast<std::uint32_t>(data) & ~Sign) | ((tid & 1u) != 0u ? Sign : 0u);
    words[3] = (static_cast<std::uint32_t>(data >> 32u) & ~Sign) | ((tid & 2u) != 0u ? Sign : 0u);
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Expect(std::uint32_t tid, std::uint32_t actual, std::uint32_t expected, const std::string& name) {
    Require(actual == expected, "lds condxchg: lane " + std::to_string(tid) + " " + name + " is " + Hex(actual) + ", expected " + Hex(expected));
}

void Run(AgcDriver::VulkanDevice& device) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) Fill(tid, &Input[tid * Inputs]);
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 128u, {false, false, false}, false, 1};
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

void CheckLanes() {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t* in = &Input[tid * Inputs];
        const std::uint32_t* out = &Output[tid * Results];
        Expect(tid, out[0], in[0], "returned low dword");
        Expect(tid, out[1], in[1], "returned high dword");
        Expect(tid, out[2], (in[2] & Sign) != 0u ? in[2] & ~Sign : in[0], "low dword");
        Expect(tid, out[3], (in[3] & Sign) != 0u ? in[3] & ~Sign : in[1], "high dword");
    }
}

void CheckContention() {
    const std::uint32_t low = Output[6];
    const std::uint32_t high = Output[7];
    Require(low < Threads && high == low + 100u, "lds condxchg: contended pair " + Hex(low) + " " + Hex(high) + " was not written by one lane");
    std::vector<bool> seen(Threads, false);
    std::uint32_t initial = 0;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t* out = &Output[tid * Results];
        Expect(tid, out[6], low, "final contended low dword");
        Expect(tid, out[7], high, "final contended high dword");
        if (out[4] == 0u && out[5] == 0u) {
            ++initial;
            continue;
        }
        Require(out[4] < Threads && out[5] == out[4] + 100u && out[4] != tid && !seen[out[4]], "lds condxchg: lane " + std::to_string(tid) + " returned the contended pair " + Hex(out[4]) + " " + Hex(out[5]));
        seen[out[4]] = true;
    }
    Require(initial == 1u && !seen[low], "lds condxchg: contended returned values do not form one serial order");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        CheckLanes();
        CheckContention();
        std::puts("lds condxchg tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
