/*
 * Darwin 上的 libdrm 垫片：只提供 PanVK / Mesa 运行时实际用到的那一小部分（约 26 个符号）。
 *
 * 真正的 libdrm 在 Darwin 上没有；PanVK 又直接发 Panthor ioctl（不只是 pan_kmod），所以我们在用户态
 * 拦截 drmIoctl()，把 Panthor / DRM-syncobj 的 ioctl 转给 Mali 的 IOKit user client。
 * 这个头文件定义类型和声明；实现在 ../drm_darwin.c。
 */
#ifndef XF86DRM_DARWIN_SHIM_H
#define XF86DRM_DARWIN_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* drm.h 是 Mesa 自己带的 include/drm-uapi/drm.h；Darwin 走它的「BSD」分支（<sys/ioccom.h>）。 */
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int drm_magic_t;

/* ---- 版本 ---- */
typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

/* ---- 设备 ---- */
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI      0
#define DRM_BUS_USB      1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X   3

#define DRM_PLATFORM_DEVICE_NAME_LEN 512

typedef struct _drmPciBusInfo {
   uint16_t domain;
   uint8_t bus;
   uint8_t dev;
   uint8_t func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
   uint16_t vendor_id;
   uint16_t device_id;
   uint16_t subvendor_id;
   uint16_t subdevice_id;
   uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmUsbBusInfo {
   uint8_t bus;
   uint8_t dev;
} drmUsbBusInfo, *drmUsbBusInfoPtr;

typedef struct _drmUsbDeviceInfo {
   uint16_t vendor;
   uint16_t product;
} drmUsbDeviceInfo, *drmUsbDeviceInfoPtr;

typedef struct _drmPlatformBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible; /* NULL 结尾的 compatible 字符串列表 */
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmHost1xBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmHost1xDeviceInfo {
   char **compatible;
} drmHost1xDeviceInfo, *drmHost1xDeviceInfoPtr;

typedef struct _drmDevice {
   char **nodes;          /* DRM_NODE_MAX 个路径，没有的是 NULL */
   int available_nodes;   /* 位掩码：1 << DRM_NODE_* */
   int bustype;
   union {
      drmPciBusInfoPtr pci;
      drmUsbBusInfoPtr usb;
      drmPlatformBusInfoPtr platform;
      drmHost1xBusInfoPtr host1x;
   } businfo;
   union {
      drmPciDeviceInfoPtr pci;
      drmUsbDeviceInfoPtr usb;
      drmPlatformDeviceInfoPtr platform;
      drmHost1xDeviceInfoPtr host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

/* ---- 核心 ---- */
int drmIoctl(int fd, unsigned long request, void *arg);

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevice(drmDevicePtr *device);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);

int drmGetMagic(int fd, drm_magic_t *magic);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/* ---- GEM / PRIME ---- */
int drmCloseBufferHandle(int fd, uint32_t handle);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* ---- syncobj ---- */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
                           unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
                       uint32_t flags);

/* ---- 垫片自己的接口（不是 libdrm 的） ---- */

/* 把 IOKit 连接（IOServiceOpen 得到的 io_connect_t）包装成一个 fd，之后的 drmIoctl(fd, …) 就走它。
 * 返回 -1 表示失败（没有 Mali 服务等）。 */
int drm_darwin_open_render(void);

/* os_mmap()/os_munmap() of Mesa (util/os_mman.h) on Darwin: mappings of a DRM fd are buffer objects (IOConnectMapMemory64) or the
 * flush id page; anything else is the system's mmap/munmap. */
#include <sys/mman.h>
void *drm_darwin_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
/* A buffer object over the caller's own pages (whole 4 KiB pages), for VK_EXT_external_memory_host: 0 and the handle, or -1 and errno. */
int drm_darwin_bo_create_host(int fd, void *ptr, size_t size, uint32_t *handle);
int drm_darwin_munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* XF86DRM_DARWIN_SHIM_H */
