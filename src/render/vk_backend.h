/*
 * Vulkan render back end for the NV2A pushbuffer executor.
 *
 * Replaces the executor's CPU rasteriser: the runtime still decodes the
 * pushbuffer and transforms vertices, this draws the result on the GPU and
 * presents it in its own window. Design and milestones: docs/vulkan-backend.md.
 */
#ifndef SSX3_VK_BACKEND_H
#define SSX3_VK_BACKEND_H

/* Register the back end with the executor (nv2a_backend_register). Call once
 * before the guest starts; Vulkan and the window come up lazily on the NV2A
 * poll thread at the first clear/draw/flip. Returns 0 on success. */
int ssx3_vk_register(void);

/* Host entry point when the Vulkan renderer is selected (POSIX: instead of
 * xbox_HostWindowMain; Windows: from WinMain): opens the window on the
 * calling (main) thread, runs guest_main on its own thread and pumps window
 * events from the start, so the window stays responsive through the load.
 * Returns guest_main's result. */
int ssx3_vk_host_main(int (*guest_main)(void));

/* Non-zero when a Vulkan loader (libvulkan / vulkan-1.dll) is installed.
 * Cheap to call more than once. */
int ssx3_vk_available(void);

/* The monitors, in the order SSX3_MONITOR numbers them (SDL / EnumDisplay-
 * Monitors). Returns how many were written to out (at most max). */
typedef struct {
    char name[96];
    int  w, h, hz;
    int  primary;
} Ssx3Display;
int ssx3_vk_displays(Ssx3Display *out, int max);

/* `--list-displays`: print the monitors (SDL display number, name, desktop
 * mode, refresh, primary flag; tab-separated) for the start-up menus. */
int ssx3_vk_list_displays(void);

#endif /* SSX3_VK_BACKEND_H */
