/* Linux 的 <sys/sysinfo.h> 在 Darwin 上不存在；PanVK 只用它读总内存（panvk_physical_device.c 里的 sysinfo()）。
 * 只提供它用到的字段，实现在 ../drm_darwin.c（用 sysctl hw.memsize）。 */
#ifndef SYS_SYSINFO_DARWIN_SHIM_H
#define SYS_SYSINFO_DARWIN_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
   unsigned long totalram;
   unsigned long freeram;
   unsigned long sharedram;
   unsigned long bufferram;
   unsigned long totalswap;
   unsigned long freeswap;
   unsigned int mem_unit;
};

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif

#endif
