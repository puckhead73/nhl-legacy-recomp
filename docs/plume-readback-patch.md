# Plume RHI patch — framebuffer readback (C-5g)

`third_party/plume/` is **gitignored** (vendored, built from source), so these two changes to
`third_party/plume/plume_vulkan.cpp` are NOT captured by commits. They are required for the
`NHL_HIGHCUT_C5_SHOT` framebuffer→PNG readback (in `gpu/hooks/plume_present.cpp`) to work — without
them the readback dereferences a null texture / requests an unsupported swapchain usage and silently
crashes the plume thread. Re-apply after any plume re-vendor/update.

## 1. Add the image→buffer copy branch to `VulkanCommandList::copyTextureRegion`

Plume only implemented buffer→image (upload) and image→image; an image→buffer (readback) copy fell
into the image→image branch and dereferenced a null `dstTexture`. Add this `else if` BEFORE the final
`else` (the image→image branch), right after the existing `buffer→image` branch's closing `}`:

```cpp
else if ((dstLocation.type == RenderTextureCopyType::PLACED_FOOTPRINT) && (srcLocation.type == RenderTextureCopyType::SUBRESOURCE)) {
    // NHL high-cut: image -> buffer readback (framebuffer->PNG debug dump). Symmetric to the
    // buffer->image upload branch above; without this an image->buffer copy fell into the
    // image->image branch below and dereferenced a null dstTexture.
    assert(srcTexture != nullptr);
    assert(dstBuffer != nullptr);

    const uint32_t blockWidth = RenderFormatBlockWidth(srcTexture->desc.format);
    VkBufferImageCopy imageCopy = {};
    imageCopy.bufferOffset = dstLocation.placedFootprint.offset;
    imageCopy.bufferRowLength = ((dstLocation.placedFootprint.rowWidth + blockWidth - 1) / blockWidth) * blockWidth;
    imageCopy.bufferImageHeight = ((dstLocation.placedFootprint.height + blockWidth - 1) / blockWidth) * blockWidth;
    imageCopy.imageSubresource.aspectMask = toAspectFlags(srcTexture->desc.format, srcTexture->desc.flags);
    imageCopy.imageSubresource.baseArrayLayer = srcLocation.subresource.arrayIndex;
    imageCopy.imageSubresource.layerCount = 1;
    imageCopy.imageSubresource.mipLevel = srcLocation.subresource.mipLevel;
    imageCopy.imageOffset.x = 0;
    imageCopy.imageOffset.y = 0;
    imageCopy.imageOffset.z = 0;
    imageCopy.imageExtent.width = dstLocation.placedFootprint.width;
    imageCopy.imageExtent.height = dstLocation.placedFootprint.height;
    imageCopy.imageExtent.depth = dstLocation.placedFootprint.depth;
    vkCmdCopyImageToBuffer(vk, srcTexture->vk, toImageLayout(srcTexture->textureLayout), dstBuffer->vk, 1, &imageCopy);
}
```

## 2. Allow reading back the swapchain image (add `TRANSFER_SRC` usage)

In `VulkanSwapChain::resize()`, the swapchain images are created without `TRANSFER_SRC`, so copying
*from* the presented image is invalid. Right after the `createInfo.imageUsage = ...` line (which sets
`COLOR_ATTACHMENT | TRANSFER_DST | SAMPLED`), add:

```cpp
// NHL high-cut: allow reading the presented image back (framebuffer->PNG debug dump). Only add
// TRANSFER_SRC if the surface advertises it (it virtually always does) so we never request an
// unsupported usage.
if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
    createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}
```

(`surfaceCapabilities` is already queried just above in `resize()`.)
