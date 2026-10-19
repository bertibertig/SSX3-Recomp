/*
 * Vulkan render back end: device, window, swapchain, clears, flips
 * (milestone 1), triangles with blend, depth, stencil and alpha test
 * (milestone 2), register combiners and four texture stages as one
 * uber-shader (milestone 3). Design: docs/vulkan-backend.md.
 *
 * Threading: every entry point is called on the NV2A poll thread, one at a
 * time (nv2a_backend.h). Vulkan, the window and the swapchain are created
 * lazily by the first call. With frame interpolation (default) a presenter
 * thread owns the swapchain; the two share the queue under s_qlock and hand
 * recorded frames over under I.lock (docs/frame-interpolation.md).
 *
 * Frame model: clears and draws between two flips are recorded into one
 * command buffer. Without interpolation flip blits the surface the title last
 * rendered into to the swapchain, submits and presents. With it, flip submits
 * and passes the frame's recording to the presenter, which re-renders it with
 * vertex positions blended from the frame before, as often as the display
 * allows.
 *
 * Switches:
 *   SSX3_VK_VALIDATE=1      enable VK_LAYER_KHRONOS_validation (if installed)
 *   SSX3_VK_DUMP=<prefix>   write the presented surface as <prefix><flip number>.bmp
 *   SSX3_VK_DUMP_EVERY=<n>  ... every n-th flip (default 60)
 *   SSX3_VK_GPU=<substr>    pick the physical device whose name contains it
 *   SSX3_VK_TEXLOG=1        log the first 60 re-decodes of cached textures
 *   SSX3_VK_NO_RC=1         ignore the combiner extension (milestone-2 look)
 *   SSX3_INTERP=0           no frame interpolation: present the game's own frames
 *   SSX3_FPS_LIMIT=<n>      presenter frame cap (default: fastest display's refresh;
 *                           0 = uncapped)
 *   SSX3_VSYNC=1            FIFO presentation (default mailbox / immediate)
 *   SSX3_VKI_DUMP=<prefix>  write interpolated frames at blend 0, 1/2, 1 as
 *                           <prefix><flip>_<t*100>.bmp, every SSX3_VK_DUMP_EVERY flips
 *   SSX3_RES_SCALE=<1..5>   internal resolution as a multiple of the title's
 *                           (default 1 = 640x480); F10 cycles it at run time
 *   SSX3_ASPECT=<shape>     widescreen in game: 16:9, 16:10, 21:9, any W:H or
 *                           decimal, or auto (the desktop's shape); default 4:3
 *                           (off). Menus, loading screens and movies stay 4:3,
 *                           pillarboxed.
 *
 * Internal resolution: every guest surface is a GPU image of its guest size
 * times the scale (surface_extent). Vertices stay in guest pixels -- the
 * shaders divide by the guest size -- so only image sizes, viewports, scissors
 * and blits see the scale.
 *
 * Widescreen (SSX3_ASPECT): in game the title projects a view R.wide times
 * wider than 4:3 into its 640x480 target (recomp_manual.c, camera hook).
 * Screen-sized surfaces then get images R.wide times wider and a display
 * aspect to match, so the squeezed picture is stretched back; 2D vertices
 * (rhw 1, the HUD) are narrowed to the 4:3 middle in the vertex shader, except
 * on the screen edges (full-screen passes). Outside the game R.wide is 1 and
 * the 4:3 frame is pillarboxed in the window like any other.
 */
#include "volk.h"
#include <SDL.h>
#include <SDL_vulkan.h>

#include <pthread.h>
#include <sys/resource.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "nv2a_backend.h"     /* xboxrecomp/src/kernel */

/* SPIR-V, compiled from src/render/shaders/ at build time (glslc -mfmt=c). */
static const uint32_t k_vert_spv[] =
#include "nv2a.vert.inc"
;
static const uint32_t k_frag_spv[] =
#include "nv2a.frag.inc"
;
#include "keyboard_sdl.h"     /* xboxrecomp/src/input */
#include "vk_backend.h"

#define VKB_MAX_SURFACES    16
#define VKB_MAX_SWAP_IMAGES 8
#define VKB_MAX_TEXTURES    512
#define VKB_MAX_SAMPLERS    32
#define VKB_VERTEX_RING     (32u << 20)    /* vertex bytes per frame */
#define VKB_STAGING_RING    (64u << 20)    /* texture upload bytes per frame */
#define VKB_FRAMES          2              /* frames in flight (NV2A thread) */

/* A guest colour surface, as a GPU image. Keyed by guest address and size:
 * a title that reuses an address at another size gets a fresh image. */
typedef struct {
    uint32_t       va, width, height;      /* guest size: the key, and vertex space */
    uint32_t       logical_w, logical_h;   /* width/aa_sx, height/aa_sy */
    uint32_t       img_w, img_h;           /* GPU image: guest size x scale */
    float          aspect;                 /* display aspect of the whole image */
    int            scale;                  /* R.scale it was made at */
    float          wide;                   /* widescreen factor it was made at (1 = 4:3) */
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkImageLayout  layout;
    uint32_t       last_used;              /* flip number, for eviction */
} VkbSurface;

/* A guest depth/stencil surface (Nv2aRenderState.zeta_va), same size as the
 * colour surface's image it is drawn with (width/height: image pixels). */
typedef struct {
    uint32_t       va, width, height;
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkImageLayout  layout;
    uint32_t       last_used;
    int            scale;                  /* R.scale it was made at */
} VkbDepth;

/* A decoded guest texture. Re-validated once per frame by hashing the
 * decoded texels: a title rewrites textures in place (SSX 3's FMV frame is
 * one 640x480 texture updated every frame). */
typedef struct {
    uint32_t       offset, width, height, color, pitch;
    uint64_t       hash;
    uint32_t       checked;                /* flip number of the last check */
    uint32_t       last_used;
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
} VkbTexture;

/* Host-visible buffer used as a per-frame bump allocator. */
typedef struct {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    uint8_t       *mapped;
    VkDeviceSize   size, used;
} VkbRing;

/* Push constants shared by both shaders (layout matches nv2a.vert/.frag). */
typedef struct {
    float    surface_size[2];
    float    tex_size[2];
    float    z_scale;
    uint32_t flags;          /* 1 textured | 2 combiners | func<<8 | ref<<16
                              * | 1<<31 alpha test */
    float    squeeze;        /* x of 2D vertices times this (1 / surface's wide) */
} VkbPush;

/* One batch's register-combiner state, std140, as nv2a.frag's RC block. */
typedef struct {
    uint32_t cw[8][4];       /* color_icw, alpha_icw, color_ocw, alpha_ocw */
    float    c0[8][4], c1[8][4];
    uint32_t fin[4];         /* final0, final1, control, stage_program */
    float    fc0[4], fc1[4], fog_color[4];
    float    tex_scale[4][4];
    uint32_t misc[4];        /* clip_plane_mode */
} VkbCombiners;

/* Bindings of the push-descriptor set: four stage samplers, then the UBO. */
#define VKB_BIND_RC 4

static struct {
    int               failed;              /* init failed: stay inert */
    int               ready;
    SDL_Window       *window;
    VkInstance        instance;
    VkSurfaceKHR      wsi;
    VkPhysicalDevice  gpu;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice          device;
    uint32_t          queue_family;
    VkQueue           queue;

    VkSwapchainKHR    swapchain;
    VkFormat          swap_format;
    VkExtent2D        swap_extent;
    uint32_t          swap_count;
    VkImage           swap_images[VKB_MAX_SWAP_IMAGES];
    VkSemaphore       sem_render_done[VKB_MAX_SWAP_IMAGES];
    volatile int      swap_dirty;          /* resized / out of date (any thread) */
    int               host_main;           /* window owned by ssx3_vk_host_main */
    volatile int      display_hz;          /* refresh of the window's display, 0 unknown */

    /* Frames in flight: the title records frame n+1 while the GPU still draws
     * frame n. Each has its own command buffer, fence, acquire semaphore and
     * rings; cmd/frame_fence/sem_acquire/vring/staging name the current one
     * (ensure_recording advances). Images are shared: barriers are
     * ALL_COMMANDS, so a frame's commands wait for the frame before on the
     * queue, and every image free goes through device_idle. */
    VkCommandPool     cmd_pool;
    VkCommandBuffer   cmds[VKB_FRAMES];
    VkFence           fences[VKB_FRAMES];
    VkSemaphore       acquires[VKB_FRAMES];
    VkbRing           vrings[VKB_FRAMES], stagings[VKB_FRAMES];
    uint32_t          frame;               /* current index into the above */
    VkCommandBuffer   cmd;
    VkFence           frame_fence;
    VkSemaphore       sem_acquire;
    int               recording;

    VkbSurface        surfaces[VKB_MAX_SURFACES];
    VkbSurface       *last;                /* last surface cleared/drawn */
    uint32_t          flips;

    VkFormat          depth_format;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout  pipe_layout;
    VkPipeline        pipeline;
    VkSampler         samplers[VKB_MAX_SAMPLERS];
    uint32_t          sampler_keys[VKB_MAX_SAMPLERS];
    uint32_t          nsamplers;
    VkbRing          *vring, *staging;
    VkDeviceSize      ubo_align;           /* minUniformBufferOffsetAlignment */
    VkbCombiners      last_rc;             /* last UBO written this frame ... */
    VkDeviceSize      last_rc_off;         /* ... and where (~0: none yet) */
    VkbDepth          depths[VKB_MAX_SURFACES];
    VkbTexture        textures[VKB_MAX_TEXTURES];
    VkbTexture        white;               /* bound when a batch is untextured */
    uint32_t         *decode_buf;          /* decode scratch, grown on demand */
    size_t            decode_cap;

    /* The attachments of the open dynamic-rendering scope, if any. */
    int               rendering;
    VkbSurface       *rt_color;
    VkbDepth         *rt_depth;

    uint32_t          frame_draws;         /* draw() calls since the last flip */
    int               first_draw_logged;
    uint32_t          stat_decodes;        /* texture decodes since the last log */
    uint64_t          stat_decode_px;      /* ... and their texels */
    uint32_t          stat_new;            /* ... of which first sight / cache misses */
    uint32_t          last_frame_draws;
    uint32_t          title_ms, title_flips;   /* window-title FPS counter */
    uint32_t          title_presents;

    const char       *dump_prefix;
    uint32_t          dump_every;
    uint32_t          max_dim;             /* maxImageDimension2D */
} V;

/* Internal resolution. `want` is set from the environment and by the window
 * thread (F10); the NV2A thread takes it over at a flip, so all of one frame
 * renders at one scale. Surfaces are found by image size, so images of the old
 * scale simply stop matching and are evicted like any unused surface. */
#define VKB_MAX_SCALE 5
static struct {
    volatile int want;
    int          scale;
    int          purge;     /* scale changed: free the old images */
    /* Widescreen: horizontal factor on top of the scale for screen-sized
     * surfaces (image wider, display aspect wider by the same factor); 1 =
     * the title's 4:3. `wide` is what this frame renders at, taken over at
     * a flip: g_ssx3_wide while the title is in game, else 1. */
    float        wide;
    int          wide_auto;            /* SSX3_ASPECT=auto: resolve from the desktop */
    uint32_t     screen_w, screen_h;   /* guest size of the shown surface */
} R = { 1, 1, 0, 1.0f, 0, 640, 480 };

extern volatile float g_ssx3_wide;     /* recomp_manual.c: requested factor */
extern int ssx3_in_game(void);         /* recomp_manual.c */
static void wide_auto(void);
static void window_size(int *w, int *h);

/* ---- frame interpolation: declarations ------------------------------------
 *
 * SSX 3's simulation runs on a fixed 60 Hz step (1/60 is a constant in ~50
 * code sites, not a delta time; docs/notes.md), so a higher frame rate cannot
 * come from the title. Instead every frame's draws are recorded as they are
 * executed, and a presenter thread re-renders the last frame with each vertex
 * position blended from its place in the frame before, at whatever rate the
 * display (or SSX3_FPS_LIMIT) allows. Shown frames trail the game by at most
 * one frame. Details: docs/frame-interpolation.md. */

#define VKI_PACKETS   6        /* recorded frames in the ring */
#define VKI_SNAPS     8        /* render-target textures copied per frame */
#define VKI_INFLIGHT  2        /* presenter command buffers */
#define VKI_RING      (32u << 20)

enum { VKI_FREE, VKI_FILLING, VKI_READY, VKI_HELD };

/* One recorded clear or draw, everything needed to issue it again. */
typedef struct {
    VkbSurface     *target;
    uint32_t        type;            /* 0 draw, 1 clear */
    uint32_t        count;
    size_t          vofs, eofs;      /* into the packet's vertex / ext arrays */
    int             has_ext, rc;     /* rc: combiner block index, -1 none */
    VkImageView     views[4];
    VkSampler       samplers[4];
    int             surf[4];         /* V.surfaces index of a render-target view */
    VkbPush         pc;
    Nv2aRenderState rs;
    uint64_t        key;             /* identity for matching across frames */
    uint64_t        sig;             /* texture-coordinate hash (presenter) */
    uint32_t        flags, argb, zstencil;   /* clear */
} VkiCmd;

typedef struct {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    uint32_t       w, h;
} VkiImage;

/* One recorded frame. Filled by the NV2A thread, read-only once READY. */
typedef struct {
    int             state;
    uint64_t        release_serial;  /* presenter submissions that may read it */
    VkiCmd         *cmds;  size_t ncmds, ccap;
    Nv2aVertex     *vtx;   size_t nvtx, vcap;
    Nv2aVertexExt  *ext;   size_t next, ecap;
    VkbCombiners   *rc;    size_t nrc, rcap;
    int            *match; size_t mcap;     /* per cmd: draw of the frame before, -1
                                             * none, -2 motion borrowed (synth_motion) */
    float         (*ppos)[4]; size_t pcap;  /* per vertex: x, y, z, rhw a frame earlier */
    VkiImage        snap[VKI_SNAPS];        /* copies of sampled render targets */
    uint32_t        width, height, logical_w, logical_h;   /* guest size */
    uint32_t        img_w, img_h;           /* the shown surface's image */
    float           aspect;                 /* ... and its display aspect */
    uint32_t        flip;
    uint32_t        steps;                  /* the title's world steps at its present */
    double          arrival, t_show, dur;   /* host seconds; the blend from the frame
                                             * before runs t_show .. t_show + dur */
} VkiPacket;

static struct {
    int             on;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    VkiPacket       pk[VKI_PACKETS];
    VkiPacket      *rec;             /* being filled (NV2A thread) */
    int             rec_none;        /* no free slot this frame */
    double          period;          /* smoothed interval between game frames */
    double          jitter, cushion; /* mean lateness spread, extra latency */
    double          step;            /* measured real length of one world step */
    int             holding;         /* blend done, next frame not here yet */
    double          last_arrival, last_show, last_dur;
    uint32_t        last_steps;
    int             have_steps;
    uint64_t        submitted, completed;   /* presenter submission serials */

    /* presenter thread only */
    VkCommandPool   pool;
    VkCommandBuffer cmd[VKI_INFLIGHT];
    VkFence         fence[VKI_INFLIGHT];
    VkSemaphore     acquire[VKI_INFLIGHT];
    VkbRing         ring[VKI_INFLIGHT];
    uint64_t        slot_serial[VKI_INFLIGHT];
    VkiImage        color, depth;
    VkiPacket      *prev, *cur;
    int             shown_final;     /* cur already shown unblended */
    double          limit_env;       /* SSX3_FPS_LIMIT: >0 cap, 0 none, <0 display */
    double          next_deadline;

    volatile uint32_t presents;      /* frames shown, for the FPS title */
    uint32_t        stat_draws, stat_matched, stat_frames;
    uint32_t        stat_jump, stat_none;   /* why draws did not match */
    uint32_t        stat_synth;             /* ... of those, given borrowed motion */
    uint32_t        stat_hold;              /* blends that waited for a late frame */
    uint32_t        stat_drop;              /* game frames not recorded: no free slot */
    uint32_t        stat_clock;             /* frames timed by the game's step count */
} I;

static void rec_draw(VkbSurface *s, const Nv2aBatch *b, const Nv2aBatchExt *e,
                     const VkbCombiners *rcu, const Nv2aTexture *const stex[4],
                     const VkImageView views[4], const VkSampler smp[4],
                     const int surf[4], const VkbPush *pc, const Nv2aRenderState *rs);
static void rec_clear(VkbSurface *s, uint32_t flags, uint32_t argb, uint32_t zstencil);
static int view_surface(VkImageView v);

/* ---- helpers --------------------------------------------------------- */

/* The one queue is shared by the NV2A thread and, with frame interpolation,
 * the presenter thread: vkQueueSubmit/Present and vkDeviceWaitIdle need
 * external synchronisation. */
static pthread_mutex_t s_qlock = PTHREAD_MUTEX_INITIALIZER;

static void device_idle(void)
{
    pthread_mutex_lock(&s_qlock);
    vkDeviceWaitIdle(V.device);
    pthread_mutex_unlock(&s_qlock);
}

static VkResult queue_submit(const VkSubmitInfo *si, VkFence fence)
{
    VkResult r;
    pthread_mutex_lock(&s_qlock);
    r = vkQueueSubmit(V.queue, 1, si, fence);
    pthread_mutex_unlock(&s_qlock);
    return r;
}

#define VK_CHECK(call)                                                      \
    do {                                                                    \
        VkResult r_ = (call);                                               \
        if (r_ != VK_SUCCESS) {                                             \
            fprintf(stderr, "  [VK] %s failed: %d\n", #call, (int)r_);      \
            return 0;                                                       \
        }                                                                   \
    } while (0)

static uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < V.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i))
                && (V.memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* One layout transition with deliberately broad masks: correctness first,
 * milestone 4 narrows them. */
static void transition_cmd(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                           VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b;

    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
}

static void transition(VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout from, VkImageLayout to)
{
    transition_cmd(V.cmd, image, aspect, from, to);
}

static void surface_to(VkbSurface *s, VkImageLayout to)
{
    if (s->layout != to) {
        transition(s->image, VK_IMAGE_ASPECT_COLOR_BIT, s->layout, to);
        s->layout = to;
    }
}

/* ---- device and window ------------------------------------------------ */

static int create_instance(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    const char *exts[16];
    unsigned n = 16;
    const char *layers[1];
    uint32_t nlayers = 0;

    if (!SDL_Vulkan_GetInstanceExtensions(V.window, &n, exts)) {
        fprintf(stderr, "  [VK] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return 0;
    }
    if (getenv("SSX3_VK_VALIDATE")) {
        uint32_t count = 0, i;
        VkLayerProperties props[64];
        vkEnumerateInstanceLayerProperties(&count, NULL);
        if (count > 64) count = 64;
        vkEnumerateInstanceLayerProperties(&count, props);
        for (i = 0; i < count; i++)
            if (!strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation"))
                layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
        fprintf(stderr, "  [VK] validation layer %s\n",
                nlayers ? "enabled" : "requested but not installed");
    }

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "SSX 3 Recomp";
    app.pEngineName = "xboxrecomp";
    app.apiVersion = VK_API_VERSION_1_3;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = nlayers;
    ci.ppEnabledLayerNames = layers;
    VK_CHECK(vkCreateInstance(&ci, NULL, &V.instance));
    volkLoadInstance(V.instance);
    return 1;
}

/* Prefer a discrete GPU that has a queue family doing graphics and present
 * and speaks Vulkan 1.3; SSX3_VK_GPU overrides by name. */
static int pick_gpu(void)
{
    VkPhysicalDevice devs[16];
    uint32_t n = 16, i, best_score = 0;
    const char *want = getenv("SSX3_VK_GPU");

    vkEnumeratePhysicalDevices(V.instance, &n, devs);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16, q, score;

        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, V.wsi, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                break;
        }
        if (q == nq)
            continue;
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && strstr(p.deviceName, want))
            score = 100;
        if (score > best_score) {
            best_score = score;
            V.gpu = devs[i];
            V.queue_family = q;
        }
    }
    if (!V.gpu) {
        fprintf(stderr, "  [VK] no Vulkan 1.3 GPU with graphics+present\n");
        return 0;
    }
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(V.gpu, &p);
        fprintf(stderr, "  [VK] GPU: %s (Vulkan %u.%u)\n", p.deviceName,
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
        V.ubo_align = p.limits.minUniformBufferOffsetAlignment;
        if (V.ubo_align < 16)
            V.ubo_align = 16;
        V.max_dim = p.limits.maxImageDimension2D;
    }
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &V.memprops);
    return 1;
}

static int device_has_extension(const char *name)
{
    VkExtensionProperties props[512];
    uint32_t n = 512, i;
    vkEnumerateDeviceExtensionProperties(V.gpu, NULL, &n, props);
    for (i = 0; i < n; i++)
        if (!strcmp(props[i].extensionName, name))
            return 1;
    return 0;
}

/* Vulkan 1.3 core gives dynamic rendering and the first two dynamic-state
 * extensions (cull, depth, stencil); VK_EXT_extended_dynamic_state3 adds the
 * blend state, so one pipeline covers every NV2A state combination. Push
 * descriptors bind each batch's texture without descriptor pools. */
static int create_device(void)
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3, have3;
    VkPhysicalDeviceFeatures2 query;
    VkDeviceCreateInfo ci;
    const char *exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    };
    uint32_t i;

    for (i = 0; i < sizeof exts / sizeof exts[0]; i++)
        if (!device_has_extension(exts[i])) {
            fprintf(stderr, "  [VK] device lacks %s\n", exts[i]);
            return 0;
        }
    memset(&have3, 0, sizeof have3);
    have3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    memset(&query, 0, sizeof query);
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    query.pNext = &have3;
    vkGetPhysicalDeviceFeatures2(V.gpu, &query);
    if (!have3.extendedDynamicState3ColorBlendEnable
            || !have3.extendedDynamicState3ColorBlendEquation
            || !have3.extendedDynamicState3ColorWriteMask) {
        fprintf(stderr, "  [VK] device lacks dynamic blend state (EDS3)\n");
        return 0;
    }

    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = V.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    memset(&eds3, 0, sizeof eds3);
    eds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
    eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    memset(&f13, 0, sizeof f13);
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = &eds3;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &f13;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = sizeof exts / sizeof exts[0];
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(V.gpu, &ci, NULL, &V.device));
    volkLoadDevice(V.device);
    vkGetDeviceQueue(V.device, V.queue_family, 0, &V.queue);

    /* Z24S8 is the NV2A's format; not every GPU renders to it (AMD does not),
     * so fall back to 32-bit float depth with 8-bit stencil. */
    {
        static const VkFormat cand[] = { VK_FORMAT_D24_UNORM_S8_UINT,
                                         VK_FORMAT_D32_SFLOAT_S8_UINT };
        for (i = 0; i < 2; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(V.gpu, cand[i], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                V.depth_format = cand[i];
                break;
            }
        }
        if (!V.depth_format) {
            fprintf(stderr, "  [VK] no depth/stencil format\n");
            return 0;
        }
    }
    return 1;
}

static void destroy_swapchain(void)
{
    uint32_t i;
    if (!V.swapchain)
        return;
    device_idle();
    for (i = 0; i < V.swap_count; i++)
        vkDestroySemaphore(V.device, V.sem_render_done[i], NULL);
    vkDestroySwapchainKHR(V.device, V.swapchain, NULL);
    V.swapchain = VK_NULL_HANDLE;
}

/* The title paces itself on the emulated VBlank, so presentation must not
 * block the NV2A thread on the host's refresh: MAILBOX, else IMMEDIATE, else
 * FIFO (always available). */
static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    VkPresentModeKHR modes[8];
    uint32_t nf = 32, nm = 8, i;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainCreateInfoKHR ci;
    const char *vsync = getenv("SSX3_VSYNC");
    int w, h;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.gpu, V.wsi, &caps);
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.gpu, V.wsi, &nf, formats);
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.gpu, V.wsi, &nm, modes);

    V.swap_format = formats[0].format;
    for (i = 0; i < nf; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
            V.swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    for (i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
    if (vsync && *vsync == '1')
        mode = VK_PRESENT_MODE_FIFO_KHR;   /* the display's refresh rate */
    else if (mode != VK_PRESENT_MODE_MAILBOX_KHR)
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

    SDL_Vulkan_GetDrawableSize(V.window, &w, &h);
    if (caps.currentExtent.width != UINT32_MAX) {
        V.swap_extent = caps.currentExtent;
    } else {
        V.swap_extent.width = (uint32_t)w;
        V.swap_extent.height = (uint32_t)h;
    }
    if (!V.swap_extent.width || !V.swap_extent.height)
        return 0;                          /* minimised: try next flip */

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = V.wsi;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = V.swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = V.swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(V.device, &ci, NULL, &V.swapchain));

    V.swap_count = VKB_MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.swap_count, V.swap_images);
    for (i = 0; i < V.swap_count; i++) {
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0 };
        VK_CHECK(vkCreateSemaphore(V.device, &sci, NULL, &V.sem_render_done[i]));
    }
    V.swap_dirty = 0;
    fprintf(stderr, "  [VK] swapchain %ux%u, %u images, %s\n",
            V.swap_extent.width, V.swap_extent.height, V.swap_count,
            mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
            : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return 1;
}

static int ring_create(VkbRing *r, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;

    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VK_CHECK(vkCreateBuffer(V.device, &bci, NULL, &r->buffer));
    vkGetBufferMemoryRequirements(V.device, r->buffer, &req);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return 0;
    VK_CHECK(vkAllocateMemory(V.device, &ai, NULL, &r->memory));
    VK_CHECK(vkBindBufferMemory(V.device, r->buffer, r->memory, 0));
    VK_CHECK(vkMapMemory(V.device, r->memory, 0, size, 0, (void **)&r->mapped));
    r->size = size;
    r->used = 0;
    return 1;
}

/* Space for `bytes` in this frame's slice of the ring, or NULL when full. */
static void *ring_alloc(VkbRing *r, VkDeviceSize bytes, VkDeviceSize align,
                        VkDeviceSize *offset)
{
    VkDeviceSize at = (r->used + align - 1) & ~(align - 1);
    if (at + bytes > r->size)
        return NULL;
    r->used = at + bytes;
    *offset = at;
    return r->mapped + at;
}

/* A device-local 2D image with one view. */
static int image_create(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImage *img,
                        VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vci;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(V.device, &ci, NULL, img));
    vkGetImageMemoryRequirements(V.device, *img, &req);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return 0;
    VK_CHECK(vkAllocateMemory(V.device, &ai, NULL, mem));
    VK_CHECK(vkBindImageMemory(V.device, *img, *mem, 0));
    memset(&vci, 0, sizeof vci);
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = *img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(V.device, &vci, NULL, view));
    return 1;
}

static VkShaderModule shader_module(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    vkCreateShaderModule(V.device, &ci, NULL, &m);
    return m;
}

/* The one graphics pipeline. Everything the NV2A changes per batch is dynamic
 * state; what stays fixed is the vertex layout (Nv2aVertex), triangle lists,
 * the attachment formats and filled polygons. */
static int create_pipeline(void)
{
    VkDescriptorSetLayoutBinding bind[VKB_BIND_RC + 1];
    VkDescriptorSetLayoutCreateInfo dslci;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plci;
    VkShaderModule vs, fs;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription vbind[2];
    VkVertexInputAttributeDescription vattr[9];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dyn;
    VkPipelineRenderingCreateInfo prci;
    VkGraphicsPipelineCreateInfo gpci;
    VkFormat color_fmt = VK_FORMAT_B8G8R8A8_UNORM;
    static const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
    };
    int i;

    memset(bind, 0, sizeof bind);
    for (i = 0; i <= VKB_BIND_RC; i++) {
        bind[i].binding = (uint32_t)i;
        bind[i].descriptorType = i == VKB_BIND_RC ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind[i].descriptorCount = 1;
        bind[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dslci, 0, sizeof dslci);
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dslci.bindingCount = VKB_BIND_RC + 1;
    dslci.pBindings = bind;
    VK_CHECK(vkCreateDescriptorSetLayout(V.device, &dslci, NULL, &V.set_layout));

    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(VkbPush);
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &V.set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(V.device, &plci, NULL, &V.pipe_layout));

    vs = shader_module(k_vert_spv, sizeof k_vert_spv);
    fs = shader_module(k_frag_spv, sizeof k_frag_spv);
    if (!vs || !fs)
        return 0;
    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    /* Binding 0: Nv2aVertex. Binding 1: Nv2aVertexExt, its stride dynamic so
     * a batch without the extension can bind one zeroed element (stride 0). */
    vbind[0].binding = 0;
    vbind[0].stride = sizeof(Nv2aVertex);
    vbind[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vbind[1].binding = 1;
    vbind[1].stride = sizeof(Nv2aVertexExt);
    vbind[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vattr[0].location = 0; vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[0].offset = (uint32_t)offsetof(Nv2aVertex, x);
    vattr[1].location = 1; vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_B8G8R8A8_UNORM;        /* 0xAARRGGBB, LE bytes B,G,R,A */
    vattr[1].offset = (uint32_t)offsetof(Nv2aVertex, diffuse);
    vattr[2].location = 2; vattr[2].binding = 0;
    vattr[2].format = VK_FORMAT_R32G32_SFLOAT;
    vattr[2].offset = (uint32_t)offsetof(Nv2aVertex, u);
    for (i = 0; i < 4; i++) {
        vattr[3 + i].location = 3 + (uint32_t)i; vattr[3 + i].binding = 1;
        vattr[3 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        vattr[3 + i].offset = (uint32_t)(offsetof(Nv2aVertexExt, tex) + i * 16);
    }
    vattr[7].location = 7; vattr[7].binding = 1;
    vattr[7].format = VK_FORMAT_B8G8R8A8_UNORM;
    vattr[7].offset = (uint32_t)offsetof(Nv2aVertexExt, specular);
    vattr[8].location = 8; vattr[8].binding = 1;
    vattr[8].format = VK_FORMAT_R32_SFLOAT;
    vattr[8].offset = (uint32_t)offsetof(Nv2aVertexExt, fog);
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vbind;
    vi.vertexAttributeDescriptionCount = 9;
    vi.pVertexAttributeDescriptions = vattr;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    /* The executor already clipped at the near plane; window z outside the
     * buffer's range is clamped, not clipped, as SET_CLIP_MIN/MAX only clip
     * what the title asks for (nv2a_backend.h notes). */
    rs.depthClampEnable = VK_FALSE;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&cba, 0, sizeof cba);
    cba.colorWriteMask = 0xF;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    memset(&dyn, 0, sizeof dyn);
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = sizeof dynamic / sizeof dynamic[0];
    dyn.pDynamicStates = dynamic;
    memset(&prci, 0, sizeof prci);
    prci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &color_fmt;
    prci.depthAttachmentFormat = V.depth_format;
    prci.stencilAttachmentFormat = V.depth_format;

    memset(&gpci, 0, sizeof gpci);
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.pNext = &prci;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = V.pipe_layout;
    VK_CHECK(vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gpci, NULL, &V.pipeline));
    vkDestroyShaderModule(V.device, vs, NULL);
    vkDestroyShaderModule(V.device, fs, NULL);
    return 1;
}

static int init_vulkan(void)
{
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fci;
    VkSemaphoreCreateInfo sci;
    const char *e;
    int i;

    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "  [VK] no Vulkan loader (libvulkan / vulkan-1.dll)\n");
        return 0;
    }
    if (!V.window) {
        /* No ssx3_vk_host_main: open the window here, on the NV2A thread,
         * which then also pumps its events (from flip). */
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "  [VK] SDL video: %s\n", SDL_GetError());
            return 0;
        }
        int ww, wh;
        wide_auto();
        window_size(&ww, &wh);
        V.window = SDL_CreateWindow("SSX 3 (Recomp, Vulkan)",
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    ww, wh,
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
        if (!V.window) {
            fprintf(stderr, "  [VK] SDL_CreateWindow: %s\n", SDL_GetError());
            return 0;
        }
        xbox_InputSdlInit();
    }
    if (!create_instance())
        return 0;
    if (!SDL_Vulkan_CreateSurface(V.window, V.instance, &V.wsi)) {
        fprintf(stderr, "  [VK] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 0;
    }
    if (!pick_gpu() || !create_device() || !create_swapchain() || !create_pipeline())
        return 0;
    for (i = 0; i < VKB_FRAMES; i++)
        if (!ring_create(&V.vrings[i], VKB_VERTEX_RING,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
                || !ring_create(&V.stagings[i], VKB_STAGING_RING,
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return 0;

    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = V.queue_family;
    VK_CHECK(vkCreateCommandPool(V.device, &pci, NULL, &V.cmd_pool));
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = V.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VKB_FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(V.device, &ai, V.cmds));
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < VKB_FRAMES; i++) {
        VK_CHECK(vkCreateFence(V.device, &fci, NULL, &V.fences[i]));
        VK_CHECK(vkCreateSemaphore(V.device, &sci, NULL, &V.acquires[i]));
    }
    V.frame = VKB_FRAMES - 1;      /* the first ensure_recording takes slot 0 */
    V.cmd = V.cmds[V.frame];
    V.frame_fence = V.fences[V.frame];
    V.sem_acquire = V.acquires[V.frame];
    V.vring = &V.vrings[V.frame];
    V.staging = &V.stagings[V.frame];

    V.dump_prefix = getenv("SSX3_VK_DUMP");
    e = getenv("SSX3_VK_DUMP_EVERY");
    V.dump_every = e && atoi(e) > 0 ? (uint32_t)atoi(e) : 60;
    return 1;
}

static void interp_start(void);
static void purge_scaled(void);

/* First call from the executor brings everything up; a failure leaves the
 * back end inert (the title keeps running, nothing is drawn). */
static int ensure_ready(void)
{
    if (V.ready)
        return 1;
    if (V.failed)
        return 0;
    if (!init_vulkan()) {
        V.failed = 1;
        fprintf(stderr, "  [VK] back end disabled\n");
        return 0;
    }
    V.ready = 1;
    interp_start();
    return 1;
}

static int ensure_recording(void)
{
    VkCommandBufferBeginInfo bi;

    if (V.recording)
        return 1;
    V.frame = (V.frame + 1) % VKB_FRAMES;
    V.cmd = V.cmds[V.frame];
    V.frame_fence = V.fences[V.frame];
    V.sem_acquire = V.acquires[V.frame];
    V.vring = &V.vrings[V.frame];
    V.staging = &V.stagings[V.frame];
    /* The frame that last used this slot has retired: its rings are free. */
    vkWaitForFences(V.device, 1, &V.frame_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(V.device, 1, &V.frame_fence);
    vkResetCommandBuffer(V.cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(V.cmd, &bi));
    V.recording = 1;
    V.vring->used = 0;
    V.staging->used = 0;
    V.last_rc_off = ~(VkDeviceSize)0;
    if (R.purge)
        purge_scaled();
    return 1;
}

/* ---- surfaces --------------------------------------------------------- */

static void end_rendering(void)
{
    if (V.rendering) {
        vkCmdEndRendering(V.cmd);
        V.rendering = 0;
    }
}

static void surface_free(VkbSurface *s)
{
    if (s->view)
        vkDestroyImageView(V.device, s->view, NULL);
    if (s->image)
        vkDestroyImage(V.device, s->image, NULL);
    if (s->memory)
        vkFreeMemory(V.device, s->memory, NULL);
    memset(s, 0, sizeof *s);
}

/* Evicting an image the frame being recorded still references would free it
 * under the command buffer, so only images unused this frame are candidates;
 * a full cache of in-use images fails the request (logged once). */
/* The GPU image size for a guest surface at the current scale, clamped to
 * what the device can create. */
static void surface_extent(const Nv2aSurface *g, uint32_t *w, uint32_t *h, float *wide)
{
    double sx, sy = R.scale;

    /* Only the screen-sized targets are widened, never shadow maps or other
     * offscreen surfaces: the title's 2D passes into those must stay as drawn. */
    *wide = (g->width == R.screen_w && g->height == R.screen_h) ? R.wide : 1.0f;
    sx = R.scale * *wide;
    double mw = V.max_dim ? V.max_dim : 4096;

    if (g->width * sx > mw)  sx = mw / g->width;
    if (g->height * sy > mw) sy = mw / g->height;
    *w = (uint32_t)(g->width * sx + 0.5);
    *h = (uint32_t)(g->height * sy + 0.5);
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}

static VkbSurface *surface_get(const Nv2aSurface *g)
{
    VkbSurface *s, *victim = NULL;
    uint32_t iw, ih;
    float wide;
    int i;

    surface_extent(g, &iw, &ih, &wide);
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        s = &V.surfaces[i];
        if (s->image && s->va == g->color_va
                && s->width == g->width && s->height == g->height
                && s->img_w == iw && s->img_h == ih) {
            s->last_used = V.flips;
            return s;
        }
    }
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        s = &V.surfaces[i];
        if (!s->image) { victim = s; break; }
        if (s->last_used != V.flips && (!victim || s->last_used < victim->last_used))
            victim = s;
    }
    if (!victim) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [VK] surface cache full\n");
        return NULL;
    }
    if (victim->image) {
        device_idle();
        if (V.last == victim) V.last = NULL;
        surface_free(victim);
    }
    s = victim;
    if (!image_create(iw, ih, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &s->image, &s->memory, &s->view)) {
        surface_free(s);
        return NULL;
    }
    s->va = g->color_va;
    s->width = g->width;
    s->height = g->height;
    s->logical_w = g->width / (g->aa_sx ? g->aa_sx : 1);
    s->logical_h = g->height / (g->aa_sy ? g->aa_sy : 1);
    s->img_w = iw;
    s->img_h = ih;
    s->aspect = (float)s->logical_w / (float)s->logical_h * wide;
    s->scale = R.scale;
    s->wide = wide;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s->last_used = V.flips;
    fprintf(stderr, "  [VK] surface 0x%08X %ux%u (logical %ux%u, image %ux%u)\n",
            s->va, s->width, s->height, s->logical_w, s->logical_h, iw, ih);
    return s;
}

static void depth_free(VkbDepth *d)
{
    if (d->view)
        vkDestroyImageView(V.device, d->view, NULL);
    if (d->image)
        vkDestroyImage(V.device, d->image, NULL);
    if (d->memory)
        vkFreeMemory(V.device, d->memory, NULL);
    memset(d, 0, sizeof *d);
}

/* The depth/stencil image for a guest zeta surface. va 0 is a scratch buffer
 * for draws without one: the pipeline always has a depth attachment, which
 * keeps it a single pipeline; such draws run with the depth test off. */
static VkbDepth *depth_get(uint32_t va, uint32_t w, uint32_t h)
{
    VkbDepth *d, *victim = NULL;
    int i;

    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        d = &V.depths[i];
        if (d->image && d->va == va && d->width == w && d->height == h) {
            d->last_used = V.flips;
            return d;
        }
    }
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        d = &V.depths[i];
        if (!d->image) { victim = d; break; }
        if (d->last_used != V.flips && (!victim || d->last_used < victim->last_used))
            victim = d;
    }
    if (!victim)
        return NULL;
    if (victim->image) {
        device_idle();
        depth_free(victim);
    }
    d = victim;
    if (!image_create(w, h, V.depth_format,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                      &d->image, &d->memory, &d->view)) {
        depth_free(d);
        return NULL;
    }
    d->va = va;
    d->width = w;
    d->height = h;
    d->scale = R.scale;
    d->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    d->last_used = V.flips;
    return d;
}

static void depth_to(VkbDepth *d, VkImageLayout to)
{
    if (d->layout != to) {
        transition(d->image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                   d->layout, to);
        d->layout = to;
    }
}

/* After a scale or widescreen change, at the start of the next frame (no
 * command buffer being recorded references the images; device_idle retires
 * the ones in flight): free the colour and depth images of the old shape at
 * once rather than leave them for eviction -- at 5x one surface is ~30 MB.
 * The presenter samples only its own snapshots and textures, never these.
 * Depth images do not record which colour surface they belong to, so a
 * widescreen change (R.purge 2) frees them all; the next draw remakes them. */
static void purge_scaled(void)
{
    int i, idle = 0, all_depths = R.purge == 2;

    R.purge = 0;
    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        VkbSurface *s = &V.surfaces[i];
        VkbDepth *d = &V.depths[i];
        float wide = s->image && s->width == R.screen_w && s->height == R.screen_h
                     ? R.wide : 1.0f;
        if (s->image && (s->scale != R.scale || s->wide != wide)) {
            if (!idle++) device_idle();
            if (V.last == s) V.last = NULL;
            surface_free(s);
        }
        if (d->image && (d->scale != R.scale || all_depths)) {
            if (!idle++) device_idle();
            depth_free(d);
        }
    }
}

/* Open (or keep) a dynamic-rendering scope on these attachments. */
static void begin_rendering(VkbSurface *c, VkbDepth *d)
{
    VkRenderingAttachmentInfo ca, da;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;

    if (V.rendering && V.rt_color == c && V.rt_depth == d)
        return;
    end_rendering();
    surface_to(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    depth_to(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = c->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da = ca;
    da.imageView = d->view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = c->img_w;
    ri.renderArea.extent.height = c->img_h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(V.cmd, &ri);
    V.rendering = 1;
    V.rt_color = c;
    V.rt_depth = d;

    vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipeline);
    vp.x = 0; vp.y = 0;
    vp.width = (float)c->img_w; vp.height = (float)c->img_h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(V.cmd, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = c->img_w; sc.extent.height = c->img_h;
    vkCmdSetScissor(V.cmd, 0, 1, &sc);
}

/* ---- textures ------------------------------------------------------------ */

extern ptrdiff_t xbox_GetMemoryOffset(void);   /* xbox_memory_layout.h */

/* Hash of the guest bytes a texture lives in, to notice when the title has
 * rewritten it. Raw memory, not decoded texels: decoding every texture every
 * frame just to compare it was a third of the NV2A thread's time.
 *
 * Level 0's exact size: an upper bound (w*h*4 for everything swizzled) took
 * in whatever lies behind a DXT texture, and in a race that is data the
 * title rewrites every frame -- ~20 textures and 1.9 M texels were decoded
 * again each frame for nothing (race at 8-10 fps). Clamped to the guest
 * region the texture lies in. 64-bit multiply-xorshift per 8 bytes: several
 * GB/s, so a frame's worth of textures costs well under a millisecond. */
static uint64_t texture_bytes(const Nv2aTexture *g)
{
    uint64_t w = g->width, h = g->height;

    if (g->pitch)
        return (uint64_t)g->pitch * h;
    switch (g->color) {
    case 0x0C:                                  /* DXT1: 8 bytes per 4x4 */
        return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    case 0x0E: case 0x0F:                       /* DXT3/5: 16 bytes per 4x4 */
        return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    case 0x00: case 0x01: case 0x0B: case 0x19: /* Y8, AY8, P8, A8 */
        return w * h;
    case 0x02: case 0x03: case 0x04: case 0x05: /* A1R5G5B5 .. R5G6B5 */
    case 0x1A: case 0x27: case 0x28:            /* A8Y8, R6G5B5, G8B8 */
        return w * h * 2;
    default:                                    /* 32 bpp, or unknown: bound */
        return w * h * 4;
    }
}

static uint64_t hash_guest_texture(const Nv2aTexture *g)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t va = g->offset, bytes, limit, h = 0x9E3779B97F4A7C15ull, w;
    const uint8_t *p;
    size_t i, n;

    bytes = texture_bytes(g);
    if (va >= 0x80000000ull && va < 0x84000000ull)
        limit = 0x84000000ull - va;            /* contiguous window */
    else if (va < 0x04000000ull)
        limit = 0x04000000ull - va;            /* main RAM */
    else
        return 0;
    if (bytes > limit)
        bytes = limit;
    p = mem + va;
    n = (size_t)(bytes / 8);
    for (i = 0; i < n; i++) {
        memcpy(&w, p + i * 8, 8);
        h ^= w;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    }
    return h | 1;                              /* 0 means "never uploaded" */
}

static void texture_free(VkbTexture *t)
{
    if (t->view)
        vkDestroyImageView(V.device, t->view, NULL);
    if (t->image)
        vkDestroyImage(V.device, t->image, NULL);
    if (t->memory)
        vkFreeMemory(V.device, t->memory, NULL);
    memset(t, 0, sizeof *t);
}

/* Copy w*h ARGB texels into the image (in the frame's command buffer, so draws
 * recorded before this one still see the old contents). */
static int texture_upload(VkbTexture *t, const uint32_t *argb)
{
    VkDeviceSize off, bytes = (VkDeviceSize)t->width * t->height * 4;
    void *dst = ring_alloc(V.staging, bytes, 16, &off);
    VkBufferImageCopy region;

    if (!dst) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [VK] texture staging full this frame\n");
        return 0;
    }
    memcpy(dst, argb, (size_t)bytes);     /* 0xAARRGGBB LE = B,G,R,A bytes */
    end_rendering();
    transition(t->image, VK_IMAGE_ASPECT_COLOR_BIT,
               t->hash ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&region, 0, sizeof region);
    region.bufferOffset = off;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = t->width;
    region.imageExtent.height = t->height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(V.cmd, V.staging->buffer, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

static int white_ready(void)
{
    static const uint32_t px = 0xFFFFFFFFu;
    if (V.white.hash)
        return 1;
    if (!V.white.image) {
        V.white.width = V.white.height = 1;
        if (!image_create(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, &V.white.image, &V.white.memory,
                          &V.white.view))
            return 0;
    }
    if (!texture_upload(&V.white, &px))
        return 0;
    V.white.hash = 1;
    return 1;
}

/* The image view a batch samples for its texture: a surface the GPU rendered
 * (render-to-texture: guest memory never saw those pixels), or a cached
 * decode of guest memory, re-validated once per frame. NULL: draw untextured. */
static VkImageView texture_view(const Nv2aTexture *g)
{
    VkbTexture *t = NULL, *victim = NULL;
    size_t n;
    uint64_t h;
    int i;

    for (i = 0; i < VKB_MAX_SURFACES; i++) {
        VkbSurface *s = &V.surfaces[i];
        if (s->image && s->va == g->offset && s->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
            if (s == V.rt_color && V.rendering)
                return NULL;               /* sampling the target being drawn */
            end_rendering();
            surface_to(s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return s->view;
        }
    }
    if (!g->width || !g->height || g->width > 4096 || g->height > 4096)
        return NULL;

    for (i = 0; i < VKB_MAX_TEXTURES; i++) {
        VkbTexture *c = &V.textures[i];
        if (c->image && c->offset == g->offset && c->width == g->width
                && c->height == g->height && c->color == g->color && c->pitch == g->pitch) {
            t = c;
            break;
        }
    }
    if (t && t->checked == V.flips) {
        t->last_used = V.flips;
        return t->view;
    }
    h = hash_guest_texture(g);
    if (t && (!h || t->hash == h)) {           /* unchanged, or unhashable: once */
        t->checked = V.flips;
        t->last_used = V.flips;
        return t->view;
    }

    n = (size_t)g->width * g->height;
    if (n > V.decode_cap) {
        free(V.decode_buf);
        V.decode_buf = (uint32_t *)malloc(n * 4);
        V.decode_cap = V.decode_buf ? n : 0;
        if (!V.decode_buf)
            return NULL;
    }
    if (!nv2a_backend_decode_texture(g, V.decode_buf))
        return NULL;                       /* format the executor cannot decode */
    V.stat_decodes++;
    V.stat_decode_px += n;
    V.stat_new += !t;
    if (t && getenv("SSX3_VK_TEXLOG")) {
        static int nlog;
        if (nlog++ < 60)
            fprintf(stderr, "  [VK] re-decode %08X %ux%u fmt %02X pitch %u hash %016llx\n",
                    g->offset, g->width, g->height, g->color, g->pitch,
                    (unsigned long long)h);
    }
    if (!h)
        h = 1;                             /* outside guest RAM: upload once */

    if (!t) {
        for (i = 0; i < VKB_MAX_TEXTURES; i++) {
            VkbTexture *c = &V.textures[i];
            if (!c->image) { victim = c; break; }
            /* With interpolation the presenter still draws the last few
             * frames' textures, so those stay too. */
            if (V.flips - c->last_used >= (I.on ? 8u : 1u)
                    && (!victim || c->last_used < victim->last_used))
                victim = c;
        }
        if (!victim)
            return NULL;
        if (victim->image) {
            device_idle();
            texture_free(victim);
        }
        t = victim;
        if (!image_create(g->width, g->height, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, &t->image, &t->memory, &t->view)) {
            texture_free(t);
            return NULL;
        }
        t->offset = g->offset; t->width = g->width; t->height = g->height;
        t->color = g->color;   t->pitch = g->pitch;
    }
    if (!texture_upload(t, V.decode_buf))
        return t->hash ? t->view : NULL;
    t->hash = h;
    t->checked = V.flips;
    t->last_used = V.flips;
    return t->view;
}

/* NV2A wrap modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border. Filter word:
 * MIN in bits 16-23, MAG in bits 24-27, 1 = nearest. */
static VkSampler sampler_get(const Nv2aTexture *g)
{
    static const VkSamplerAddressMode modes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    uint32_t mag_nearest = g && ((g->filter >> 24) & 0xF) == 1;
    uint32_t min_nearest = g && ((g->filter >> 16) & 0xFF) == 1;
    uint32_t key = (g ? (g->addr_u & 7) | ((g->addr_v & 7) << 3) : 3 | (3 << 3))
                 | (mag_nearest << 6) | (min_nearest << 7);
    VkSamplerCreateInfo ci;
    uint32_t i;

    for (i = 0; i < V.nsamplers; i++)
        if (V.sampler_keys[i] == key)
            return V.samplers[i];
    if (V.nsamplers == VKB_MAX_SAMPLERS)
        return V.samplers[0];
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = mag_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = min_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = modes[key & 7];
    ci.addressModeV = modes[(key >> 3) & 7];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.maxLod = 0.0f;
    if (vkCreateSampler(V.device, &ci, NULL, &V.samplers[V.nsamplers]) != VK_SUCCESS)
        return V.nsamplers ? V.samplers[0] : VK_NULL_HANDLE;
    V.sampler_keys[V.nsamplers] = key;
    return V.samplers[V.nsamplers++];
}

/* ---- debug dump --------------------------------------------------------- */

/* Copy a surface back and write it as a bottom-up 24-bit BMP, the format the
 * executor's own RECOMP_FB_DUMP writes, so the two can be compared directly.
 * Waits for the GPU: a debugging aid, not for every frame. */
static void dump_surface(VkbSurface *s)
{
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkBufferImageCopy region;
    VkSubmitInfo si;
    VkDeviceSize size = (VkDeviceSize)s->img_w * s->img_h * 4;
    uint8_t *px;
    char path[512];
    FILE *f;
    uint32_t y, x, row_bytes, pad;
    uint8_t hdr[54];
    uint32_t filesz;

    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(V.device, &bci, NULL, &buf) != VK_SUCCESS)
        return;
    vkGetBufferMemoryRequirements(V.device, buf, &req);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(V.device, &ai, NULL, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(V.device, buf, NULL);
        return;
    }
    vkBindBufferMemory(V.device, buf, mem, 0);

    vkWaitForFences(V.device, 1, &V.frame_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(V.device, 1, &V.frame_fence);
    vkResetCommandBuffer(V.cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(V.cmd, &bi);
    surface_to(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = s->img_w;
    region.imageExtent.height = s->img_h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
    vkEndCommandBuffer(V.cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    queue_submit(&si, V.frame_fence);
    vkWaitForFences(V.device, 1, &V.frame_fence, VK_TRUE, UINT64_MAX);

    vkMapMemory(V.device, mem, 0, size, 0, (void **)&px);
    /* Named by flip number, so a dump can be matched to log lines. */
    snprintf(path, sizeof path, "%s%06u.bmp", V.dump_prefix, V.flips);
    f = fopen(path, "wb");
    if (f) {
        row_bytes = s->img_w * 3;
        pad = (4 - (row_bytes & 3)) & 3;
        filesz = 54 + (row_bytes + pad) * s->img_h;
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &s->img_w, 4);
        memcpy(hdr + 22, &s->img_h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = s->img_h; y-- > 0;) {
            const uint8_t *row = px + (size_t)y * s->img_w * 4;   /* B,G,R,A */
            for (x = 0; x < s->img_w; x++)
                fwrite(row + x * 4, 1, 3, f);
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
    }
    vkUnmapMemory(V.device, mem);
    vkDestroyBuffer(V.device, buf, NULL);
    vkFreeMemory(V.device, mem, NULL);
}

/* ---- back-end entry points -------------------------------------------- */

/* ---- NV2A state (GL enum values, nv2a_backend.h) -> Vulkan ---------------- */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;          /* 0x8006 */
    }
}

/* GL_NEVER 0x200 .. GL_ALWAYS 0x207 are in Vulkan's order. */
static VkCompareOp compare_op(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (VkCompareOp)(f - 0x200) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;       /* 0x1E00 */
    }
}

static void apply_state(VkCommandBuffer cmd, const Nv2aRenderState *rs)
{
    VkBool32 blend = rs->blend_enable ? VK_TRUE : VK_FALSE;
    VkColorBlendEquationEXT eq;
    VkColorComponentFlags mask = 0;
    float bc[4];

    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blend);
    eq.srcColorBlendFactor = blend_factor(rs->blend_src);
    eq.dstColorBlendFactor = blend_factor(rs->blend_dst);
    eq.colorBlendOp = blend_op(rs->blend_eq);
    eq.srcAlphaBlendFactor = eq.srcColorBlendFactor;
    eq.dstAlphaBlendFactor = eq.dstColorBlendFactor;
    eq.alphaBlendOp = eq.colorBlendOp;
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
    bc[0] = (float)((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bc[1] = (float)((rs->blend_color >>  8) & 0xFF) / 255.0f;
    bc[2] = (float)( rs->blend_color        & 0xFF) / 255.0f;
    bc[3] = (float)((rs->blend_color >> 24) & 0xFF) / 255.0f;
    vkCmdSetBlendConstants(cmd, bc);

    if (rs->color_mask & 0x00010000u) mask |= VK_COLOR_COMPONENT_R_BIT;
    if (rs->color_mask & 0x00000100u) mask |= VK_COLOR_COMPONENT_G_BIT;
    if (rs->color_mask & 0x00000001u) mask |= VK_COLOR_COMPONENT_B_BIT;
    if (rs->color_mask & 0x01000000u) mask |= VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);

    /* Culling stays off: the NV2A decides facing from the projected, mirrored
     * triangle (nv2a_backend.h advice). Per-triangle culling: milestone 4. */
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_CLOCKWISE);

    /* GL writes depth only while the test is enabled, and so does the NV2A. */
    vkCmdSetDepthTestEnable(cmd, rs->depth_test_enable ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, (rs->depth_test_enable && rs->depth_write) ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, compare_op(rs->depth_func));

    vkCmdSetStencilTestEnable(cmd, rs->stencil_enable ? VK_TRUE : VK_FALSE);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                      stencil_op(rs->stencil_fail), stencil_op(rs->stencil_zpass),
                      stencil_op(rs->stencil_zfail), compare_op(rs->stencil_func));
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_func_mask & 0xFF);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_write_mask & 0xFF);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_ref & 0xFF);
}

/* ---- back-end entry points -------------------------------------------- */

/* ---- frame interpolation ------------------------------------------------- */

/* The title's world-step count at its last present (src/recomp_manual.c);
 * one step is nominally 1/60 s -- in practice its timer's period in whole
 * milliseconds, so the real step length is measured (I.step). */
extern volatile uint32_t g_ssx3_present_steps;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Grow *p to hold `need` elements; 0 when out of memory. */
static int grow(void **p, size_t *cap, size_t need, size_t elem)
{
    size_t n = *cap ? *cap : 256;
    void *q;

    if (need <= *cap)
        return 1;
    while (n < need)
        n *= 2;
    if (!(q = realloc(*p, n * elem)))
        return 0;
    *p = q;
    *cap = n;
    return 1;
}

static int view_surface(VkImageView v)
{
    int i;
    if (!v)
        return -1;
    for (i = 0; i < VKB_MAX_SURFACES; i++)
        if (V.surfaces[i].image && V.surfaces[i].view == v)
            return i;
    return -1;
}

static void vki_image_free(VkiImage *m)
{
    if (m->view)
        vkDestroyImageView(V.device, m->view, NULL);
    if (m->image)
        vkDestroyImage(V.device, m->image, NULL);
    if (m->memory)
        vkFreeMemory(V.device, m->memory, NULL);
    memset(m, 0, sizeof *m);
}

/* The slot this frame records into, or NULL when the presenter holds them
 * all (the frame is then not shown; it still renders as usual). */
static VkiPacket *rec_packet(void)
{
    int i;

    if (I.rec || I.rec_none)
        return I.rec;
    pthread_mutex_lock(&I.lock);
    /* Retire presenter submissions that finished (a fence's serial only moves
     * after its submission, so a signalled fence covers the serial read). */
    for (i = 0; i < VKI_INFLIGHT; i++)
        if (I.slot_serial[i] > I.completed && vkGetFenceStatus(V.device, I.fence[i]) == VK_SUCCESS)
            I.completed = I.slot_serial[i];
    for (i = 0; i < VKI_PACKETS; i++) {
        VkiPacket *p = &I.pk[i];
        if (p->state == VKI_FREE && p->release_serial <= I.completed) {
            p->state = VKI_FILLING;
            p->ncmds = p->nvtx = p->next = p->nrc = 0;
            I.rec = p;
            break;
        }
    }
    pthread_mutex_unlock(&I.lock);
    if (!I.rec) {
        I.rec_none = 1;
        I.stat_drop++;
    }
    return I.rec;
}

static uint64_t fnv(uint64_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 0x100000001B3ull;
    }
    return h;
}

/* What makes two draws in consecutive frames "the same object": textures
 * by guest address, how it blends and tests (the vertex count is compared
 * separately: near-plane clipping changes it). Not vertex data
 * or combiner constants -- those animate. */
static uint64_t rec_key(const Nv2aBatch *b, const Nv2aBatchExt *e,
                        const Nv2aTexture *const stex[4], const Nv2aRenderState *rs)
{
    uint64_t h = 0xCBF29CE484222325ull;
    int i;

    (void)b;
    for (i = 0; i < 4; i++)
        h = fnv(h, stex[i] ? stex[i]->offset : 0);
    h = fnv(h, e ? 1u : 0u);
    if (e && e->rc) {
        h = fnv(h, e->rc->control);
        h = fnv(h, e->rc->stage_program);
    }
    h = fnv(h, rs->blend_enable | rs->blend_src << 4 | rs->blend_dst << 20);
    h = fnv(h, rs->depth_test_enable | rs->depth_write << 1 | rs->alpha_test_enable << 2
               | rs->stencil_enable << 3);
    return h;
}

static void rec_draw(VkbSurface *s, const Nv2aBatch *b, const Nv2aBatchExt *e,
                     const VkbCombiners *rcu, const Nv2aTexture *const stex[4],
                     const VkImageView views[4], const VkSampler smp[4],
                     const int surf[4], const VkbPush *pc, const Nv2aRenderState *rs)
{
    VkiPacket *p = rec_packet();
    VkiCmd *c;
    int i;

    if (!p)
        return;
    if (!grow((void **)&p->cmds, &p->ccap, p->ncmds + 1, sizeof *p->cmds)
            || !grow((void **)&p->vtx, &p->vcap, p->nvtx + b->count, sizeof *p->vtx)
            || (e && !grow((void **)&p->ext, &p->ecap, p->next + b->count, sizeof *p->ext))
            || (rcu && !grow((void **)&p->rc, &p->rcap, p->nrc + 1, sizeof *p->rc)))
        return;
    c = &p->cmds[p->ncmds++];
    memset(c, 0, sizeof *c);
    c->target = s;
    c->type = 0;
    c->count = b->count;
    c->vofs = p->nvtx;
    memcpy(p->vtx + p->nvtx, b->vertices, b->count * sizeof *p->vtx);
    p->nvtx += b->count;
    c->has_ext = e != NULL;
    if (e) {
        c->eofs = p->next;
        memcpy(p->ext + p->next, e->vertices, b->count * sizeof *p->ext);
        p->next += b->count;
    }
    c->rc = -1;
    if (rcu) {
        if (!p->nrc || memcmp(&p->rc[p->nrc - 1], rcu, sizeof *rcu))
            p->rc[p->nrc++] = *rcu;
        c->rc = (int)p->nrc - 1;
    }
    for (i = 0; i < 4; i++) {
        c->views[i] = views[i] ? views[i] : V.white.view;
        c->samplers[i] = smp[i];
        c->surf[i] = views[i] ? surf[i] : -1;
    }
    c->pc = *pc;
    c->rs = *rs;
    c->key = rec_key(b, e, stex, rs);
}

static void rec_clear(VkbSurface *s, uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkiPacket *p = rec_packet();
    VkiCmd *c;

    if (!p || !grow((void **)&p->cmds, &p->ccap, p->ncmds + 1, sizeof *p->cmds))
        return;
    c = &p->cmds[p->ncmds++];
    memset(c, 0, sizeof *c);
    c->target = s;
    c->type = 1;
    c->rc = -1;
    c->flags = flags;
    c->argb = argb;
    c->zstencil = zstencil;
}

static void rec_release(VkiPacket *p)
{
    pthread_mutex_lock(&I.lock);
    p->state = VKI_FREE;
    pthread_mutex_unlock(&I.lock);
}

/* At the flip, on the NV2A thread with V.cmd open and no rendering scope:
 * keep the commands that drew the shown surface and copy the render targets
 * they sample (the next frame redraws those while the presenter still reads
 * this one). NULL: nothing to show. */
static VkiPacket *rec_finish(VkbSurface *disp)
{
    VkiPacket *p = I.rec;
    int map[VKB_MAX_SURFACES];
    size_t i, n = 0, draws = 0;
    int nsnap = 0, k;

    I.rec = NULL;
    I.rec_none = 0;
    if (!p)
        return NULL;
    if (!disp) {
        rec_release(p);
        return NULL;
    }
    for (i = 0; i < p->ncmds; i++)
        if (p->cmds[i].target == disp) {
            draws += p->cmds[i].type == 0;
            p->cmds[n++] = p->cmds[i];
        }
    p->ncmds = n;
    if (!draws) {
        rec_release(p);
        return NULL;
    }
    for (k = 0; k < VKB_MAX_SURFACES; k++)
        map[k] = -1;
    for (i = 0; i < p->ncmds; i++) {
        VkiCmd *c = &p->cmds[i];
        for (k = 0; k < 4; k++) {
            VkbSurface *rt;
            VkiImage *m;
            VkImageCopy cp;
            int sidx = c->surf[k];

            if (c->type != 0 || sidx < 0)
                continue;
            if (map[sidx] >= 0) {
                c->views[k] = p->snap[map[sidx]].view;
                continue;
            }
            rt = &V.surfaces[sidx];
            if (nsnap == VKI_SNAPS || !rt->image || rt->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                c->views[k] = V.white.view;
                continue;
            }
            m = &p->snap[nsnap];
            if (m->image && (m->w != rt->img_w || m->h != rt->img_h))
                vki_image_free(m);       /* earlier V.cmd retired, presenter released it */
            if (!m->image) {
                if (!image_create(rt->img_w, rt->img_h, VK_FORMAT_B8G8R8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, &m->image, &m->memory, &m->view)) {
                    vki_image_free(m);
                    c->views[k] = V.white.view;
                    continue;
                }
                m->w = rt->img_w;
                m->h = rt->img_h;
            }
            surface_to(rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(m->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            memset(&cp, 0, sizeof cp);
            cp.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.srcSubresource.layerCount = 1;
            cp.dstSubresource = cp.srcSubresource;
            cp.extent.width = rt->img_w;
            cp.extent.height = rt->img_h;
            cp.extent.depth = 1;
            vkCmdCopyImage(V.cmd, rt->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           m->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            transition(m->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            map[sidx] = nsnap++;
            c->views[k] = m->view;
        }
    }
    p->width = disp->width;
    p->height = disp->height;
    p->logical_w = disp->logical_w;
    p->logical_h = disp->logical_h;
    p->img_w = disp->img_w;
    p->img_h = disp->img_h;
    p->aspect = disp->aspect;
    p->flip = V.flips;
    p->steps = g_ssx3_present_steps;
    return p;
}

/* Hand a finished frame to the presenter (after its V.cmd was submitted)
 * and give it a display time. The blend into it lasts as long as the game
 * time between it and the frame before -- world steps x 1/60 s when the
 * title's step count moved (two steps when it fell behind and updated twice),
 * else the smoothed arrival interval -- and starts where the previous blend
 * ends, so motion plays back evenly whatever the arrival jitter. A cushion of
 * about twice that jitter keeps a slightly late frame from stalling a blend;
 * the start stays within [arrival + cushion, + two steps] and drifts back
 * towards arrival + cushion, so the shown frames trail the game by about one
 * frame plus the cushion. */
static void rec_publish(VkiPacket *p)
{
    double now = now_s(), interval, target, lo, hi;
    uint32_t dsteps = p->steps - I.last_steps;

    pthread_mutex_lock(&I.lock);
    interval = now - I.last_arrival;

    if (I.last_arrival > 0 && interval < 0.1) {
        I.period += (interval - I.period) * 0.05;
        if (I.period < 1.0 / 240) I.period = 1.0 / 240;
        if (I.period > 1.0 / 15) I.period = 1.0 / 15;
    }
    if (I.have_steps && dsteps >= 1 && dsteps <= 6) {
        if (interval < 0.1)
            I.step += (interval / dsteps - I.step) * 0.02;
        p->dur = dsteps * I.step;
        I.stat_clock++;
    } else {
        p->dur = I.period;
    }
    I.have_steps = 1;
    I.last_steps = p->steps;
    target = I.last_show + I.last_dur;
    if (I.last_arrival > 0 && interval < 0.1)
        I.jitter += (fabs(target - now - I.cushion) - I.jitter) * 0.05;
    I.cushion = 2.0 * I.jitter;
    if (I.cushion < 0.002) I.cushion = 0.002;
    if (I.cushion > 0.016) I.cushion = 0.016;
    lo = now + I.cushion;
    hi = lo + 2 * I.step;
    if (I.last_arrival <= 0 || interval >= 0.1 || target < lo)
        p->t_show = lo;
    else {
        /* Ahead of what the cushion needs: give a tenth of the excess back
         * per frame, so the latency settles at the cushion and stays. */
        p->t_show = target - 0.1 * (target - lo);
        if (p->t_show > hi)
            p->t_show = hi;
    }
    p->arrival = now;
    I.last_arrival = now;
    I.last_show = p->t_show;
    I.last_dur = p->dur;
    p->state = VKI_READY;
    pthread_cond_signal(&I.cond);
    pthread_mutex_unlock(&I.lock);
}

/* Texture coordinates of a draw as one hash: static geometry keeps them
 * from frame to frame while its positions move, so equal signatures (same
 * key and count) identify the same mesh. */
static uint64_t draw_sig(const VkiPacket *p, const VkiCmd *c)
{
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t v, bits[2];

    for (v = 0; v < c->count; v++) {
        if (c->has_ext) {
            memcpy(bits, p->ext[c->eofs + v].tex[0], sizeof bits);
        } else {
            memcpy(&bits[0], &p->vtx[c->vofs + v].u, 4);
            memcpy(&bits[1], &p->vtx[c->vofs + v].v, 4);
        }
        h = fnv(h, bits[0]);
        h = fnv(h, bits[1]);
    }
    return h;
}

/* Mean screen distance over up to 16 sampled vertices; FLT_MAX when any
 * sampled vertex jumps further than `limit` or crosses w = 0. */
static float draw_dist(const Nv2aVertex *a, const Nv2aVertex *b, uint32_t n, float limit)
{
    uint32_t step = n > 16 ? n / 16 : 1, v, m = 0;
    float sum = 0;

    for (v = 0; v < n; v += step, m++) {
        float dx = fabsf(a[v].x - b[v].x), dy = fabsf(a[v].y - b[v].y);
        if (dx > limit || dy > limit || (a[v].rhw > 0) != (b[v].rhw > 0))
            return 3.0e38f;
        sum += dx + dy;
    }
    return sum / (float)m;
}

/* Where every vertex of `cur` was a frame earlier (cur->ppos). Paired draws
 * take their partner's vertices. A transformed draw without a partner -- a
 * terrain chunk whose near-plane clipping changed its vertex count, say --
 * would stand still while its neighbours move and open cracks between them,
 * so it borrows motion: a vertex at exactly the position of a paired vertex
 * (a shared edge) takes that vertex's motion, any other the motion of the
 * nearest paired vertex at a similar depth. Screen-space draws without a
 * partner (HUD) stay put. */
#define VKI_GX        32
#define VKI_GY        24
#define VKI_CELL      16
#define VKI_HASH_BITS 18

typedef struct { float m[4]; float x, y, w; } VkiSample;

static struct { uint32_t xb, yb, gen; float m[4]; } s_vh[1u << VKI_HASH_BITS];
static uint32_t  s_vgen;
static VkiSample s_cell[VKI_GY][VKI_GX][VKI_CELL];
static uint32_t  s_cell_n[VKI_GY][VKI_GX], s_cell_seen[VKI_GY][VKI_GX];

static uint32_t vhash(uint32_t xb, uint32_t yb)
{
    return ((xb * 0x9E3779B1u) ^ (yb * 0x85EBCA77u)) >> (32 - VKI_HASH_BITS);
}

static void cell_of(const VkiPacket *p, float x, float y, int *cx, int *cy)
{
    int gx = (int)(x * VKI_GX / (float)p->width), gy = (int)(y * VKI_GY / (float)p->height);
    *cx = gx < 0 ? 0 : gx >= VKI_GX ? VKI_GX - 1 : gx;
    *cy = gy < 0 ? 0 : gy >= VKI_GY ? VKI_GY - 1 : gy;
}

static void synth_motion(const VkiPacket *prev, VkiPacket *cur)
{
    float scale = 1.0f / (float)(cur->width + cur->height);
    size_t i;
    uint32_t v;

    if (!prev || !grow((void **)&cur->ppos, &cur->pcap, cur->nvtx ? cur->nvtx : 1,
                       sizeof *cur->ppos)) {
        for (i = 0; i < cur->ncmds; i++)
            cur->match[i] = -1;
        return;
    }
    if (++s_vgen == 0) {
        memset(s_vh, 0, sizeof s_vh);
        s_vgen = 1;
    }
    memset(s_cell_n, 0, sizeof s_cell_n);
    memset(s_cell_seen, 0, sizeof s_cell_seen);

    for (i = 0; i < cur->ncmds; i++) {
        const VkiCmd *c = &cur->cmds[i];
        const Nv2aVertex *a, *b;
        if (c->type != 0 || cur->match[i] < 0)
            continue;
        a = prev->vtx + prev->cmds[cur->match[i]].vofs;
        b = cur->vtx + c->vofs;
        for (v = 0; v < c->count; v++) {
            float *pp = cur->ppos[c->vofs + v];
            float m[4];
            uint32_t xb, yb, h, probe;
            int cx, cy;
            uint32_t *n, seen;

            pp[0] = a[v].x; pp[1] = a[v].y; pp[2] = a[v].z; pp[3] = a[v].rhw;
            if (!c->has_ext || b[v].rhw <= 0)
                continue;
            m[0] = a[v].x - b[v].x; m[1] = a[v].y - b[v].y;
            m[2] = a[v].z - b[v].z; m[3] = a[v].rhw - b[v].rhw;
            memcpy(&xb, &b[v].x, 4);
            memcpy(&yb, &b[v].y, 4);
            for (h = vhash(xb, yb), probe = 0; probe < 8;
                 probe++, h = (h + 1) & ((1u << VKI_HASH_BITS) - 1)) {
                if (s_vh[h].gen != s_vgen) {
                    s_vh[h].gen = s_vgen;
                    s_vh[h].xb = xb;
                    s_vh[h].yb = yb;
                    memcpy(s_vh[h].m, m, sizeof m);
                    break;
                }
                if (s_vh[h].xb == xb && s_vh[h].yb == yb)
                    break;
            }
            cell_of(cur, b[v].x, b[v].y, &cx, &cy);
            n = &s_cell_n[cy][cx];
            seen = ++s_cell_seen[cy][cx];
            {
                /* Reservoir: an even sample of the cell, not its first draws. */
                uint32_t slot = *n < VKI_CELL ? (*n)++ : (seen * 2654435761u) % seen;
                if (slot < VKI_CELL) {
                    VkiSample *sm = &s_cell[cy][cx][slot];
                    memcpy(sm->m, m, sizeof m);
                    sm->x = b[v].x;
                    sm->y = b[v].y;
                    sm->w = 1.0f / b[v].rhw;
                }
            }
        }
    }

    for (i = 0; i < cur->ncmds; i++) {
        const VkiCmd *c = &cur->cmds[i];
        const Nv2aVertex *b;
        if (c->type != 0 || cur->match[i] >= 0 || !c->has_ext)
            continue;
        b = cur->vtx + c->vofs;
        for (v = 0; v < c->count; v++) {
            float *pp = cur->ppos[c->vofs + v];
            const float *m = NULL;
            uint32_t xb, yb, h, probe;

            memcpy(&xb, &b[v].x, 4);
            memcpy(&yb, &b[v].y, 4);
            for (h = vhash(xb, yb), probe = 0; probe < 8 && s_vh[h].gen == s_vgen;
                 probe++, h = (h + 1) & ((1u << VKI_HASH_BITS) - 1))
                if (s_vh[h].xb == xb && s_vh[h].yb == yb) {
                    m = s_vh[h].m;
                    break;
                }
            if (!m && b[v].rhw > 0) {
                float w = 1.0f / b[v].rhw, best = 3.0e38f;
                int cx, cy, gx, gy;
                uint32_t k;
                cell_of(cur, b[v].x, b[v].y, &cx, &cy);
                for (gy = cy - 1; gy <= cy + 1; gy++)
                    for (gx = cx - 1; gx <= cx + 1; gx++) {
                        if (gx < 0 || gy < 0 || gx >= VKI_GX || gy >= VKI_GY)
                            continue;
                        for (k = 0; k < s_cell_n[gy][gx]; k++) {
                            const VkiSample *sm = &s_cell[gy][gx][k];
                            float score = (fabsf(sm->x - b[v].x) + fabsf(sm->y - b[v].y)) * scale
                                        + 2.0f * fabsf(sm->w - w) / (sm->w > w ? sm->w : w);
                            if (score < best) {
                                best = score;
                                m = sm->m;
                            }
                        }
                    }
            }
            pp[0] = b[v].x; pp[1] = b[v].y; pp[2] = b[v].z; pp[3] = b[v].rhw;
            if (m) {
                pp[0] += m[0]; pp[1] += m[1]; pp[2] += m[2]; pp[3] += m[3];
            }
        }
        cur->match[i] = -2;
        I.stat_synth++;
    }
}

/* Pair each draw of `cur` with the same object in `prev`: same identity key
 * and vertex count, preferring equal texture coordinates, then the nearest.
 * Draws that jump (camera cut, respawn) or have no partner are shown as
 * they are. */
static void match_packets(const VkiPacket *prev, VkiPacket *cur)
{
    enum { HBITS = 12, HSIZE = 1 << HBITS };
    static int head[HSIZE];
    static int *chain, *used;
    static size_t chain_cap, used_cap;
    size_t i, k;
    float limit, near_limit;

    if (!grow((void **)&cur->match, &cur->mcap, cur->ncmds ? cur->ncmds : 1, sizeof *cur->match))
        return;
    for (i = 0; i < cur->ncmds; i++) {
        cur->match[i] = -1;
        if (cur->cmds[i].type == 0)
            cur->cmds[i].sig = draw_sig(cur, &cur->cmds[i]);
    }
    if (!prev || prev->width != cur->width || prev->height != cur->height)
        return;
    if (!grow((void **)&chain, &chain_cap, prev->ncmds + 1, sizeof *chain)
            || !grow((void **)&used, &used_cap, prev->ncmds + 1, sizeof *used))
        return;
    limit = 0.25f * (float)(cur->width > cur->height ? cur->width : cur->height);
    near_limit = limit / 4;
    for (k = 0; k < HSIZE; k++)
        head[k] = -1;
    /* Chains in reverse so each walks prev in submission order. */
    for (k = prev->ncmds; k-- > 0;) {
        const VkiCmd *q = &prev->cmds[k];
        uint32_t hb;
        used[k] = 0;
        if (q->type != 0)
            continue;
        hb = (uint32_t)((q->key ^ (q->count * 0x9E3779B97F4A7C15ull)) >> (64 - HBITS));
        chain[k] = head[hb];
        head[hb] = (int)k;
    }
    for (i = 0; i < cur->ncmds; i++) {
        const VkiCmd *c = &cur->cmds[i];
        const Nv2aVertex *b = cur->vtx + c->vofs;
        uint32_t hb;
        int best = -1, best_sig = 0, any = 0, q;
        float best_d = 3.0e38f;

        if (c->type != 0)
            continue;
        I.stat_draws++;
        hb = (uint32_t)((c->key ^ (c->count * 0x9E3779B97F4A7C15ull)) >> (64 - HBITS));
        for (q = head[hb]; q >= 0; q = chain[q]) {
            const VkiCmd *p = &prev->cmds[q];
            int same_sig;
            float d;
            if (used[q] || p->key != c->key || p->count != c->count)
                continue;
            any = 1;
            same_sig = p->sig == c->sig;
            if (best_sig && !same_sig)
                continue;
            d = draw_dist(prev->vtx + p->vofs, b, c->count, limit);
            if (d > (same_sig ? limit : near_limit))
                continue;
            if ((same_sig && !best_sig) || d < best_d) {
                best = q;
                best_d = d;
                best_sig = same_sig;
            }
        }
        if (best >= 0) {
            cur->match[i] = best;
            used[best] = 1;
            I.stat_matched++;
        } else if (any) {
            I.stat_jump++;
        } else {
            I.stat_none++;
        }
    }
    synth_motion(prev, cur);
}

static int vki_target(uint32_t w, uint32_t h)
{
    if (I.color.image && I.color.w == w && I.color.h == h)
        return 1;
    device_idle();
    vki_image_free(&I.color);
    vki_image_free(&I.depth);
    if (!image_create(w, h, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT, &I.color.image, &I.color.memory, &I.color.view)
            || !image_create(w, h, V.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                             &I.depth.image, &I.depth.memory, &I.depth.view)) {
        vki_image_free(&I.color);
        vki_image_free(&I.depth);
        return 0;
    }
    I.color.w = I.depth.w = w;
    I.color.h = I.depth.h = h;
    return 1;
}

/* Replay `cur` into the presenter's target with positions at blend t
 * between the matched draws of `prev` (t = 1: exactly `cur`). */
static void vki_replay(VkCommandBuffer cmd, VkbRing *ring, const VkiPacket *prev,
                       const VkiPacket *cur, float t)
{
    VkRenderingAttachmentInfo ca, da;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    VkDeviceSize rc_off = 0;
    int rc_last = -1;
    size_t i;

    transition_cmd(cmd, I.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition_cmd(cmd, I.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = I.color.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da = ca;
    da.imageView = I.depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 1.0f;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = cur->img_w;
    ri.renderArea.extent.height = cur->img_h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipeline);
    vp.x = 0; vp.y = 0;
    vp.width = (float)cur->img_w; vp.height = (float)cur->img_h;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    sc.offset.x = 0; sc.offset.y = 0;
    sc.extent.width = cur->img_w; sc.extent.height = cur->img_h;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    for (i = 0; i < cur->ncmds; i++) {
        const VkiCmd *c = &cur->cmds[i];
        VkDescriptorImageInfo dii[4];
        VkDescriptorBufferInfo dbi;
        VkWriteDescriptorSet w[VKB_BIND_RC + 1];
        VkDeviceSize voff[2], vstride[2];
        VkBuffer vbufs[2];
        Nv2aVertex *dst;
        void *edst;
        int k;

        if (c->type == 1) {
            VkClearAttachment at[2];
            VkClearRect rect;
            uint32_t n = 0;
            memset(at, 0, sizeof at);
            if (c->flags & 0xF0) {
                at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                at[n].clearValue.color.float32[0] = (float)((c->argb >> 16) & 0xFF) / 255.0f;
                at[n].clearValue.color.float32[1] = (float)((c->argb >>  8) & 0xFF) / 255.0f;
                at[n].clearValue.color.float32[2] = (float)( c->argb        & 0xFF) / 255.0f;
                at[n].clearValue.color.float32[3] = (float)((c->argb >> 24) & 0xFF) / 255.0f;
                n++;
            }
            if (c->flags & 0x3) {
                at[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                at[n].clearValue.depthStencil.depth =
                    (float)((double)(c->zstencil >> 8) / 16777215.0);
                at[n].clearValue.depthStencil.stencil = c->zstencil & 0xFF;
                n++;
            }
            if (n) {
                memset(&rect, 0, sizeof rect);
                rect.rect = sc;
                rect.layerCount = 1;
                vkCmdClearAttachments(cmd, n, at, 1, &rect);
            }
            continue;
        }

        dst = (Nv2aVertex *)ring_alloc(ring, (VkDeviceSize)c->count * sizeof(Nv2aVertex),
                                       16, &voff[0]);
        edst = ring_alloc(ring, c->has_ext ? (VkDeviceSize)c->count * sizeof(Nv2aVertexExt)
                                           : sizeof(Nv2aVertexExt), 16, &voff[1]);
        if (c->rc >= 0 && c->rc != rc_last) {
            void *u = ring_alloc(ring, sizeof(VkbCombiners), V.ubo_align, &rc_off);
            if (u) {
                memcpy(u, &cur->rc[c->rc], sizeof(VkbCombiners));
                rc_last = c->rc;
            } else {
                dst = NULL;
            }
        }
        if (!dst || !edst) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "  [VKI] presenter vertex ring full\n");
            break;
        }
        if (prev && t < 1.0f && cur->match && cur->match[i] != -1) {
            const float (*a)[4] = (const float (*)[4])cur->ppos + c->vofs;
            const Nv2aVertex *b = cur->vtx + c->vofs;
            uint32_t v;
            for (v = 0; v < c->count; v++) {
                dst[v] = b[v];
                dst[v].x = a[v][0] + (b[v].x - a[v][0]) * t;
                dst[v].y = a[v][1] + (b[v].y - a[v][1]) * t;
                dst[v].z = a[v][2] + (b[v].z - a[v][2]) * t;
                dst[v].rhw = a[v][3] + (b[v].rhw - a[v][3]) * t;
            }
        } else {
            memcpy(dst, cur->vtx + c->vofs, c->count * sizeof(Nv2aVertex));
        }
        if (c->has_ext)
            memcpy(edst, cur->ext + c->eofs, c->count * sizeof(Nv2aVertexExt));
        else
            memset(edst, 0, sizeof(Nv2aVertexExt));

        memset(w, 0, sizeof w);
        for (k = 0; k < 4; k++) {
            dii[k].sampler = c->samplers[k];
            dii[k].imageView = c->views[k];
            dii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstBinding = (uint32_t)k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &dii[k];
        }
        dbi.buffer = ring->buffer;
        dbi.offset = c->rc >= 0 ? rc_off : 0;
        dbi.range = sizeof(VkbCombiners);
        w[VKB_BIND_RC].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[VKB_BIND_RC].dstBinding = VKB_BIND_RC;
        w[VKB_BIND_RC].descriptorCount = 1;
        w[VKB_BIND_RC].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[VKB_BIND_RC].pBufferInfo = &dbi;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipe_layout,
                                  0, VKB_BIND_RC + 1, w);
        apply_state(cmd, &c->rs);
        vkCmdPushConstants(cmd, V.pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof c->pc, &c->pc);
        vbufs[0] = vbufs[1] = ring->buffer;
        vstride[0] = sizeof(Nv2aVertex);
        vstride[1] = c->has_ext ? sizeof(Nv2aVertexExt) : 0;
        vkCmdBindVertexBuffers2(cmd, 0, 2, vbufs, voff, NULL, vstride);
        vkCmdDraw(cmd, c->count, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
}

/* Render and present one frame at blend t. 0: nothing presented. */
static int vki_present(float t)
{
    int slot = (int)(I.submitted % VKI_INFLIGHT);
    VkCommandBuffer cmd = I.cmd[slot];
    VkCommandBufferBeginInfo bi;
    VkMemoryBarrier mb;
    VkImageSubresourceRange range;
    VkClearColorValue black;
    VkImageBlit blit;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkImage img;
    VkResult r;
    uint32_t idx;
    double sw, sh, aspect, fw, fh;

    vkWaitForFences(V.device, 1, &I.fence[slot], VK_TRUE, UINT64_MAX);
    pthread_mutex_lock(&I.lock);
    if (I.slot_serial[slot] > I.completed)
        I.completed = I.slot_serial[slot];
    pthread_mutex_unlock(&I.lock);

    if (V.swap_dirty || !V.swapchain) {
        destroy_swapchain();
        if (!create_swapchain())
            return 0;
    }
    if (!vki_target(I.cur->img_w, I.cur->img_h))
        return 0;
    r = vkAcquireNextImageKHR(V.device, V.swapchain, 100000000ull, I.acquire[slot],
                              VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
            V.swap_dirty = 1;
        return 0;
    }
    vkResetFences(V.device, 1, &I.fence[slot]);
    vkResetCommandBuffer(cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    /* Everything the NV2A thread submitted before (snapshots, texture
     * uploads) is visible to this replay. */
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    I.ring[slot].used = 0;
    vki_replay(cmd, &I.ring[slot], I.prev, I.cur, t);

    img = V.swap_images[idx];
    memset(&range, 0, sizeof range);
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    memset(&black, 0, sizeof black);
    transition_cmd(cmd, I.color.image, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition_cmd(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)I.cur->img_w;
    blit.srcOffsets[1].y = (int32_t)I.cur->img_h;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    sw = (double)V.swap_extent.width;
    sh = (double)V.swap_extent.height;
    aspect = I.cur->aspect;
    fw = sw; fh = sw / aspect;
    if (fh > sh) { fh = sh; fw = sh * aspect; }
    blit.dstOffsets[0].x = (int32_t)((sw - fw) / 2);
    blit.dstOffsets[0].y = (int32_t)((sh - fh) / 2);
    blit.dstOffsets[1].x = blit.dstOffsets[0].x + (int32_t)fw;
    blit.dstOffsets[1].y = blit.dstOffsets[0].y + (int32_t)fh;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(cmd, I.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    transition_cmd(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cmd);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &I.acquire[slot];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &V.sem_render_done[idx];
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &V.sem_render_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &V.swapchain;
    pi.pImageIndices = &idx;
    pthread_mutex_lock(&s_qlock);
    r = vkQueueSubmit(V.queue, 1, &si, I.fence[slot]);
    if (r == VK_SUCCESS)
        r = vkQueuePresentKHR(V.queue, &pi);
    pthread_mutex_unlock(&s_qlock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        V.swap_dirty = 1;
    pthread_mutex_lock(&I.lock);
    I.slot_serial[slot] = ++I.submitted;
    pthread_mutex_unlock(&I.lock);
    I.presents++;
    return 1;
}

/* SSX3_VKI_DUMP=<prefix>: write the presenter's image (the blended frame,
 * before scaling to the window) as <prefix><flip>_<t*100>.bmp, for frames
 * whose flip number is a multiple of SSX3_VK_DUMP_EVERY. Waits for the GPU. */
static void vki_dump(int slot, uint32_t flip, float t)
{
    VkCommandBuffer cmd = I.cmd[slot];
    VkCommandBufferBeginInfo bi;
    VkBufferImageCopy region;
    VkSubmitInfo si;
    VkbRing buf;
    uint32_t w = I.color.w, h = I.color.h, x, y, row_bytes, pad, filesz;
    uint8_t hdr[54];
    char path[512];
    FILE *f;

    memset(&buf, 0, sizeof buf);
    if (!ring_create(&buf, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return;
    vkWaitForFences(V.device, 1, &I.fence[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(V.device, 1, &I.fence[slot]);
    vkResetCommandBuffer(cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &bi);
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, I.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf.buffer, 1, &region);
    vkEndCommandBuffer(cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    queue_submit(&si, I.fence[slot]);
    vkWaitForFences(V.device, 1, &I.fence[slot], VK_TRUE, UINT64_MAX);

    snprintf(path, sizeof path, "%s%06u_%03d.bmp", getenv("SSX3_VKI_DUMP"), flip,
             (int)(t * 100.0f + 0.5f));
    if ((f = fopen(path, "wb"))) {
        row_bytes = w * 3;
        pad = (4 - (row_bytes & 3)) & 3;
        filesz = 54 + (row_bytes + pad) * h;
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = h; y-- > 0;) {
            const uint8_t *row = buf.mapped + (size_t)y * w * 4;
            for (x = 0; x < w; x++)
                fwrite(row + x * 4, 1, 3, f);
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
    }
    vkUnmapMemory(V.device, buf.memory);
    vkDestroyBuffer(V.device, buf.buffer, NULL);
    vkFreeMemory(V.device, buf.memory, NULL);
}

/* Every submission retired: all released packets are free again. */
static void vki_drain(void)
{
    vkWaitForFences(V.device, VKI_INFLIGHT, I.fence, VK_TRUE, UINT64_MAX);
    pthread_mutex_lock(&I.lock);
    I.completed = I.submitted;
    pthread_mutex_unlock(&I.lock);
}

static void timed_wait(double until)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    {
        double d = until - now_s();
        long ns;
        if (d <= 0)
            return;
        if (d > 0.1)
            d = 0.1;
        ns = ts.tv_nsec + (long)(d * 1e9);
        ts.tv_sec += ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
    }
    pthread_cond_timedwait(&I.cond, &I.lock, &ts);
}

static void *presenter_main(void *unused)
{
    int dump_on = getenv("SSX3_VKI_DUMP") != NULL;

    (void)unused;
    pthread_mutex_lock(&I.lock);
    for (;;) {
        VkiPacket *next = NULL;
        int i, nready = 0, adopted = 0;
        double now = now_s(), t;

        /* The oldest frame not yet shown; take it once its time has come,
         * at once when frames pile up. */
        for (i = 0; i < VKI_PACKETS; i++)
            if (I.pk[i].state == VKI_READY) {
                nready++;
                if (!next || I.pk[i].flip < next->flip)
                    next = &I.pk[i];
            }
        if (next && (!I.cur || now >= next->t_show || nready > 2)) {
            I.holding = 0;
            if (I.prev) {
                I.prev->state = VKI_FREE;
                I.prev->release_serial = I.submitted;
            }
            I.prev = I.cur;
            I.cur = next;
            next->state = VKI_HELD;
            if (nready > 2)
                next->t_show = now;
            I.shown_final = 0;
            adopted = 1;
        }
        if (!I.cur) {
            pthread_cond_wait(&I.cond, &I.lock);
            continue;
        }
        if (adopted) {
            pthread_mutex_unlock(&I.lock);
            match_packets(I.prev, I.cur);
            I.stat_frames++;
            pthread_mutex_lock(&I.lock);
        }
        t = I.prev ? (now - I.cur->t_show) / I.cur->dur : 1.0;
        if (t >= 1.0) {
            t = 1.0;
            if (I.shown_final) {
                /* Nothing new to show until the next frame's time. A hold
                 * (visible stutter) when that frame has not even arrived. */
                if (!next && !I.holding && I.prev) {
                    I.holding = 1;
                    I.stat_hold++;
                }
                pthread_mutex_unlock(&I.lock);
                vki_drain();
                pthread_mutex_lock(&I.lock);
                timed_wait(next ? next->t_show : now + 0.1);
                continue;
            }
        }
        if (t < 0)
            t = 0;
        pthread_mutex_unlock(&I.lock);

        {
            /* Explicit cap, else the display's refresh (FIFO paces itself). */
            double hz = I.limit_env >= 0 ? I.limit_env : (double)V.display_hz;
            double d, dt = hz > 0 ? 1.0 / hz : 0;
            if (dt <= 0)
                goto no_cap;
            d = I.next_deadline - now_s();
            if (d > 0) {
                struct timespec ts;
                ts.tv_sec = (time_t)d;
                ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            }
            I.next_deadline += dt;
            if (I.next_deadline < now_s())
                I.next_deadline = now_s();
        }
no_cap:
        if (adopted && I.prev && dump_on && V.dump_every
                && I.cur->flip % V.dump_every == 0) {
            /* The same frame at blends 0, 1/2 and 1, one after the other. */
            static const float ts[3] = { 0.0f, 0.5f, 1.0f };
            for (i = 0; i < 3; i++)
                if (vki_present(ts[i]))
                    vki_dump((int)((I.submitted - 1) % VKI_INFLIGHT), I.cur->flip, ts[i]);
        }
        if (vki_present((float)t)) {
            if (t >= 1.0)
                I.shown_final = 1;
        } else {
            struct timespec ts = { 0, 20000000 };
            nanosleep(&ts, NULL);
        }
        pthread_mutex_lock(&I.lock);
    }
    return NULL;
}

/* On the NV2A thread right after Vulkan came up. Interpolation needs the
 * window thread (ssx3_vk_host_main): the presenter owns the swapchain. */
static void interp_start(void)
{
    const char *e = getenv("SSX3_INTERP");
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fci;
    VkSemaphoreCreateInfo sci;
    pthread_t th;
    int i;

    if ((e && *e == '0') || !V.host_main)
        return;
    pthread_mutex_init(&I.lock, NULL);
    pthread_cond_init(&I.cond, NULL);
    I.period = I.step = 1.0 / 60;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = V.queue_family;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VKI_INFLIGHT;
    memset(&fci, 0, sizeof fci);
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateCommandPool(V.device, &pci, NULL, &I.pool) != VK_SUCCESS)
        goto fail;
    ai.commandPool = I.pool;
    if (vkAllocateCommandBuffers(V.device, &ai, I.cmd) != VK_SUCCESS)
        goto fail;
    for (i = 0; i < VKI_INFLIGHT; i++)
        if (vkCreateFence(V.device, &fci, NULL, &I.fence[i]) != VK_SUCCESS
                || vkCreateSemaphore(V.device, &sci, NULL, &I.acquire[i]) != VK_SUCCESS
                || !ring_create(&I.ring[i], VKI_RING,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            goto fail;
    e = getenv("SSX3_FPS_LIMIT");
    I.limit_env = e ? atof(e) : -1.0;
    I.next_deadline = now_s();
    I.on = 1;
    if (pthread_create(&th, NULL, presenter_main, NULL) != 0) {
        I.on = 0;
        goto fail;
    }
    pthread_detach(th);
    if (I.limit_env > 0)
        fprintf(stderr, "  [VKI] frame interpolation on, frame cap %.0f fps\n", I.limit_env);
    else if (I.limit_env == 0)
        fprintf(stderr, "  [VKI] frame interpolation on, uncapped\n");
    else
        fprintf(stderr, "  [VKI] frame interpolation on, frame cap = display refresh\n");
    return;
fail:
    fprintf(stderr, "  [VKI] frame interpolation unavailable, presenting game frames\n");
}

/* vkb_flip with interpolation on: submit the frame without presenting and
 * hand its recording to the presenter. */
static void flip_interp(void)
{
    VkiPacket *p;
    VkSubmitInfo si;

    if (!ensure_recording())
        return;
    end_rendering();
    p = rec_finish(V.last);
    vkEndCommandBuffer(V.cmd);
    V.recording = 0;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    if (queue_submit(&si, V.frame_fence) != VK_SUCCESS) {
        fprintf(stderr, "  [VK] submit failed\n");
        if (p)
            rec_release(p);
        return;
    }
    if (p)
        rec_publish(p);
    if (V.dump_prefix && V.last && V.flips % V.dump_every == 0)
        dump_surface(V.last);
}

static void vkb_clear(const Nv2aSurface *g, const Nv2aRenderState *rs,
                      uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkbSurface *s;
    VkImageSubresourceRange range;

    if (!ensure_ready() || !ensure_recording())
        return;
    if (!(s = surface_get(g)))
        return;
    V.last = s;
    if (I.on)
        rec_clear(s, flags, argb, zstencil);
    end_rendering();
    memset(&range, 0, sizeof range);
    range.levelCount = 1;
    range.layerCount = 1;
    if (flags & 0xF0) {
        VkClearColorValue c;
        c.float32[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        c.float32[1] = (float)((argb >>  8) & 0xFF) / 255.0f;
        c.float32[2] = (float)( argb        & 0xFF) / 255.0f;
        c.float32[3] = (float)((argb >> 24) & 0xFF) / 255.0f;
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        surface_to(s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(V.cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &c, 1, &range);
    }
    if (flags & 0x3) {
        /* Z24S8 clear value: depth in the top 24 bits, stencil in the low 8. */
        VkbDepth *d = depth_get(rs ? rs->zeta_va : 0, s->img_w, s->img_h);
        VkClearDepthStencilValue dv;
        if (d) {
            dv.depth = (float)((double)(zstencil >> 8) / 16777215.0);
            dv.stencil = zstencil & 0xFF;
            range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            depth_to(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearDepthStencilImage(V.cmd, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &dv, 1, &range);
        }
    }
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

/* The batch's combiner state in this frame's ring, as nv2a.frag reads it.
 * Consecutive batches mostly share it, so an unchanged block is reused. */
static void combiners_build(const Nv2aBatchExt *e, VkbCombiners *out)
{
    const Nv2aCombiner *rc = e->rc;
    uint32_t flags = rc->control >> 8;
    VkbCombiners u;
    int i;

    memset(&u, 0, sizeof u);
    for (i = 0; i < 8; i++) {
        u.cw[i][0] = rc->color_icw[i];
        u.cw[i][1] = rc->alpha_icw[i];
        u.cw[i][2] = rc->color_ocw[i];
        u.cw[i][3] = rc->alpha_ocw[i];
        /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT). */
        unpack_argb(rc->factor0[(flags & 0x010) ? i : 0], u.c0[i]);
        unpack_argb(rc->factor1[(flags & 0x100) ? i : 0], u.c1[i]);
    }
    u.fin[0] = rc->final0;
    u.fin[1] = rc->final1;
    u.fin[2] = rc->control;
    u.fin[3] = rc->stage_program;
    unpack_argb(rc->final_c0, u.fc0);
    unpack_argb(rc->final_c1, u.fc1);
    unpack_argb(e->fog_color, u.fog_color);
    for (i = 0; i < 4; i++) {
        const Nv2aTexture *t = e->textures[i];
        /* Linear textures take texel coordinates, the rest normalised ones. */
        u.tex_scale[i][0] = (t && t->pitch && t->width) ? 1.0f / (float)t->width : 1.0f;
        u.tex_scale[i][1] = (t && t->pitch && t->height) ? 1.0f / (float)t->height : 1.0f;
    }
    u.misc[0] = e->clip_plane_mode;
    *out = u;
}

static int combiners_upload(const VkbCombiners *pu, VkDeviceSize *off)
{
    VkbCombiners u = *pu;
    void *dst;

    if (V.last_rc_off != ~(VkDeviceSize)0 && !memcmp(&u, &V.last_rc, sizeof u)) {
        *off = V.last_rc_off;
        return 1;
    }
    if (!(dst = ring_alloc(V.vring, sizeof u, V.ubo_align, off)))
        return 0;
    memcpy(dst, &u, sizeof u);
    V.last_rc = u;
    V.last_rc_off = *off;
    return 1;
}

static void vkb_draw(const Nv2aSurface *g, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    static int no_rc = -1;
    const Nv2aBatchExt *e;
    int use_rc;
    VkbSurface *s;
    VkbDepth *d;
    const Nv2aTexture *stex[4] = { NULL, NULL, NULL, NULL };
    VkImageView views[4] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorImageInfo dii[4];
    VkDescriptorBufferInfo dbi;
    VkWriteDescriptorSet w[VKB_BIND_RC + 1];
    VkbPush pc;
    VkDeviceSize voff[2], vstride[2], rc_off = 0;
    VkDeviceSize bytes = (VkDeviceSize)b->count * sizeof(Nv2aVertex);
    VkbCombiners rcu;
    int surf[4] = { -1, -1, -1, -1 };
    VkDeviceSize ebytes;
    VkBuffer vbufs[2];
    void *dst, *edst;
    int i;

    if (no_rc < 0)
        no_rc = getenv("SSX3_VK_NO_RC") != NULL;
    e = no_rc ? NULL : b->ext;
    use_rc = e && e->rc;
    ebytes = e ? (VkDeviceSize)b->count * sizeof(Nv2aVertexExt) : sizeof(Nv2aVertexExt);
    if (!b->count || !ensure_ready() || !ensure_recording() || !white_ready())
        return;
    if (!(s = surface_get(g)))
        return;
    V.last = s;
    V.frame_draws++;
    d = depth_get(rs->zeta_va, s->img_w, s->img_h);
    if (!d)
        return;

    /* Textures first: an upload or a render-to-texture transition ends the
     * rendering scope, which begin_rendering below reopens. With combiners
     * every stage samples what the extension names; without, stage 0 is the
     * batch's one texture. */
    if (use_rc)
        for (i = 0; i < 4; i++)
            stex[i] = e->textures[i];
    else
        stex[0] = b->texture;
    for (i = 0; i < 4; i++)
        if (stex[i]) {
            views[i] = texture_view(stex[i]);
            surf[i] = view_surface(views[i]);
        }
    if (use_rc)
        combiners_build(e, &rcu);

    if (!(dst = ring_alloc(V.vring, bytes, 16, &voff[0]))
            || !(edst = ring_alloc(V.vring, ebytes, 16, &voff[1]))
            || (use_rc && !combiners_upload(&rcu, &rc_off))) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [VK] vertex ring full this frame\n");
        return;
    }
    memcpy(dst, b->vertices, (size_t)bytes);
    if (e)
        memcpy(edst, e->vertices, (size_t)ebytes);
    else
        memset(edst, 0, (size_t)ebytes);

    begin_rendering(s, d);
    apply_state(V.cmd, rs);

    memset(w, 0, sizeof w);
    for (i = 0; i < 4; i++) {
        /* A stage with nothing usable reads white, as on the CPU path. */
        dii[i].sampler = sampler_get(views[i] ? stex[i] : NULL);
        dii[i].imageView = views[i] ? views[i] : V.white.view;
        dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &dii[i];
    }
    /* The UBO binding must be valid even when the shader skips it: without
     * combiners rc_off stays 0, an in-range block it never reads. */
    dbi.buffer = V.vring->buffer;
    dbi.offset = rc_off;
    dbi.range = sizeof(VkbCombiners);
    w[VKB_BIND_RC].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[VKB_BIND_RC].dstBinding = VKB_BIND_RC;
    w[VKB_BIND_RC].descriptorCount = 1;
    w[VKB_BIND_RC].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[VKB_BIND_RC].pBufferInfo = &dbi;
    vkCmdPushDescriptorSetKHR(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipe_layout,
                              0, VKB_BIND_RC + 1, w);

    pc.surface_size[0] = (float)s->width;
    pc.surface_size[1] = (float)s->height;
    pc.tex_size[0] = (!use_rc && views[0]) ? (float)b->texture->width : 1.0f;
    pc.tex_size[1] = (!use_rc && views[0]) ? (float)b->texture->height : 1.0f;
    pc.z_scale = 1.0f / 16777215.0f;
    pc.squeeze = 1.0f / s->wide;
    pc.flags = ((!use_rc && views[0]) ? 1u : 0u)
             | (use_rc ? 2u : 0u)
             | ((rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
                 ? rs->alpha_func - 0x200 : 7u) << 8)
             | ((rs->alpha_ref & 0xFF) << 16)
             | (rs->alpha_test_enable ? 0x80000000u : 0u);
    vkCmdPushConstants(V.cmd, V.pipe_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    vbufs[0] = vbufs[1] = V.vring->buffer;
    vstride[0] = sizeof(Nv2aVertex);
    vstride[1] = e ? sizeof(Nv2aVertexExt) : 0;
    vkCmdBindVertexBuffers2(V.cmd, 0, 2, vbufs, voff, NULL, vstride);
    vkCmdDraw(V.cmd, b->count, 1, 0, 0);

    if (I.on) {
        VkSampler smp[4];
        for (i = 0; i < 4; i++)
            smp[i] = dii[i].sampler;
        rec_draw(s, b, e, use_rc ? &rcu : NULL, stex, views, smp, surf, &pc, rs);
    }
}

static void scale_key(const SDL_Event *ev);

static void pump_events(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            fprintf(stderr, "  [VK] window closed\n");
            fflush(stderr);
            _Exit(0);
        }
        if (ev.type == SDL_WINDOWEVENT
                && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                    || ev.window.event == SDL_WINDOWEVENT_RESIZED))
            V.swap_dirty = 1;
        scale_key(&ev);
        xbox_InputSdlEvent(&ev);
    }
}

/* The surface scaled to fit the window with its display aspect kept; the
 * rest of the window stays black. */
static void fit_rect(const VkbSurface *s, VkOffset3D out[2])
{
    double sw = (double)V.swap_extent.width, sh = (double)V.swap_extent.height;
    double aspect = s->aspect;
    double w = sw, h = sw / aspect;

    if (h > sh) { h = sh; w = sh * aspect; }
    out[0].x = (int32_t)((sw - w) / 2); out[0].y = (int32_t)((sh - h) / 2); out[0].z = 0;
    out[1].x = out[0].x + (int32_t)w;   out[1].y = out[0].y + (int32_t)h;   out[1].z = 1;
}

/* "title | FPS: x | draws: n", once a second -- the format of the CPU
 * renderer's window (fb_present_sdl.c), so the two read the same. Wall time:
 * clock() would be CPU time on POSIX. */
static void update_title(void)
{
    uint32_t now = SDL_GetTicks();
    char tb[192];

    if (!V.title_ms) {
        V.title_ms = now;
        V.title_flips = V.flips;
        V.title_presents = I.presents;
        return;
    }
    if (now - V.title_ms < 1000)
        return;
    if (I.on)
        /* Shown frames (interpolated) and the game's own frames. */
        snprintf(tb, sizeof tb, "SSX 3 (Recomp, Vulkan) | FPS: %.1f (game %.1f) | draws: %u",
                 (double)(I.presents - V.title_presents) * 1000.0 / (double)(now - V.title_ms),
                 (double)(V.flips - V.title_flips) * 1000.0 / (double)(now - V.title_ms),
                 V.last_frame_draws);
    else
        snprintf(tb, sizeof tb, "SSX 3 (Recomp, Vulkan) | FPS: %.1f | draws: %u",
                 (double)(V.flips - V.title_flips) * 1000.0 / (double)(now - V.title_ms),
                 V.last_frame_draws);
    SDL_SetWindowTitle(V.window, tb);
    {
        /* The same figure in the log every 5 s, for runs nobody watches. */
        static uint32_t logged;
        if (now - logged >= 5000) {
            fprintf(stderr, "  [VK] %s | decodes/5s: %u (%u new, %.1f Mtexels)\n",
                    tb + sizeof "SSX 3 (Recomp, Vulkan) | " - 1, V.stat_decodes,
                    V.stat_new, (double)V.stat_decode_px / 1e6);
            if (I.on && I.stat_frames) {
                /* Racy reads of the presenter's counters: a log line only. */
                fprintf(stderr, "  [VKI] holds/5s: %u, unrecorded frames: %u, on game clock: %u,"
                        " cushion %.1f ms, step %.2f ms\n", I.stat_hold, I.stat_drop,
                        I.stat_clock, I.cushion * 1e3, I.step * 1e3);
                I.stat_hold = I.stat_drop = I.stat_clock = 0;
                fprintf(stderr, "  [VKI] blended draws/5s: %u of %u (%.0f%%), %u frames;"
                        " unpaired: %u moved too far, %u no partner, %u of them given borrowed motion\n",
                        I.stat_matched, I.stat_draws,
                        I.stat_draws ? 100.0 * I.stat_matched / I.stat_draws : 0.0,
                        I.stat_frames, I.stat_jump, I.stat_none, I.stat_synth);
                I.stat_matched = I.stat_draws = I.stat_frames = 0;
                I.stat_jump = I.stat_none = I.stat_synth = 0;
            }
            V.stat_decodes = V.stat_new = 0;
            V.stat_decode_px = 0;
            logged = now;
        }
    }
    V.title_ms = now;
    V.title_flips = V.flips;
    V.title_presents = I.presents;
}

/* At a flip: take over a new requested scale for the next frame. This
 * frame's shown surface keeps its image until it goes unused. */
static void scale_update(void)
{
    int want = R.want;

    if (want < 1) want = 1;
    if (want > VKB_MAX_SCALE) want = VKB_MAX_SCALE;
    if (want == R.scale)
        return;
    R.scale = want;
    if (!R.purge)
        R.purge = 1;
    fprintf(stderr, "  [VK] internal resolution %dx\n", R.scale);
}

/* At a flip: widescreen for the next frame -- the requested factor while the
 * title is in game (its camera hook widens the projection then), else 4:3.
 * The shown surface's guest size says which surfaces are screen-sized. */
static void wide_update(void)
{
    float want = g_ssx3_wide > 1.001f && ssx3_in_game() ? g_ssx3_wide : 1.0f;

    if (V.last) {
        R.screen_w = V.last->width;
        R.screen_h = V.last->height;
    }
    if (want == R.wide)
        return;
    R.wide = want;
    R.purge = 2;
    fprintf(stderr, "  [VK] %s (frame aspect %.3f)\n",
            want > 1.0f ? "widescreen on: in game" : "widescreen off: 4:3, pillarboxed",
            4.0 / 3.0 * want);
}

/* SSX3_ASPECT: the requested frame shape, as g_ssx3_wide = shape / (4/3).
 * "auto" is resolved by wide_auto once SDL video is up. */
static void wide_init(void)
{
    const char *e = getenv("SSX3_ASPECT");
    double a = 4.0 / 3.0, w, h;

    if (!e || !*e)
        return;
    if (!strcmp(e, "auto")) {
        R.wide_auto = 1;
        return;
    }
    if (sscanf(e, "%lf:%lf", &w, &h) == 2 && w > 0 && h > 0)
        a = w / h;
    else if (sscanf(e, "%lf", &w) == 1 && w > 0)
        a = w;
    else
        fprintf(stderr, "  [VK] SSX3_ASPECT=%s not understood (16:9, 21:9, 1.78, auto)\n", e);
    if (a < 4.0 / 3.0 + 0.001) {
        if (a < 4.0 / 3.0 - 0.001)
            fprintf(stderr, "  [VK] SSX3_ASPECT=%s is narrower than 4:3, using 4:3\n", e);
        return;
    }
    g_ssx3_wide = (float)(a * 0.75);
    fprintf(stderr, "  [VK] widescreen: %.3f:1 in game (x%.3f)\n", a, a * 0.75);
}

/* SSX3_ASPECT=auto: the desktop's shape (needs SDL video). */
static void wide_auto(void)
{
    SDL_DisplayMode m;

    if (!R.wide_auto)
        return;
    R.wide_auto = 0;
    if (SDL_GetDesktopDisplayMode(0, &m) != 0 || m.w <= 0 || m.h <= 0) {
        fprintf(stderr, "  [VK] SSX3_ASPECT=auto: no desktop mode (%s), using 4:3\n",
                SDL_GetError());
        return;
    }
    if ((double)m.w / m.h > 4.0 / 3.0 + 0.001)
        g_ssx3_wide = (float)((double)m.w / m.h * 0.75);
    fprintf(stderr, "  [VK] widescreen auto: desktop %dx%d -> x%.3f\n", m.w, m.h,
            (double)g_ssx3_wide);
}

/* The window's first size: 960 high for 4:3, 720 high and as wide as the
 * frame shape otherwise (1280x720 at 16:9, 1680x720 at 21:9). */
static void window_size(int *w, int *h)
{
    if (g_ssx3_wide > 1.001f) {
        *h = 720;
        *w = (int)(720.0 * 4.0 / 3.0 * g_ssx3_wide + 0.5);
    } else {
        *w = 1280;
        *h = 960;
    }
}

/* SSX3_RES_SCALE at start-up; F10 cycles 1x .. VKB_MAX_SCALE x (any thread
 * that pumps window events). */
static void scale_init(void)
{
    const char *e = getenv("SSX3_RES_SCALE");

    if (e && *e)
        R.want = atoi(e);
    if (R.want < 1 || R.want > VKB_MAX_SCALE) {
        if (e && *e)
            fprintf(stderr, "  [VK] SSX3_RES_SCALE=%s out of range (1..%d), using 1\n",
                    e, VKB_MAX_SCALE);
        R.want = 1;
    }
    R.scale = R.want;
}

static void scale_key(const SDL_Event *ev)
{
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat
            && ev->key.keysym.scancode == SDL_SCANCODE_F10)
        R.want = R.want % VKB_MAX_SCALE + 1;
}

static void vkb_flip(void)
{
    uint32_t idx;
    VkResult r;
    VkImage img;
    VkClearColorValue black;
    VkImageSubresourceRange range;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkbSurface *s;

    if (!ensure_ready())
        return;
    if (!V.host_main) {
        pump_events();
        update_title();
    }
    V.flips++;
    if (V.flips == 1 || (V.frame_draws && !V.first_draw_logged)) {
        /* Boot time to the first flip and the first frame with draws. */
        fprintf(stderr, "  [VK] %s at %.1f s (flip %u)\n",
                V.flips == 1 ? "first flip" : "first frame with draws",
                (double)SDL_GetTicks() / 1000.0, V.flips);
        if (V.frame_draws)
            V.first_draw_logged = 1;
    }
    V.last_frame_draws = V.frame_draws;
    V.frame_draws = 0;
    scale_update();
    wide_update();
    if (I.on) {
        flip_interp();
        return;
    }
    if (V.swap_dirty) {
        destroy_swapchain();
        if (!create_swapchain())
            return;
    }
    if (!ensure_recording())
        return;

    /* Bounded: the NV2A thread must never block on the window (a hidden or
     * unresponsive window can withhold images indefinitely). Without an image
     * the frame's work is still submitted, just not shown. */
    r = vkAcquireNextImageKHR(V.device, V.swapchain, 100000000ull /* 100 ms */,
                              V.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VkSubmitInfo none;
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
            V.swap_dirty = 1;
        end_rendering();
        vkEndCommandBuffer(V.cmd);
        V.recording = 0;
        memset(&none, 0, sizeof none);
        none.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        none.commandBufferCount = 1;
        none.pCommandBuffers = &V.cmd;
        queue_submit(&none, V.frame_fence);
        return;
    }
    img = V.swap_images[idx];
    end_rendering();

    memset(&range, 0, sizeof range);
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    memset(&black, 0, sizeof black);
    transition(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(V.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &range);
    s = V.last;
    if (s && s->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        VkImageBlit blit;
        memset(&blit, 0, sizeof blit);
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = (int32_t)s->img_w;
        blit.srcOffsets[1].y = (int32_t)s->img_h;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        fit_rect(s, blit.dstOffsets);
        surface_to(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdBlitImage(V.cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    transition(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(V.cmd);
    V.recording = 0;

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &V.sem_acquire;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &V.sem_render_done[idx];
    if (queue_submit(&si, V.frame_fence) != VK_SUCCESS) {
        fprintf(stderr, "  [VK] submit failed\n");
        return;
    }
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &V.sem_render_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &V.swapchain;
    pi.pImageIndices = &idx;
    pthread_mutex_lock(&s_qlock);
    r = vkQueuePresentKHR(V.queue, &pi);
    pthread_mutex_unlock(&s_qlock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        V.swap_dirty = 1;

    if (V.dump_prefix && s && V.flips % V.dump_every == 0)
        dump_surface(s);
}

static const Nv2aBackend s_vk_backend = { vkb_clear, vkb_draw, vkb_flip };

/* ---- the window on the process main thread ------------------------------ */

extern void xbox_InputSetMainPump(int on);    /* src/input/xinput_device.c */

/* The highest refresh rate of the connected displays: the presenter's
 * default frame cap (more frames than a display shows only take GPU time
 * from the game). The fastest display, not the window's: under Wayland SDL
 * cannot tell which display a window is on. */
static void read_display_hz(void)
{
    SDL_DisplayMode m;
    int d, n = SDL_GetNumVideoDisplays(), hz = 0;

    for (d = 0; d < n; d++)
        if (SDL_GetCurrentDisplayMode(d, &m) == 0 && m.refresh_rate > hz)
            hz = m.refresh_rate;
    if (hz > 0 && hz != V.display_hz) {
        V.display_hz = hz;
        fprintf(stderr, "  [VK] fastest display refresh %d Hz\n", hz);
    }
}

static int (*s_guest_main)(void);
static int s_guest_rc;
static Uint32 s_guest_done_event = (Uint32)-1;

static void *guest_thread(void *unused)
{
    SDL_Event ev;

    (void)unused;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

/* The guest's thread, with xbox_HostWindowMain's stack rule: at least the
 * main thread's and generously more, since lifted code recurses on it. */
static int start_guest_thread(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

int ssx3_vk_host_main(int (*guest_main)(void))
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "  [VK] SDL init: %s; running without a window thread\n",
                SDL_GetError());
        return guest_main();
    }
    int ww, wh;

    wide_auto();
    window_size(&ww, &wh);
    V.window = SDL_CreateWindow("SSX 3 (Recomp, Vulkan) | loading",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                ww, wh,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    s_guest_done_event = SDL_RegisterEvents(1);
    if (!V.window || s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [VK] window: %s; running without a window thread\n",
                SDL_GetError());
        if (V.window) {
            SDL_DestroyWindow(V.window);
            V.window = NULL;
        }
        return guest_main();
    }
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit();
    read_display_hz();
    V.host_main = 1;
    s_guest_main = guest_main;
    if (!start_guest_thread()) {
        fprintf(stderr, "  [VK] cannot start the guest thread\n");
        V.host_main = 0;
        xbox_InputSetMainPump(0);
        return guest_main();
    }
    fprintf(stderr, "  [VK] window open; guest runs on its own thread\n");

    /* Pump from the start, through the whole load, so the desktop never sees
     * an unresponsive window; refresh the FPS title while frames come. */
    for (;;) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 16)) {
            do {
                if (ev.type == s_guest_done_event)
                    return s_guest_rc;
                if (ev.type == SDL_QUIT) {
                    fprintf(stderr, "  [VK] window closed\n");
                    fflush(stderr);
                    _Exit(0);
                }
                if (ev.type == SDL_WINDOWEVENT
                        && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                            || ev.window.event == SDL_WINDOWEVENT_RESIZED))
                    V.swap_dirty = 1;
                if (ev.type == SDL_DISPLAYEVENT)
                    read_display_hz();
                scale_key(&ev);
                xbox_InputSdlEvent(&ev);
            } while (SDL_PollEvent(&ev));
        }
        if (V.flips)
            update_title();
    }
}

int ssx3_vk_register(void)
{
    scale_init();
    wide_init();
    nv2a_backend_register(&s_vk_backend);
    fprintf(stderr, "  [VK] Vulkan back end registered (starts on the first frame)\n");
    return 0;
}
