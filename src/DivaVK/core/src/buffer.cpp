/*
    DivaVK - buffer objects.

    Two storage strategies:
    - Dynamic buffers (anything created with GL_DYNAMIC_STORAGE_BIT / GL_MAP_WRITE_BIT, or
      glBufferData with a non-STATIC usage, up to 16 MB) keep a CPU shadow copy. The GPU sees
      per-frame "versions" carved from the frame's upload arena. A write after the current
      version has been referenced by a recorded command creates a new version, which gives
      GL's ordering semantics (each draw sees the data current at the time it was issued)
      without stalling or breaking render passes.
    - Static buffers live in device-local memory and are updated with staged copies. Copies
      to a buffer that hasn't been used yet this frame are hoisted into the upload command
      buffer; otherwise they are recorded inline (which ends the current render pass).
*/

#include "context.hpp"

namespace dvk {
    static const VkBufferUsageFlags BUFFER_USAGE = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
        | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
        | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    static const VkDeviceSize MAX_DYNAMIC_SIZE = 16ull << 20;

    BufferObj* Context::buffer_for_target(GLenum target) {
        GLuint name = 0;
        switch (target) {
        case GL_ARRAY_BUFFER: name = array_buffer; break;
        case GL_ELEMENT_ARRAY_BUFFER: {
            VertexArrayObj* vao = vertex_array ? vertex_arrays.get(vertex_array) : &default_vao;
            name = vao ? vao->element_buffer : 0;
        } break;
        case GL_UNIFORM_BUFFER: name = generic_uniform_buffer; break;
        case GL_SHADER_STORAGE_BUFFER: name = generic_storage_buffer; break;
        case GL_COPY_READ_BUFFER: name = copy_read_buffer; break;
        case GL_COPY_WRITE_BUFFER: name = copy_write_buffer; break;
        case GL_PIXEL_PACK_BUFFER: name = pixel_pack_buffer; break;
        case GL_PIXEL_UNPACK_BUFFER: name = pixel_unpack_buffer; break;
        default:
            log_once("buffer_target", "Unsupported buffer target 0x%04X", target);
            return nullptr;
        }
        return buffers.get(name);
    }

    void Context::buffer_destroy(BufferObj* b) {
        if (b->buffer) {
            VkBuffer buf = b->buffer;
            VmaAllocation alloc = b->alloc;
            VmaAllocator a = vma;
            defer_destroy([a, buf, alloc]() { vmaDestroyBuffer(a, buf, alloc); });
            b->buffer = VK_NULL_HANDLE;
            b->alloc = VK_NULL_HANDLE;
        }
        b->shadow.clear();
        b->shadow.shrink_to_fit();
        b->ver_serial = 0;
        b->created = false;
        b->size = 0;
    }

    void Context::buffer_storage(BufferObj* b, GLsizeiptr size, const void* data, bool dynamic) {
        buffer_destroy(b);
        b->size = (VkDeviceSize)std::max<GLsizeiptr>(size, 0);
        b->created = true;
        b->dynamic = dynamic && b->size <= MAX_DYNAMIC_SIZE;
        if (!b->size)
            return;

        if (b->dynamic) {
            b->shadow.resize((size_t)b->size);
            if (data)
                memcpy(b->shadow.data(), data, (size_t)b->size);
            else
                memset(b->shadow.data(), 0, (size_t)b->size);
            b->ver_serial = 0;
            return;
        }

        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = b->size;
        bci.usage = BUFFER_USAGE;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        DVK_CHECK(vmaCreateBuffer(vma, &bci, &aci, &b->buffer, &b->alloc, nullptr));
        b->last_used_serial = 0;
        if (data)
            buffer_write(b, 0, size, data);
    }

    void Context::buffer_write(BufferObj* b, GLintptr offset, GLsizeiptr size, const void* data) {
        if (!b || !b->created || size <= 0 || !data)
            return;
        if (offset < 0 || (VkDeviceSize)(offset + size) > b->size) {
            set_error(GL_INVALID_VALUE);
            return;
        }

        if (b->dynamic) {
            // Versions are created at first use and immediately referenced, so any write
            // invalidates the current version; the next use snapshots the new contents.
            memcpy(b->shadow.data() + offset, data, (size_t)size);
            b->ver_serial = 0;
            return;
        }

        // Static: staged copy
        begin_frame_if_needed();
        UploadArena::Alloc src = frame().arena.alloc(*this, (VkDeviceSize)size, 16);
        memcpy(src.ptr, data, (size_t)size);

        VkBufferCopy region = { src.offset, (VkDeviceSize)offset, (VkDeviceSize)size };
        if (b->last_used_serial != serial) {
            vkCmdCopyBuffer(upload_cmd(), src.buffer, b->buffer, 1, &region);
            stats.hoisted_uploads++;
        }
        else {
            if (pass_active)
                stats.pass_breaks++;
            end_pass();
            VkCommandBuffer cb = cmd();
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdCopyBuffer(cb, src.buffer, b->buffer, 1, &region);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            stats.inline_uploads++;
        }
    }

    bool Context::buffer_resolve(BufferObj* b, VkBuffer& out_buffer, VkDeviceSize& out_offset) {
        if (!b || !b->created || !b->size)
            return false;

        if (!b->dynamic) {
            b->last_used_serial = serial;
            out_buffer = b->buffer;
            out_offset = 0;
            return true;
        }

        if (b->ver_serial != serial) {
            VkDeviceSize align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment,
                props.limits.minStorageBufferOffsetAlignment);
            UploadArena::Alloc a = frame().arena.alloc(*this, b->size, std::max<VkDeviceSize>(align, 16));
            memcpy(a.ptr, b->shadow.data(), (size_t)b->size);
            b->ver_buffer = a.buffer;
            b->ver_offset = a.offset;
            b->ver_ptr = a.ptr;
            b->ver_serial = serial;
        }
        out_buffer = b->ver_buffer;
        out_offset = b->ver_offset;
        return true;
    }

    void* Context::buffer_map(BufferObj* b, GLintptr offset, GLsizeiptr length, GLbitfield access) {
        if (!b || !b->created || b->map_ptr) {
            set_error(GL_INVALID_OPERATION);
            return nullptr;
        }
        if (length <= 0 || offset < 0 || (VkDeviceSize)(offset + length) > b->size) {
            set_error(GL_INVALID_VALUE);
            return nullptr;
        }

        b->map_access = access;
        b->map_offset = offset;
        b->map_length = length;
        if (b->dynamic) {
            b->map_ptr = b->shadow.data() + offset;
            return b->map_ptr;
        }

        b->map_temp.resize((size_t)length);
        if (access & GL_MAP_READ_BIT)
            buffer_read(b, offset, length, b->map_temp.data());
        b->map_ptr = b->map_temp.data();
        return b->map_ptr;
    }

    bool Context::buffer_unmap(BufferObj* b) {
        if (!b || !b->map_ptr) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        if (b->map_access & GL_MAP_WRITE_BIT) {
            if (b->dynamic)
                b->ver_serial = 0;
            else {
                uint8_t* p = b->map_ptr;
                b->map_ptr = nullptr;
                buffer_write(b, b->map_offset, b->map_length, p);
            }
        }
        b->map_ptr = nullptr;
        b->map_temp.clear();
        return true;
    }

    void Context::buffer_read(BufferObj* b, GLintptr offset, GLsizeiptr size, void* out) {
        if (b->dynamic) {
            memcpy(out, b->shadow.data() + offset, (size_t)size);
            return;
        }

        // Synchronous GPU readback
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        aci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // nothing here flushes or invalidates by hand
        VkBuffer rb;
        VmaAllocation ra;
        VmaAllocationInfo ai = {};
        DVK_CHECK(vmaCreateBuffer(vma, &bci, &aci, &rb, &ra, &ai));

        end_pass();
        VkBufferCopy region = { (VkDeviceSize)offset, 0, (VkDeviceSize)size };
        vkCmdCopyBuffer(cmd(), b->buffer, rb, 1, &region);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
        Frame& f = frame();
        submit_frame(false);
        wait_frame(f);
        vmaInvalidateAllocation(vma, ra, 0, VK_WHOLE_SIZE);
        memcpy(out, ai.pMappedData, (size_t)size);
        vmaDestroyBuffer(vma, rb, ra);
    }
}
