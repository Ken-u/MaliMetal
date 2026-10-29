// SPDX-License-Identifier: MIT
// Entry points of the native Metal frontend over the bridge (NativeBridgeMetalFrontend.mm).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
extern "C" {
// Opens the process's one device: loader exports vkGetInstanceProcAddr (Turnip), cache is a writable shader-cache directory, registryID is the
// accelerator service's registry entry. missing (may be null) is told the class and selector of every unimplemented call.
id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID, void (*missing)(const char *, const char *));
// Bare AIR bitcode as a library (the bridge's fixtures; Metal itself accepts only metallibs).
id native_bridge_metal_library_with_air(const void *bytes, size_t length);
// One line for the plugin's status report: command buffers committed/failed locally, pipelines made, refusals, the last refusal's text.
size_t native_bridge_metal_status(char *out, size_t size);
// The pixel formats of the textures asked for (first eight distinct) and how many texture requests the runtime refused.
size_t native_bridge_metal_formats(char *out, size_t size);
// Where problems the frontend notes are written (one call per line of up to 160 bytes); null for nowhere.
void native_bridge_metal_set_console(void (*console)(const char *));
// The process's first problem (a refusal or a missing call) into out, and how many problems there were.
uint64_t native_bridge_metal_first_problem(char *out, size_t size);
// Called once, on the thread that hit it, with the first problem's text (null: not called).
extern void (*native_bridge_metal_problem_hook)(const char *);
size_t native_bridge_metal_telemetry(char *out, size_t size);
// Structured cumulative artifact/driver-cache counters for offline qualification.
struct NmbShaderCacheStats;
bool native_bridge_metal_shader_cache_stats(struct NmbShaderCacheStats *out);
bool native_bridge_metal_work_stats(uint64_t out[10]);
// Where textures over buffers and surfaces cost copies: linear images at Metal's row pitch made / refused by the driver / refused again, textures
// given a shadow image, shadow copies planned, texels copied back, passes over linear images in place / through copies.
bool native_bridge_metal_texture_counts(uint64_t out[8]);
// The index'th distinct message the runtime's sink heard (its id and text, digits masked in the key; the first one's text is copied to out, its
// full length returned, 0 past the last kind) and how many came: why a texture was given a shadow image, which pass was degraded, ...
size_t native_bridge_metal_message_kind(size_t index, uint64_t *count, char *out, size_t size);
// The command buffers' timeline (boot-arg mali-timeline=1): up to five completed command buffers from *cursor on, as one line of "t age start gpu ctc
// rp.dr.kpx rep.sub.sch.ca.cb" records joined by ';' (times in units of 100 us, t on the uptime clock; the last field is where the commit's time went
// before the GPU, see native_bridge_metal_timeline in the .mm); advances *cursor, returns the length, 0 when nothing newer.
size_t native_bridge_metal_timeline(uint64_t *cursor, char *out, size_t size);
// The same fixed CPU, memory and pipeline workload on the tablet and the host, so their times compare directly (int-145): compute, first-touch
// page faults, memset bandwidth, CommonCrypto SHA-256, then the first `pairs` vertex/fragment pairs of the given QuartzCore metallib made into
// BGRA8 render states and drawn once each (pipeline_timing_host's draw=1 order). The results become the telemetry's Y rows; also returned.
size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size);
// Lets go of every held IOSurface import whose surface only the import still retains (also done at each command buffer's completion).
void native_bridge_metal_trim_surface_imports(void);
}
#endif
