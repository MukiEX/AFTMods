/*
    DivaVK - OpenXR output, internal interface (see xr.cpp and dvk_xr_* in divavk.h).
*/

#pragma once

#include "common.hpp"

namespace dvk {
    struct Context;

    // True once dvk_xr_setup found a headset: Vulkan must then be created through the runtime.
    bool xr_enabled();
    bool xr_create_vk_instance(const VkInstanceCreateInfo& ci, VkInstance& out);
    VkPhysicalDevice xr_physical_device(VkInstance instance);
    bool xr_create_vk_device(VkPhysicalDevice phys, const VkDeviceCreateInfo& ci, VkDevice& out);

    // After the context is created: session, spaces and the frame thread.
    void xr_start(Context& ctx);
    // Game thread, while recording the frame being presented: copies the default framebuffer
    // into a staging image for the frame thread.
    void xr_record_frame(Context& ctx, VkCommandBuffer cb);
    // Game thread, right after that frame's vkQueueSubmit: hands the copy to the frame thread.
    void xr_frame_submitted();
    // Before the context's Vulkan objects go away.
    void xr_shutdown(Context& ctx);

    // Device.cpp's log target, also used before dvk_create.
    void set_log_target(dvk_log_func fn, void* user);
}
