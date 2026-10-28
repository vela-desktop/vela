// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef VELA_RENDER_RENDER_H
#define VELA_RENDER_RENDER_H

// The renderer's insides: the structs and functions that vulkan.c, renderer.c,
// texture.c and pass.c share. The rest of the compositor uses only
// render/renderer.h.

#include "render/renderer.h"

#include <vulkan/vulkan.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/interface.h>
#include <wlr/render/dmabuf.h>
#include <wlr/util/addon.h>

// --------------------------------------------------------------- formats --

// The pixel formats the renderer knows: the DRM code (that of app and output
// buffers) and the matching Vulkan format.
struct vela_pixel_format {
    uint32_t drm;
    // "Raw" view: shaders read the values as they are (sRGB-encoded and
    // premultiplied) and take them to linear space themselves (§7.5).
    VkFormat unorm;
    // View with sRGB conversion on write, to draw on: the GPU blends in linear
    // and encodes. VK_FORMAT_UNDEFINED if there is none.
    VkFormat srgb;
    bool alpha; // X... formats ignore the fourth channel
    uint32_t bytes_per_pixel;
};

extern const struct vela_pixel_format vela_pixel_formats[];
extern const int vela_pixel_format_count;
const struct vela_pixel_format *vela_pixel_format_from_drm(uint32_t drm);

// ---------------------------------------------------------------- device --

#define VELA_MAX_SHM_FORMATS 16

struct vela_vulkan {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    char name[256];

    int render_fd; // the render node of the same GPU, ours
    // (format, modifier) importable as dmabuf and usable as a render target.
    struct wlr_drm_format_set render_formats;
    // (format, modifier) of the dmabufs we can read as textures: those we
    // offer apps (linux-dmabuf).
    struct wlr_drm_format_set texture_formats;
    // Formats of shared-memory (wl_shm) buffers we can upload.
    uint32_t shm_formats[VELA_MAX_SHM_FORMATS];
    int shm_format_count;
    // The GPU can export and import semaphores as sync_files (implicit sync
    // with the kernel, §7.3).
    bool sync_file;

    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;

    // GPU timestamps (§4.3: what a frame costs): ns per tick; 0 if the queue
    // can't write them. With calibrated timestamps we also know *when* the GPU
    // finished, on CLOCK_MONOTONIC.
    float timestamp_period;
    uint64_t timestamp_mask;
    PFN_vkGetCalibratedTimestampsKHR get_calibrated_timestamps;

    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDeviceDrmPropertiesEXT drm;
};

uint32_t vela_vulkan_memory_type(const struct vela_vulkan *vk, uint32_t type_bits, VkMemoryPropertyFlags flags);
// A dmabuf of that format and modifier can be imported for that use.
bool vela_vulkan_supports_dmabuf(const struct vela_vulkan *vk, VkFormat format, uint64_t modifier,
    VkImageUsageFlags usage);
// A dmabuf as a VkImage, without copies (all planes in the same dmabuf).
bool vela_vulkan_import_dmabuf(const struct vela_vulkan *vk, const struct wlr_dmabuf_attributes *dmabuf,
    VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory);
VkShaderModule vela_vulkan_shader(const struct vela_vulkan *vk, const uint32_t *code, size_t bytes);

// --------------------------------------------------------------- drawing --

// The constants of every draw (see shaders/push.glsl): 160 bytes, below the
// 256 Vulkan 1.4 guarantees.
struct vela_quad_push {
    float dst[4]; // x, y, width, height in target pixels
    float target[2]; // target size
    float alpha;
    // bit 0: bicubic magnification; bit 1: rounded clip; bit 2: color filter;
    // bit 3: the blur gets Vela's acrylic recipe (blur_mix.frag)
    uint32_t flags;
    float uv_origin[2]; // texture coordinates of the top left corner
    float uv_x[2]; // step along the top edge
    float uv_y[2]; // step along the left edge
    float pad[2];
    float color[4]; // rectangles and shadows: premultiplied linear color
    float shape_rect[4]; // rounded rectangle (the clip, or what casts the shadow)
    float shape[4]; // corner radius, shadow sigma
    // The output's color filter (night light, color filters): a 3x3 matrix in
    // linear space, one row per vec4.
    float filter[12];
};
_Static_assert(sizeof(struct vela_quad_push) == 160, "push constants must match push.glsl");

enum vela_pipeline_kind {
    VELA_PIPELINE_TEXTURE,
    VELA_PIPELINE_RECT,
    VELA_PIPELINE_SHADOW,
    VELA_PIPELINE_BLUR_DOWN,
    VELA_PIPELINE_BLUR_UP,
    VELA_PIPELINE_BLUR_MIX,
};

// The blur's work images: a chain of levels, each at half the resolution of
// the previous one (the first at half the output's), in 16-bit floating point:
// linear, no banding. They grow when needed and are reused from one drawing to
// the next.
#define VELA_BLUR_LEVELS 4
#define VELA_BLUR_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

struct vela_blur_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t width;
    uint32_t height;
};

// A dmabuf buffer imported as a render target (output, cursor, capture). Lives
// as long as the buffer (wlr_addon) and is in renderer->targets.
struct vela_target {
    struct vela_renderer *renderer;
    struct wlr_addon addon;
    struct wl_list link;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format; // of the view: _SRGB, the GPU encodes on write
    int dmabuf_fd; // plane 0, for implicit sync (not ours)
    uint32_t width;
    uint32_t height;
    bool initialized; // the content must be kept from one drawing to the next
    bool sampleable; // can also be read as a texture (the blur reads what is behind)
};

// A texture: a buffer's content ready to be read by shaders.
// - dmabuf: imported as it is, without copies; one import per buffer
//   (wlr_addon), as long as wlroots uses the texture.
// - wl_shm and the like: copied into an image of ours; when the app updates,
//   only the changed part is copied again.
// wlr_texture is the first member: from a wlr_texture* we get back here.
struct vela_texture {
    struct wlr_texture base;
    struct vela_renderer *renderer; // NULL: the renderer is already closed
    struct wl_list link; // renderer->textures
    const struct vela_pixel_format *format;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;

    // Imported dmabuf: the buffer it's tied to (locked as long as the texture
    // exists) and its plane 0 descriptor, for implicit sync.
    struct wlr_buffer *buffer;
    struct wlr_addon addon;
    int dmabuf_fd;
    bool foreign; // on every use: acquired from the "foreign" queue

    bool uploaded; // our own image: the content is there
    int refs; // wlroots' references (texture_from_buffer / destroy)
};

struct vela_texture *vela_texture_from_wlr_texture(struct wlr_texture *texture);
struct wlr_texture *vela_texture_create(struct vela_renderer *renderer, struct wlr_buffer *buffer);
// When the renderer closes: frees the Vulkan resources at once.
void vela_texture_release(struct vela_texture *texture);

// A CPU-visible block of memory for uploads and readbacks.
struct vela_staging {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t *data;
};

// -------------------------------------------------------- renderer state --

#define VELA_TIMING_SLOTS 64
#define VELA_READBACK_MAX_JOBS 4

// A command buffer of the pool: free when it isn't recording and the GPU has
// finished the submission that contained it (point).
struct command_slot {
    VkCommandBuffer cmd;
    uint64_t point; // 0: recording or free
    bool busy;
};

// A retired image (texture, target, blur level): destroyed when the timeline
// reaches `point`.
struct retired_image {
    uint64_t point;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
};

// A semaphore used by a submission: free again when the timeline reaches
// `point`. release: exportable as a sync_file (end of work).
struct used_semaphore {
    uint64_t point;
    VkSemaphore semaphore;
    bool release;
};

struct staging_chunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *data;
    VkDeviceSize size;
    VkDeviceSize used;
    uint64_t point; // last submission reading it; UINT64_MAX: in use by commands not submitted yet
    unsigned readers; // pending CPU readbacks: GPU completion alone cannot recycle the slice
    bool readback; // memory the CPU reads quickly (cached), for copies from the GPU
};

struct pipeline_entry {
    VkFormat format;
    enum vela_pipeline_kind kind;
    bool blend;
    VkPipeline pipeline;
};

struct vela_renderer {
    struct wlr_renderer base; // first member: wlr_renderer* <-> vela_renderer*
    struct vela_vulkan *vk; // not ours: the server destroys it after us

    VkCommandPool pool;
    struct command_slot *commands;
    int command_count, command_capacity;
    VkCommandBuffer upload; // uploads waiting for the next submission

    VkSemaphore timeline;
    uint64_t last_point;
    uint64_t completed;
    uint64_t readback_point; // later draws must follow submitted readback reads, even after cancellation

    VkSemaphore *free_waits;
    int free_wait_count, free_wait_capacity;
    VkSemaphore *free_releases;
    int free_release_count, free_release_capacity;
    struct used_semaphore *used_semaphores;
    int used_semaphore_count, used_semaphore_capacity;
    struct retired_image *retired;
    int retired_count, retired_capacity;
    struct staging_chunk *staging;
    int staging_count, staging_capacity;

    struct vela_readback *readbacks[VELA_READBACK_MAX_JOBS];
    size_t readback_next; // round-robin cursor in the bounded queue
    struct wl_event_source *readback_timer;
    size_t readback_count, readback_bytes;
    uint64_t readback_completed, readback_cancelled, readback_failed, readback_rejected;

    // The arrays of a submission, reused (they only grow).
    VkSemaphoreSubmitInfo *wait_infos;
    int wait_infos_capacity;
    VkSemaphore *wait_semaphores;
    int wait_semaphores_capacity;

    struct wlr_drm_syncobj_timeline *sync_timeline;
    uint64_t sync_point;

    VkQueryPool query_pool;
    uint64_t timing[VELA_TIMING_SLOTS]; // timeline point of each slot; UINT64_MAX: recording
    int next_timing;
    // Calibration: the same moment read on the GPU and on CLOCK_MONOTONIC.
    uint64_t calibration_ticks;
    int64_t calibration_ns;
    int64_t calibrated_at;

    struct wl_list targets; // vela_target.link
    struct wl_list textures; // vela_texture.link
    struct wlr_drm_format_set shm_formats;

    VkSampler nearest;
    VkSampler linear;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkShaderModule vert, texture_frag, rect_frag, shadow_frag, blur_down_frag, blur_up_frag, blur_mix_frag;
    struct pipeline_entry *pipelines;
    int pipeline_count, pipeline_capacity;
    struct vela_blur_image blur[VELA_BLUR_LEVELS];
};

struct vela_target *vela_renderer_target(struct vela_renderer *renderer, struct wlr_buffer *buffer);
// A free command buffer, already recording.
VkCommandBuffer vela_renderer_begin_commands(struct vela_renderer *renderer);
// Uploads of wl_shm buffers are recorded here and go to the GPU with the next
// submission, before it.
VkCommandBuffer vela_renderer_upload_commands(struct vela_renderer *renderer);
bool vela_renderer_stage(struct vela_renderer *renderer, VkDeviceSize size, struct vela_staging *out);
// The same for copies the CPU reads back (captures): cached memory, where
// reading is many times faster than from the uncached memory uploads use.
bool vela_renderer_stage_readback(struct vela_renderer *renderer, VkDeviceSize size, struct vela_staging *out);
// Submits `cmd` (still recording). wait_fds: sync_files to wait for first,
// closed by the renderer. release_fd, if not NULL, gets a sync_file signaled
// at the end of the work (-1 if not available: then the CPU has already
// waited). Returns the timeline point, 0 if the submission failed.
uint64_t vela_renderer_submit(struct vela_renderer *renderer, VkCommandBuffer cmd, const int *wait_fds,
    int wait_count, int *release_fd);
// Compositor-owned readback: GPU-wait for earlier draws, then make later
// submissions GPU-wait for this copy. Queue order alone is not a dependency.
uint64_t vela_renderer_submit_readback(struct vela_renderer *renderer, VkCommandBuffer cmd);
void vela_renderer_wait(struct vela_renderer *renderer, uint64_t point);
// Frees what the GPU has finished using (retired images, semaphores, extra
// staging blocks).
void vela_renderer_collect(struct vela_renderer *renderer);
// An image to destroy once the GPU has finished all the work submitted so far
// and the work being recorded. view and memory can be VK_NULL_HANDLE.
void vela_renderer_retire_image(struct vela_renderer *renderer, VkImage image, VkImageView view,
    VkDeviceMemory memory);
// Signals the next point of the syncobj timeline with `sync_file` (not closed;
// -1: the work is already done). Returns the point, 0 without a timeline.
uint64_t vela_renderer_signal_sync_point(struct vela_renderer *renderer, int sync_file);

// Pipelines: one per (target format, kind, blending).
VkPipeline vela_renderer_pipeline(struct vela_renderer *renderer, VkFormat target, enum vela_pipeline_kind kind,
    bool blend);

// Levels large enough to blur a zone of width x height pixels.
bool vela_renderer_prepare_blur(struct vela_renderer *renderer, uint32_t width, uint32_t height);

// A measurement slot for the next command buffer; -1 without timestamps or
// when all slots are in use.
int vela_renderer_timing_slot(struct vela_renderer *renderer);
void vela_renderer_write_timestamp(struct vela_renderer *renderer, VkCommandBuffer cmd, int slot, bool end);
void vela_renderer_timing_submitted(struct vela_renderer *renderer, int slot, uint64_t point);

#endif
