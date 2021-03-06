// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image_representation_dawn_ozone.h"

#include <dawn/native/VulkanBackend.h>

#include <vulkan/vulkan.h>
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image_representation.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/native_pixmap.h"

namespace gpu {

SharedImageRepresentationDawnOzone::SharedImageRepresentationDawnOzone(
    SharedImageManager* manager,
    SharedImageBacking* backing,
    MemoryTypeTracker* tracker,
    WGPUDevice device,
    WGPUTextureFormat format,
    scoped_refptr<gfx::NativePixmap> pixmap,
    scoped_refptr<base::RefCountedData<DawnProcTable>> dawn_procs)
    : SharedImageRepresentationDawn(manager, backing, tracker),
      device_(device),
      format_(format),
      pixmap_(pixmap),
      dawn_procs_(dawn_procs) {
  DCHECK(device_);

  // Keep a reference to the device so that it stays valid (it might become
  // lost in which case operations will be noops).
  dawn_procs_->data.deviceReference(device_);
}

SharedImageRepresentationDawnOzone::~SharedImageRepresentationDawnOzone() {
  EndAccess();
  dawn_procs_->data.deviceRelease(device_);
}

WGPUTexture SharedImageRepresentationDawnOzone::BeginAccess(
    WGPUTextureUsage usage) {
  // It doesn't make sense to have two overlapping BeginAccess calls on the same
  // representation.
  if (texture_) {
    return nullptr;
  }

  // For multi-planar formats, Mesa is yet to support to allocate and bind
  // vkmemory for each plane respectively.
  // https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/intel/vulkan/anv_formats.c#L765
  // For now we assume all plane handles are same, and we don't use the
  // VK_IMAGE_CREATE_DISJOINT_BIT when creating the vkimage for the pixmap.
  DCHECK(pixmap_->SupportsZeroCopyWebGPUImport() ||
         pixmap_->GetNumberOfPlanes() == 1)
      << "Disjoint Multi-plane importing is not supported.";

  if (!ozone_backing()->VaSync()) {
    return nullptr;
  }

  std::vector<gfx::GpuFenceHandle> fences;
  bool need_end_fence;
  if (!ozone_backing()->BeginAccess(
          /*readonly=*/false, SharedImageBackingOzone::AccessStream::kWebGPU,
          &fences, need_end_fence)) {
    return nullptr;
  }
  DCHECK(need_end_fence);

  gfx::Size pixmap_size = pixmap_->GetBufferSize();
  WGPUTextureDescriptor texture_descriptor = {};
  texture_descriptor.format = format_;
  texture_descriptor.usage = usage;
  texture_descriptor.dimension = WGPUTextureDimension_2D;
  texture_descriptor.size = {static_cast<uint32_t>(pixmap_size.width()),
                             static_cast<uint32_t>(pixmap_size.height()), 1};
  texture_descriptor.mipLevelCount = 1;
  texture_descriptor.sampleCount = 1;

  // We need to have internal usages of CopySrc for copies and
  // RenderAttachment for clears.
  WGPUDawnTextureInternalUsageDescriptor internalDesc = {};
  internalDesc.chain.sType = WGPUSType_DawnTextureInternalUsageDescriptor;
  internalDesc.internalUsage =
      WGPUTextureUsage_CopySrc | WGPUTextureUsage_RenderAttachment;
  texture_descriptor.nextInChain =
      reinterpret_cast<WGPUChainedStruct*>(&internalDesc);

  dawn::native::vulkan::ExternalImageDescriptorDmaBuf descriptor = {};
  descriptor.cTextureDescriptor = &texture_descriptor;
  descriptor.isInitialized = IsCleared();

  // Import the dma-buf into Dawn via the Vulkan backend. As per the Vulkan
  // documentation, importing memory from a file descriptor transfers
  // ownership of the fd from the application to the Vulkan implementation.
  // Thus, we need to dup the fd so the fd corresponding to the dmabuf isn't
  // closed twice (once by ScopedFD and once by the Vulkan implementation).
  int fd = dup(pixmap_->GetDmaBufFd(0));
  descriptor.memoryFD = fd;
  // stride is not required for multi-planar formats.
  descriptor.stride = pixmap_->GetDmaBufPitch(0);
  descriptor.drmModifier = pixmap_->GetBufferFormatModifier();
  descriptor.waitFDs = {};

  for (auto& fence : fences) {
    descriptor.waitFDs.push_back(fence.owned_fd.release());
  }

  texture_ = dawn::native::vulkan::WrapVulkanImage(device_, &descriptor);
  if (!texture_) {
    ozone_backing()->EndAccess(/*readonly=*/false,
                               SharedImageBackingOzone::AccessStream::kWebGPU,
                               gfx::GpuFenceHandle());
    close(fd);
  }

  return texture_;
}

void SharedImageRepresentationDawnOzone::EndAccess() {
  if (!texture_) {
    return;
  }

  // Grab the signal semaphore from dawn
  dawn::native::vulkan::ExternalImageExportInfoOpaqueFD export_info;
  if (!dawn::native::vulkan::ExportVulkanImage(
          texture_, VK_IMAGE_LAYOUT_UNDEFINED, &export_info)) {
    DLOG(ERROR) << "Failed to export Dawn Vulkan image.";
  } else {
    if (export_info.isInitialized) {
      SetCleared();
    }

    // TODO(hob): Handle waiting on multiple semaphores from dawn.
    DCHECK(export_info.semaphoreHandles.size() == 1);
    gfx::GpuFenceHandle fence;
    fence.owned_fd = base::ScopedFD(export_info.semaphoreHandles[0]);
    ozone_backing()->EndAccess(/*readonly=*/false,
                               SharedImageBackingOzone::AccessStream::kWebGPU,
                               std::move(fence));
  }
  dawn_procs_->data.textureDestroy(texture_);
  dawn_procs_->data.textureRelease(texture_);
  texture_ = nullptr;
}

}  // namespace gpu
