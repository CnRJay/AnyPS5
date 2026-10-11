#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Slices = 4;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::size_t SliceBytes = 65536;
constexpr std::size_t BlockBytes = SliceBytes * Slices;
constexpr std::uint32_t Initial = 0x5a5a5a5au;

// image_store of s12 at (v0, 0, s13) of a 2D array shape, and at (v0, 0) of a 2D shape.
alignas(256) constexpr std::array<std::uint32_t, 6> StoreArrayCode{
    0x7e020280, 0x7e04020d, 0x7e06020c, 0xf0201128, 0x00010300, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 5> StoreFlatCode{
    0x7e020280, 0x7e04020c, 0xf0201108, 0x00010200, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
        }
#endif
        Require(block != nullptr, "slice views: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* texels, std::uint32_t first, std::uint32_t last) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2DArray << 28u),
        last | (first << 16u),
        0u,
        0u,
        0u,
    };
}

// Stores through a descriptor of slices `first` through `last`, at its slice `slice` (a flat shape stores
// to its first slice).
void Store(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::uint32_t first, std::uint32_t last, bool flat, std::uint32_t slice, std::uint32_t value) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, first, last);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = value;
    userData[13] = slice;
    const std::span<const std::uint32_t> code = flat ? std::span<const std::uint32_t>(StoreFlatCode) : std::span<const std::uint32_t>(StoreArrayCode);
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

void RequireSlice(const std::uint8_t* texels, std::uint32_t slice, std::uint32_t value, const std::string& step) {
    const auto* words = reinterpret_cast<const std::uint32_t*>(texels + slice * SliceBytes);
    const auto stored = std::count(words, words + SliceBytes / 4u, value);
    const auto initial = std::count(words, words + SliceBytes / 4u, Initial);
    Require(stored == static_cast<std::ptrdiff_t>(Threads) && initial == static_cast<std::ptrdiff_t>(SliceBytes / 4u - Threads), step + ": slice " + std::to_string(slice) + " holds " + std::to_string(stored) + " words of " + Hex(value) + " and " + std::to_string(initial) + " untouched ones, expected " + std::to_string(Threads) + " and " + std::to_string(SliceBytes / 4u - Threads));
}

void Run(AgcDriver::VulkanDevice& device, std::uint8_t* texels) {
    const auto surface = AddressOf(texels);
    const auto bytes = [](std::uint32_t slices) { return std::uint64_t{SliceBytes} * slices; };
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), BlockBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, BlockBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Store(device, texels, 1, 1, false, 0, 0x11111111u);
    Require(StorageTexture::FindLive(surface, bytes(2)) != nullptr, "the store through slice 1 did not make an image of the slices up to it");
    Store(device, texels, 2, 2, true, 0, 0x55555555u);
    Require(StorageTexture::FindLive(surface, bytes(3)) != nullptr && StorageTexture::FindLive(surface, bytes(2)) == nullptr, "the flat store through slice 2 did not replace the image of slices 0 and 1");
    Store(device, texels, 0, 3, false, 0, 0x33333333u);
    const auto large = StorageTexture::FindLive(surface, BlockBytes);
    Require(large != nullptr && StorageTexture::FindLive(surface, bytes(3)) == nullptr, "the store through every slice did not replace the smaller image");
    Store(device, texels, 3, 3, false, 0, 0x66666666u);
    StorageTexture::FlushPending(surface, BlockBytes, nullptr, "test");
    device.WaitIdle();
    RequireSlice(texels, 3, 0x66666666u, "the store through slice 3 alone");
    Store(device, texels, 2, 3, false, 1, 0x22222222u);
    Store(device, texels, 1, 1, false, 0, 0x11111111u);
    Require(StorageTexture::FindLive(surface, BlockBytes) == large && StorageTexture::FindLive(surface, bytes(3)) == nullptr && StorageTexture::FindLive(surface, bytes(2)) == nullptr, "a store through fewer slices after the largest view did not use its image");
    StorageTexture::FlushPending(surface, BlockBytes, nullptr, "test");
    device.WaitIdle();
    const std::string steps = "stores through slices 1, 2, 0 through 3, 3 and 2 through 3";
    RequireSlice(texels, 0, 0x33333333u, steps);
    RequireSlice(texels, 1, 0x11111111u, steps);
    RequireSlice(texels, 2, 0x55555555u, steps);
    RequireSlice(texels, 3, 0x22222222u, steps);    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}
}

int main() {
    try {
        GuestBlock block;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device, block.Data());
        std::puts("storage slice views tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}