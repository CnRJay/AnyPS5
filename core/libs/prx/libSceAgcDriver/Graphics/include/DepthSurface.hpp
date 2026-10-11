#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHSURFACE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace AgcDriver::Graphics {

class Texture;
class StorageTexture;

VkImageView DepthSurfaceView(const Context& context, const DepthTarget& target);
std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel);
void ClearDepthSurfaces(VkDevice device);
bool DepthSurfaceAt(std::uint64_t address);
std::uint64_t HtileDepthClearAddress(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const std::array<std::uint32_t, 3>& numThreads);
void NoteHtileDepthClear(std::uint64_t htileAddress);
bool DepthSurfaceHolds(const Context& context, const GuestTextureResource& resource, const Texture* texture);
std::shared_ptr<Texture> DepthSurfaceTexture(const Context& context, std::span<const std::uint32_t> words, const GuestTextureResource& resource, VkComponentMapping components);
// A shader binds the depth or stencil plane of a depth surface as storage image `image` (mip
// `mip`): what it writes there is copied into that plane of the depth image before the surface is
// next attached or sampled (the depth image is the surface's only copy; guest memory never receives
// it). The storage image starts from guest memory, not from the depth image, so a shader reading
// the plane through it sees the guest's bytes. Throws for an image that does not cover the plane in
// its texel format (R32F or R16 depth, R8 stencil).
void NoteDepthStorage(const Context& context, const std::shared_ptr<StorageTexture>& image, std::uint32_t mip);

}

#endif
