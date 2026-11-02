/* Shared state of the NVN Vulkan execution layer (nvn_vk_*.c). */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
#include <vulkan/vulkan.h>
#include "nvk_vk_dispatch.h"   /* generated: Vulkan entry points as pointers */
#include "nvn_obj.h"

#define VKLOG(...) do { fprintf(stderr, "[android-nvn-vk] " __VA_ARGS__); fputc('\n', stderr); } while (0)
_Noreturn void nvk_die(const char *what);
#define VKCHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { VKLOG("%s -> VkResult %d", what, r_); nvk_die(what); } } while (0)

extern VkDevice nvk_dev;
extern VkPhysicalDevice nvk_phys;
extern VkQueue nvk_queue;
extern uint32_t nvk_family;

/* The queue is shared by the submit and present paths. */
void nvk_queue_lock(void);
void nvk_queue_unlock(void);
uint32_t nvk_memory_type(uint32_t allowed, VkMemoryPropertyFlags want);
/* Coarse full memory barrier (outside a rendering instance). */
void nvk_barrier(VkCommandBuffer cb);
/* Value of the submission now being recorded (or the next one). Objects used
 * by it may be destroyed once the completion thread reports that value. */
uint64_t nvk_pending_fence(void);
uint64_t nvk_completed_fence(void);
/* Changes with every command buffer begun (a submission may record several segments). */
uint64_t nvk_cmd_epoch(void);
void nvk_defer_buffer(VkBuffer buffer, VkDeviceMemory memory, uint64_t fence);
void nvk_defer_image(VkImage image, VkDeviceMemory memory, VkImageView *views, unsigned nviews, uint64_t fence);
/* The same, and release(ctx) once destroyed (memory handed back to a suballocator). */
void nvk_defer_image_release(VkImage image, VkDeviceMemory memory, VkImageView *views, unsigned nviews, uint64_t fence, void (*release)(void *), void *ctx);

/* NVN textures as Vulkan images (nvn_vk_image.c), keyed by texture serial.
 * Creation records the first upload of the pool contents into cb, which must
 * be recording outside a rendering instance. */
typedef struct NvkImage NvkImage;
NvkImage *nvk_image(const tex_t *t, VkCommandBuffer cb);
/* First use as a fully cleared colour attachment: old pool bytes are discarded. */
NvkImage *nvk_image_discard(const tex_t *t, VkCommandBuffer cb);
NvkImage *nvk_image_existing(uint64_t serial);
VkImage nvk_image_handle(const NvkImage *image);
VkFormat nvk_image_format(const NvkImage *image);
VkImageAspectFlags nvk_image_aspect(const NvkImage *image);
/* Extent of one mip level. */
VkExtent2D nvk_image_level_extent(const NvkImage *image, uint32_t level);
/* Single-level view for a render-target binding (NVN view or whole layer 0 range). */
VkImageView nvk_image_attachment_view(NvkImage *image, const view_t *view, uint32_t *level, uint32_t *layers);
void nvk_image_release(uint64_t serial);
void nvk_image_sweep(void);                          /* submission start: drop long-unused kept images */
void nvk_image_drop_range(uintptr_t lo, uintptr_t hi);   /* pool finalised: its kept images go */
void nvk_image_written(NvkImage *image);   /* the GPU now owns the contents (never re-uploaded from the pool) */
/* Re-uploads pool contents the CPU changed since the last upload (cb outside rendering). */
void nvk_image_refresh(NvkImage *image, const tex_t *t, VkCommandBuffer cb);
/* Sampling / storage view with the shader's view type. v: the texture pool view (may be NULL). */
VkImageView nvk_image_sample_view(NvkImage *image, const tex_t *t, const view_t *v, VkImageViewType type, int storage);
/* 1x1 zero image for unbound slots of the given view type. */
VkImageView nvk_dummy_view(VkImageViewType type, int storage, VkCommandBuffer cb);
int nvk_image_gpu_owned(const NvkImage *image);   /* rendered or stored to by the GPU */
void nvk_shader_debug_frame(uint64_t frame);   /* live shader bisection, TOTK_SHADER_DEBUG */
int nvk_white_view_ready(void);
VkImageView nvk_white_view(VkCommandBuffer cb);   /* debugging: 2D texture of ones */
void nvk_debug_dump_image(NvkImage *img, uint64_t serial);
void nvk_debug_dump_buffer(uint64_t addr, uint64_t size);
void nvk_scene_defer_write(const NvkImage *img, uint64_t serial);   /* a write to the scene target runs its deferred clear first (nvn_vk_backend.c) */
void nvk_scene_defer_read(const NvkImage *img, uint64_t serial);    /* a read of it first drops that clear (B5) */
int nvk_dump_frame_active(void);   /* the "dump" debug frame is being recorded (nvn_vk_draw.c) */
int nvk_bufwatch_hit(uint64_t addr, uint64_t n);   /* debugging: "bufwatch" is logging uses of a buffer containing addr */
void nvk_bufwatch_note(const char *what, uint64_t addr, uint64_t n);   /* debugging: GPU copy of a buffer range, written to a file */   /* debugging: write level 0 to a file */
int nvk_feedback_split_on(void);   /* sampling an open-pass attachment starts a new pass */
/* Attachment tracking for feedback splits (slots: bits 0-7 colour, bit 8 depth/stencil; nvn_vk_backend.c). */
uint16_t nvk_rt_slots(uint64_t serial, const NvkImage *img);
int nvk_feedback_split_needed(uint16_t slots, uint16_t draw_writes);
void nvk_pass_before_draw(uint16_t writes);
void nvk_pass_after_draw(uint16_t writes, uint16_t sampled);
/* Before a draw or clear: the attachment slots it writes and reads (attachment use prediction, nvn_vk_backend.c). */
void nvk_pass_access(uint16_t writes, uint16_t reads);
void nvk_pass_label(const char *ps_variant);
void nvk_pass_local_read_barrier(uint16_t attachments);   /* framebuffer fetch: make this pass's writes readable (bit 8: depth) */
VkImageView nvk_rt_depth_input_view(void);                /* depth attachment, depth aspect, as an input attachment */
uint64_t nvk_open_pass_serial(void);
/* nvn_vk_record.c: Vulkan recording on a second thread (TOTK_VK_RECORD_THREAD=1). */
void nvk_record_install(void);
void nvk_record_front(void);
void nvk_record_sync(void);
void nvk_record_stats(uint64_t v[4]);
void nvk_segment_checkpoint(void);
int nvk_record_segments_on(void);
int nvk_record_segment(VkQueue queue, VkCommandBuffer old, VkCommandBuffer next);
void nvk_record_overlap_stats(uint64_t v[3]);   /* queued handovers, replay sync ns, segment submit ns */
void nvk_record_template_bytes(size_t bytes);              /* size of the next template update's data block */
PFN_vkCmdSetVertexInputEXT nvk_record_vertex_input(PFN_vkCmdSetVertexInputEXT driver);
uint64_t nvk_buffer_write_epoch(void);                    /* changes with every GPU write into game buffers */
void nvk_buffer_write_bump(void);                      /* nonzero while a pass is open, unique per pass */
uint16_t nvk_pass_written_slots(void);                    /* attachment slots the open pass wrote (bit 8: depth) */
void nvk_split_report(uint64_t f0, uint64_t f, double nf); /* nvn_vk_draw.c: why passes split (every 300 frames) */
int nvk_fetch_depth_on(void);                             /* nvn_vk_image.c: TOTK_VK_FETCH_DEPTH=1 and local read */
VkImageView nvk_image_depth_input_view(NvkImage *image, const view_t *view);
VkImageView nvk_rt_color_view(int j);                    /* the view colour attachment j has in the pass */
void nvk_note_gpu_write(uint64_t addr, uint64_t size);   /* a storage write or copy of this submission */
int nvk_gpu_written_live(uint64_t addr, uint64_t size);  /* GPU work not completed yet writes into the range */   /* TOTK_VK_PASS_TIMING: names the open pass after its first fragment shader */
int nvk_ubo_diff_on(void);   /* TOTK_VK_UBO_DIFF: diagnostic, nvn_vk_backend.c */
void nvk_ubo_diff_note(uint64_t addr, uint64_t size, const uint8_t *copy);
void nvk_ubo_diff_skip(void);
void nvk_ubo_diff_stats(uint64_t v[5]);
int nvk_ucap_active(void);   /* the submission being replayed has uniforms captured at its submit call (TOTK_ASYNC_REPLAY) */
int nvk_ucap_cover(uint64_t addr, uint64_t size, uint64_t *serial, uint32_t *idx, const uint8_t **bytes, uint64_t *rlo, uint64_t *rlen);   /* the range is inside one captured range and unchanged since: its bytes and identity */
void nvk_ucap_stats(uint64_t *subs, uint64_t *bytes, uint64_t *ns);
void nvk_pass_storage_write(int image);   /* a draw of the open pass stores to memory (image: to an image) */
void nvk_image_cpu_written_rows(const tex_t *t, int was_gpu);   /* CPU wrote some rows: partial refresh unless the GPU owned it */
int nvk_image_gpu_owned_serial(uint64_t serial);
void nvk_image_reupload_all(void);   /* debugging: whole re-upload of every pool-filled image at its next use */
/* Live switches (nvn_vk_draw.c): "set NAME=VALUE" / "unset NAME" lines in the shader debug file override the
   environment for switches read through nvk_live_getenv; nvk_live_gen changes whenever the overrides do. */
extern unsigned nvk_live_gen;
const char *nvk_live_getenv(const char *name);
int nvk_pass_storage_written(void);      /* the open pass has stored to memory */
void nvk_pass_storage_range(uint64_t addr, uint64_t size);   /* a buffer range a draw of the open pass stores to */
int nvk_pass_storage_overlaps(uint64_t addr, uint64_t size);  /* the open pass stored to a buffer overlapping this range */
int nvk_ubo_unflushed(uint64_t addr, uint64_t size);   /* a uniform update inside the open pass covers this range */
void nvk_ubo_read(uint64_t addr, uint64_t size, uint8_t *out);   /* contents as the GPU will see them now */
uint64_t nvk_ubo_epoch(void);   /* a draw of the open pass writes storage: barriers must end it */
uint64_t nvk_ubo_unflushed_seq(uint64_t addr, uint64_t size);   /* newest unflushed update overlapping the range, 0: none */
int nvk_dummy_view_ready(VkImageViewType type, int storage);   /* created already: usable inside a pass */

/* Replay state shared by the command walker and the draw path (nvn_mtl.m S). */
typedef struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp; int depth_clamp; int render_disabled; int raster_discard;
    float blend_color[4];
} NvkState;
extern NvkState nvk_S;

/* Backend services for the draw path (nvn_vk_backend.c). */
VkCommandBuffer nvk_cmd(void);
int nvk_resolve(uint64_t addr, uint64_t len, VkBuffer *buffer, uint64_t *offset);
void nvk_rendering_begin(void);
/* Why a render pass ends: counted per cause in the periodic pass statistics. */
enum { NVK_END_RT, NVK_END_TRANSFER, NVK_END_TEXTURE, NVK_END_BUFFER, NVK_END_DISPATCH, NVK_END_FEEDBACK, NVK_END_COMMIT, NVK_END_PREDICT, NVK_END_N };
void nvk_rendering_end(int cause);
int nvk_rendering_attached(uint64_t serial);   /* serial is an attachment of the open render pass */
int nvk_rendering_attached_image(uint64_t serial, const NvkImage *img);   /* same, also through another serial of a shared image */
const p_set_rt *nvk_render_targets(void);   /* NULL before the first SET_RT */
VkExtent2D nvk_render_extent(void);
int nvk_scissor(p_scissor *out);
VkDescriptorPool nvk_descriptor_pool(void);
/* Texture and sampler descriptor pools (mtl_reg_tex / mtl_reg_samp). */
int nvk_registered_texture(int id, tex_t *t, view_t *v, int *has_view);
int nvk_registered_sampler(int id, samp_par *p);
VkSampler nvk_sampler_for_id(int id);
void nvk_sampler_id_stats(uint64_t *hits, uint64_t *misses);
VkSampler nvk_sampler(const samp_par *p);
uint32_t nvk_zero_buffer(VkBuffer *buffer);   /* returns its size */
VkBuffer nvk_support_buffer(void);

/* Shaders from the translation bridge (nvn_vk_shader.c). */
typedef struct {
    uint8_t set, kind, rw, dtype;
    int16_t reg, sreg, view;
    uint16_t off, binding, count;
} NvkBinding;
typedef struct NvkShader {
    char variant[80];
    int stage;                          /* 0 vertex, 1 fragment, 5 compute */
    VkShaderModule module;
    VkDescriptorSetLayout layout;
    NvkBinding *b; int nb;
    struct { uint8_t loc, cls; } in[32]; int nin;   /* inputs (attributes / varyings); cls: 0 float, 1 uint, 2 sint */
    uint32_t in_mask, out_locs;                     /* input and output locations */
    uint8_t out_mask;                               /* fragment colour outputs */
    uint8_t frag_depth;                             /* fragment: writes gl_FragDepth (BuiltIn FragDepth in its SPIR-V) */
    uint32_t local[3];
    VkBuffer c1; VkDeviceMemory c1_memory; uint32_t c1_size;   /* c1 is VK_NULL_HANDLE until a draw supplies the game's bytes */
    uint8_t shared;                     /* listed for reuse across unused bound state */
    /* Framebuffer fetch proofs (tools/fetch_proof.py, <variant>.fetch in the cache): */
    uint32_t fetch_locs;                /* vertex: outputs that are the screen position of the clip position */
    uint8_t nfetch; struct { uint32_t id; uint8_t slot, loc; } fetch[8];   /* fragment: samples of a texture at the fragment's own pixel */
    uint64_t dyn;                       /* bindings (by index) that are dynamic uniform buffers in the layout */
    uint8_t ndyn, dyn_order[16];        /* their indices by rising binding number: the order of the dynamic offsets */
} NvkShader;
extern int nvk_dyn_ubo_limit;           /* nvn_vk_shader.c: dynamic uniform buffers per pipeline layout (0: none) */
/* Variant of program stage i matching the bound state (translated on first use). */
NvkShader *nvk_shader_for(const prog_t *p, unsigned i, int *nvn_stage);
/* Fragment variant whose inputs at mask read zero (the vertex stage does not write them). */
NvkShader *nvk_shader_zero_inputs(NvkShader *ps, uint32_t mask);
/* Fragment variant whose proven own-pixel samples (ps->fetch[k]) read colour attachment att[k] as an input attachment
   (0xff: sampled as before); NULL when it cannot be built. Replaced texture bindings get kind 15 (bound to a dummy),
   input attachments kind 14 (reg = attachment index). */
NvkShader *nvk_shader_fetch_variant(NvkShader *ps, const uint8_t att[8]);
/* Fragment variant adding delta to every gl_FragDepth store (class depth off the 16-bit tie); NULL when not built. */
NvkShader *nvk_shader_depth_offset(NvkShader *ps, float delta);
/* The same with att[k] = 8 for the depth attachment (TOTK_VK_FETCH_DEPTH); zsw: per channel (4 bits each, r lowest)
   0 = depth, 4-6 = zero, 7 = one, the sampled view's component mapping. 0x7650 = what sampling depth returns. */
NvkShader *nvk_shader_fetch_variant_z(NvkShader *ps, const uint8_t att[8], uint16_t zsw);
/* Returns an existing shader variant; never translates or performs bridge I/O. */
NvkShader *nvk_shader_find_variant(const char *name);
/* Loads a cached variant (module and layout, no c1) ahead of its first draw; 0 if not cached. */
int nvk_shader_preload(const char *name, int stage);
/* Reads seed.idx (tools/android_shader_seed.py) once; later calls return at once. */
void nvk_shader_seed_load(void);
void nvk_pipeline_warm_signal(void);
uint64_t nvk_pool_remaining(uint64_t addr);
VkDescriptorSet nvk_alloc_set(VkDescriptorSetLayout layout);
/* 1 when sampling t needs commands first (creation or a stale upload). */
int nvk_image_needs_prepare(uint64_t serial, const tex_t *t);
int nvk_image_needs_prepare_existing(NvkImage *img, const tex_t *t);   /* img: nvk_image_existing(t->serial) */
uint32_t nvk_texture_info(int id);   /* 0x80000000 | format << 8 | target, or 0 */
/* Counter value a report still on the GPU will write to addr: 1 known (*val),
 * 0 pending with an unknown value (samples passed), -1 no pending report. */
int nvk_counter_lookup(uint64_t addr, uint64_t *val);
uint64_t nvk_draws_issued(void);
uint64_t nvk_state_skipped(void);   /* state blocks not re-emitted because the command buffer already held them */

VkPipelineCache nvk_pipeline_cache_begin(void);
void nvk_pipeline_cache_end(void);

void nvk_image_readback(const tex_t *t);
void *nvk_tiny_wb_record(VkCommandBuffer cmd, const tex_t *t, unsigned slot, size_t cap, unsigned slots);   /* TOTK_VK_TINY_WRITEBACK */
int nvk_tiny_wb_finish(void *state, uint64_t stamp);
void nvk_alias_writeback(const tex_t *t);   /* before a new texture: GPU-written images overlapping it reach memory */
int nvk_submit_wait(void);
/* Cumulative texture-copy counts/bytes, and replay-thread CPU time since the
 * previous read. GPU transfer time is not measured by this counter. */
void nvk_upload_stats(uint64_t *first, uint64_t *reuploads, uint64_t *bytes);
uint64_t nvk_upload_cpu_ns_take(void);

static inline uint64_t nvk_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec; }
extern _Atomic uint64_t nvk_shader_count, nvk_shader_ns, nvk_pipeline_count, nvk_pipeline_ns;
extern _Atomic uint64_t nvk_upload_ns, nvk_upload_n, nvk_upload_bytes;   /* nvn_vk_image.c: texture uploads */
void nvk_image_cpu_written(const tex_t *t);
