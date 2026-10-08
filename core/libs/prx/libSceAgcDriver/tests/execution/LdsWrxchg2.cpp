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
constexpr std::uint32_t Inputs = 8;
constexpr std::uint32_t Results = 32;
constexpr std::uint32_t SecondOffset = 101;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 84> Code{
    0x34020085, 0xe0381000, 0x80000201, 0xe0381010, 0x80000601, 0x34580084, 0x345a0082, 0x4a5a5aff,
    0x00000200, 0x345c0085, 0x4a5c5cff, 0x00000480, 0x345e0083, 0x4a5e5eff, 0x00000880, 0x34600082,
    0x4a6060ff, 0x00000b80, 0x7e6202ff, 0x00000c00, 0x7e4e0280, 0x4a4a0081, 0x4a4c00ff, 0x00000065,
    0xbf8c3f70, 0xd8380301, 0x0003022c, 0xd8b80301, 0x0a07062c, 0xd8dc0301, 0x0c00002c, 0xd83c0200,
    0x0003022d, 0xd8bc0200, 0x0e07062d, 0xd8e00200, 0x1000002d, 0xd9380301, 0x0004022e, 0xd9b80301,
    0x1208062e, 0xd9dc0301, 0x1600002e, 0xd93c0100, 0x0004022f, 0xd9bc0100, 0x1a08062f, 0xd9e00100,
    0x1e00002f, 0xd8340000, 0x00000230, 0xd8b80000, 0x22070630, 0xd8d80000, 0x24000030, 0xd8380100,
    0x00272731, 0xbf8cc07f, 0xbf8a0000, 0xd8b80100, 0x28262531, 0xbf8cc07f, 0xbf8a0000, 0xd8dc0100,
    0x2a000031, 0x34020087, 0xbf8cc07f, 0xe0781000, 0x80010a01, 0xe0781010, 0x80010e01, 0xe0781020,
    0x80011201, 0xe0781030, 0x80011601, 0xe0781040, 0x80011a01, 0xe0781050, 0x80011e01, 0xe07c1060,
    0x80012201, 0xe0781070, 0x80012801, 0xbf810000,
};

void Fill(std::uint32_t tid, std::uint32_t* words) {
    std::uint64_t seed = (tid + 1u) * 0x9e3779b97f4a7c15ull;
    for (std::uint32_t i = 0; i < Inputs; ++i) {
        seed = seed * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
        words[i] = static_cast<std::uint32_t>(seed >> 32u);
    }
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
    Require(actual == expected, "lds wrxchg2: lane " + std::to_string(tid) + " " + name + " is " + Hex(actual) + ", expected " + Hex(expected));
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
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 3080u, {false, false, false}, false, 1};
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
    constexpr std::array<const char*, 4> names{"ds_wrxchg2_rtn_b32", "ds_wrxchg2st64_rtn_b32", "ds_wrxchg2_rtn_b64", "ds_wrxchg2st64_rtn_b64"};
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t* in = &Input[tid * Inputs];
        const std::uint32_t* out = &Output[tid * Results];
        for (std::uint32_t op = 0; op < 2u; ++op) {
            for (std::uint32_t i = 0; i < 2u; ++i) {
                Expect(tid, out[op * 4u + i], in[i], std::string(names[op]) + " returned dword " + std::to_string(i));
                Expect(tid, out[op * 4u + 2u + i], in[4u + i], std::string(names[op]) + " stored dword " + std::to_string(i));
            }
        }
        for (std::uint32_t op = 2; op < 4u; ++op) {
            for (std::uint32_t i = 0; i < 4u; ++i) {
                Expect(tid, out[8u + (op - 2u) * 8u + i], in[i], std::string(names[op]) + " returned dword " + std::to_string(i));
                Expect(tid, out[8u + (op - 2u) * 8u + 4u + i], in[4u + i], std::string(names[op]) + " stored dword " + std::to_string(i));
            }
        }
        Expect(tid, out[24], in[0], "ds_wrxchg2_rtn_b32 with equal offsets first returned dword");
        Expect(tid, out[25], in[0], "ds_wrxchg2_rtn_b32 with equal offsets second returned dword");
        Expect(tid, out[26], in[5], "ds_wrxchg2_rtn_b32 with equal offsets stored dword");
    }
}

void CheckContention(std::uint32_t dword, std::uint32_t first) {
    const std::string name = "contended ds_wrxchg2_rtn_b32 dword " + std::to_string(dword);
    const std::uint32_t final = Output[30u + dword];
    Require(final >= first && final < first + Threads, "lds wrxchg2: " + name + " holds " + Hex(final) + ", which no lane wrote");
    std::vector<bool> seen(Threads, false);
    std::uint32_t initial = 0;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t* out = &Output[tid * Results];
        Expect(tid, out[30u + dword], final, name + " final value");
        const std::uint32_t old = out[28u + dword];
        if (old == 0u) {
            ++initial;
            continue;
        }
        Require(old >= first && old < first + Threads && old - first != tid && !seen[old - first], "lds wrxchg2: lane " + std::to_string(tid) + " " + name + " returned " + Hex(old));
        seen[old - first] = true;
    }
    Require(initial == 1u && !seen[final - first], "lds wrxchg2: " + name + " returned values do not form one serial order");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        CheckLanes();
        CheckContention(0u, 1u);
        CheckContention(1u, SecondOffset);
        std::puts("lds wrxchg2 tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
