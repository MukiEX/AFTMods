/*
    DivaVK - occlusion queries (GL_SAMPLES_PASSED / GL_ANY_SAMPLES_PASSED).

    GL lets a query begin outside any render pass; Vulkan queries must begin and end inside
    the same subpass. The Vulkan query is therefore started lazily at the first draw after
    glBeginQuery and ended at glEndQuery or when the pass ends, whichever comes first.
    Results become available once the frame's fence signals, matching how DivaGL polls
    GL_QUERY_RESULT_AVAILABLE a few frames later.
*/

#include "context.hpp"

namespace dvk {
    struct QueryRecord {
        GLuint name;
        uint32_t index;
    };
    static std::vector<QueryRecord> s_records[FRAMES_IN_FLIGHT];

    static bool s_vk_active = false;
    static uint32_t s_vk_index = 0;

    void Context::begin_query(GLenum target, GLuint id) {
        if (target != GL_SAMPLES_PASSED && target != GL_ANY_SAMPLES_PASSED
            && target != GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
            log_once(("qt" + std::to_string(target)).c_str(), "Unsupported query target 0x%04X", target);
            return;
        }
        QueryObj* q = queries.get_or_create(id);
        if (!q)
            return;
        q->target = target;
        q->issued = false;
        q->has_result = false;
        q->result = 0;
        q->pool = VK_NULL_HANDLE;
        active_queries_samples = id;
    }

    void Context::query_on_draw() {
        if (!active_queries_samples || s_vk_active || !pass_active)
            return;
        QueryObj* q = queries.get(active_queries_samples);
        if (!q || q->issued)
            return;
        Frame& f = frame();
        if (f.queries_used >= 1024) {
            log_once("query_pool_full", "Occlusion query pool exhausted for this frame");
            return;
        }
        s_vk_index = f.queries_used++;
        VkQueryControlFlags flags = (q->target == GL_SAMPLES_PASSED && features.occlusionQueryPrecise)
            ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
        vkCmdBeginQuery(f.cmd, f.query_pool, s_vk_index, flags);
        s_vk_active = true;
        q->issued = true;
        q->pool = f.query_pool;
        q->index = s_vk_index;
        q->serial = serial;
        s_records[frame_index].push_back({ q->name, s_vk_index });
    }

    void Context::query_on_pass_end() {
        if (!s_vk_active)
            return;
        vkCmdEndQuery(frame().cmd, frame().query_pool, s_vk_index);
        s_vk_active = false;
    }

    void Context::end_query(GLenum target) {
        (void)target;
        if (s_vk_active && pass_active)
            query_on_pass_end();
        QueryObj* q = queries.get(active_queries_samples);
        if (q && !q->issued) {
            // No draws were recorded: the result is simply zero.
            q->has_result = true;
            q->result = 0;
        }
        active_queries_samples = 0;
    }

    void query_collect_frame(Context& ctx, Frame& f) {
        uint32_t fi = (uint32_t)(&f - ctx.frames);
        for (const QueryRecord& r : s_records[fi]) {
            QueryObj* q = ctx.queries.get(r.name);
            if (!q || !q->issued || q->index != r.index || q->serial != f.serial)
                continue; // query restarted or deleted since
            uint64_t v = 0;
            vkGetQueryPoolResults(ctx.device, f.query_pool, r.index, 1, sizeof(v), &v, sizeof(v),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            q->result = q->target == GL_SAMPLES_PASSED ? v : (v ? 1 : 0);
            q->has_result = true;
        }
        s_records[fi].clear();
    }

    bool Context::query_result(QueryObj* q, bool wait, uint64_t& out) {
        if (!q)
            return false;
        if (!q->has_result && q->issued && wait) {
            // Find the frame that recorded it and wait for it
            for (Frame& f : frames)
                if (f.serial == q->serial) {
                    if (recording && &f == &frame())
                        submit_frame(false);
                    wait_frame(f);
                    break;
                }
        }
        if (!q->has_result)
            return false;
        out = q->result;
        return true;
    }
}
