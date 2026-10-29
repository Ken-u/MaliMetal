/*
 * libdrm stand-in for Darwin / iOS: PanVK talks to the Mali-G610 through it.
 *
 * PanVK issues Panthor ioctls (drmIoctl on a DRM fd), uses DRM syncobjs through libdrm (drmSyncobj*) and mmap()s buffer objects through the DRM
 * fd. None of that exists here. What exists is the IOKit service MaliService (package/Mali), whose user client takes Panthor's uAPI
 * apart into calls that carry no user pointers (mali_uapi.h). This file is the other half:
 *
 *   - the "DRM fd" is a descriptor of /dev/null; every fd is served by ONE IOKit connection per process (PanVK keeps several fds, which are
 *     dups of one another or opens of the same node: handles are the connection's anyway);
 *   - drmIoctl() decodes the Panthor ioctl and walks its arrays (VM_BIND ops, GROUP_CREATE queues, GROUP_SUBMIT queue submits and syncs)
 *     itself, one IOKit call per element;
 *   - mmap of a buffer object is IOConnectMapMemory64 (os_mmap / os_munmap of Mesa are redirected here, see mesa-patches); the page behind
 *     DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET is a zero page: a latest_flush of 0 makes every job flush the caches, which is what a GPU
 *     that is not cache coherent with the CPU needs anyway;
 *   - DRM syncobjs (binary and timeline) live in this process: a syncobj holds fences, a fence is "the job with this number on this queue
 *     of this group" and is signaled when the group's syncobj page (mapped read-only from the kernel) has reached the number. GROUP_SUBMIT
 *     waits for its wait syncs on the CPU before it hands the job to the kernel and attaches the fence of the job to its signal syncs.
 *
 * Not supported (the Vulkan driver does not need them for what runs here): PRIME / dma-buf, sync files, sharing a syncobj with another process.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#include "drm-uapi/panthor_drm.h"
#include "mali_uapi.h"
#include "sys/sysinfo.h"
#include "xf86drm.h"

#include <stdio.h>
#define SHIM_LOG(...) do { if (g_trace) fprintf(stderr, "drm_darwin: " __VA_ARGS__); } while (0)

static int g_trace;   /* DRM_DARWIN_TRACE=1 in the environment */

/* ======================================================================================================== the connection */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;   /* the tables below; never held across an IOKit call or a wait */
static io_connect_t g_conn;
static struct mali_dev_info g_info;
static dev_t g_null_dev;
static ino_t g_null_ino;

static int kr_errno(kern_return_t kr)
{
   switch (kr) {
   case KERN_SUCCESS: return 0;
   case kIOReturnBadArgument: return EINVAL;
   case kIOReturnNoMemory:
   case kIOReturnNoResources: return ENOMEM;
   case kIOReturnNoSpace: return ENOSPC;
   case kIOReturnBusy: return EBUSY;
   case kIOReturnNotFound: return ENOENT;
   case kIOReturnTimeout: return ETIMEDOUT;
   case kIOReturnNotPermitted:
   case kIOReturnNotPrivileged: return EPERM;
   default: return EIO;
   }
}

/* Where the time goes in the calls into the driver (a poster extension has a few seconds to check in, and its main thread spent them in here):
 * count, total and longest, per selector, and for the mapping of a buffer object into the process and its unmapping. drm_darwin_timing() formats
 * them for the plugin, which logs them once a second. */
struct call_stat { uint64_t n, ns, max_ns; };
#define STAT_MAP MALI_SELECTOR_COUNT
#define STAT_UNMAP (MALI_SELECTOR_COUNT + 1)
static struct call_stat g_call_stat[MALI_SELECTOR_COUNT + 2];

static uint64_t stat_now(void)
{
   return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void stat_add(unsigned which, uint64_t began)
{
   const uint64_t took = stat_now() - began;
   struct call_stat *c = &g_call_stat[which];
   __atomic_fetch_add(&c->n, 1, __ATOMIC_RELAXED);
   __atomic_fetch_add(&c->ns, took, __ATOMIC_RELAXED);
   if (took > c->max_ns)
      c->max_ns = took;
}

size_t drm_darwin_timing(char *out, size_t cap)
{
   size_t used = 0;
   out[0] = 0;
   for (unsigned i = 0; i < MALI_SELECTOR_COUNT + 2; i++) {
      const struct call_stat *c = &g_call_stat[i];
      if (!c->n || used + 40 >= cap)
         continue;
      const char *name = i == STAT_MAP ? "map" : i == STAT_UNMAP ? "unmap" : NULL;
      if (name)
         used += (size_t)snprintf(out + used, cap - used, " %s:%llux %llu/%llums", name, (unsigned long long)c->n, (unsigned long long)(c->ns / 1000000),
                                  (unsigned long long)(c->max_ns / 1000000));
      else
         used += (size_t)snprintf(out + used, cap - used, " %u:%llux %llu/%llums", i, (unsigned long long)c->n, (unsigned long long)(c->ns / 1000000),
                                  (unsigned long long)(c->max_ns / 1000000));
   }
   return used;
}

static kern_return_t kcall(unsigned selector, const void *in, size_t in_size, void *out, size_t out_size)
{
   size_t out_len = out_size;
   const uint64_t began = stat_now();
   kern_return_t kr = IOConnectCallStructMethod(g_conn, selector, in, in_size, out, out ? &out_len : NULL);
   if (selector < MALI_SELECTOR_COUNT && selector != MALI_LOG)
      stat_add(selector, began);
   return kr;
}

static void *kmap(uint32_t type, size_t *size)
{
   mach_vm_address_t addr = 0;
   mach_vm_size_t len = 0;
   const uint64_t began = stat_now();
   kern_return_t kr = IOConnectMapMemory64(g_conn, type, mach_task_self(), &addr, &len, kIOMapAnywhere | kIOMapWriteCombineCache);
   stat_add(STAT_MAP, began);
   if (kr != KERN_SUCCESS) {
      SHIM_LOG("IOConnectMapMemory64(type %#x) failed: %#x\n", type, kr);
      return NULL;
   }
   if (size)
      *size = (size_t)len;
   return (void *)(uintptr_t)addr;
}

/* One connection per process, made when the first DRM fd is opened. */
static int connect_once(void)
{
   int ok = 0;
   pthread_mutex_lock(&g_lock);
   if (g_conn) {
      ok = 1;
      goto out;
   }
   const char *trace = getenv("DRM_DARWIN_TRACE");
   g_trace = trace && trace[0] == '1';
   io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(MALI_SERVICE));
   if (!svc) {
      SHIM_LOG("no %s service\n", MALI_SERVICE);
      goto out;
   }
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
   IOObjectRelease(svc);
   if (kr != KERN_SUCCESS) {
      SHIM_LOG("IOServiceOpen failed: %#x\n", kr);
      g_conn = 0;
      goto out;
   }
   kr = kcall(MALI_DEV_QUERY, NULL, 0, &g_info, sizeof g_info);
   if (kr != KERN_SUCCESS) {
      SHIM_LOG("DEV_QUERY failed: %#x\n", kr);
      IOServiceClose(g_conn);
      g_conn = 0;
      goto out;
   }
   struct stat st;
   if (stat("/dev/null", &st) == 0) {
      g_null_dev = st.st_dev;
      g_null_ino = st.st_ino;
   }
   ok = 1;
out:
   pthread_mutex_unlock(&g_lock);
   return ok;
}

int drm_darwin_open_render(void)
{
   if (!connect_once()) {
      errno = ENODEV;
      return -1;
   }
   return open("/dev/null", O_RDWR | O_CLOEXEC);
}

static int is_drm_fd(int fd)
{
   struct stat st;
   return fd >= 0 && fstat(fd, &st) == 0 && g_null_ino && st.st_dev == g_null_dev && st.st_ino == g_null_ino;
}

/* ======================================================================================================== time */

static int64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void nap(unsigned *spins)
{
   if (*spins < 2000) {   /* a job is usually done within tens of microseconds */
      (*spins)++;
      return;
   }
   usleep(100);
}

/* ======================================================================================================== groups and fences */

#define MAX_GROUPS 16
#define MAX_LEAVES 16

struct group {
   int used;
   uint32_t id;           /* the kernel's group id */
   uint32_t gen;          /* tells a reused id from the group a fence was made for */
   uint32_t queues;
   volatile uint64_t *sync;   /* the kernel's syncobj page: queue q's finished job count is sync[2 * q] */
   uint64_t last[MALI_MAX_QUEUES];   /* the number of the last job given to queue q (0: none) */
};
static struct group g_groups[MAX_GROUPS];
static uint32_t g_group_gen;

struct leaf {
   uint32_t group_id, gen, queue;
   uint64_t seqno;
};
/* signaled when every leaf is; no leaf: signaled */
struct fence {
   uint8_t n;
   struct leaf l[MAX_LEAVES];
};

static struct group *group_by_id(uint32_t id)
{
   for (int i = 0; i < MAX_GROUPS; i++)
      if (g_groups[i].used && g_groups[i].id == id)
         return &g_groups[i];
   return NULL;
}

static int leaf_signaled(const struct leaf *l)
{
   struct group *g = group_by_id(l->group_id);
   if (!g || g->gen != l->gen)
      return 1;   /* the group is gone: its jobs are not coming back */
   return atomic_load_explicit((_Atomic uint64_t *)&g->sync[2 * l->queue], memory_order_acquire) >= l->seqno;
}

static int fence_signaled(const struct fence *f)
{
   for (unsigned i = 0; i < f->n; i++)
      if (!leaf_signaled(&f->l[i]))
         return 0;
   return 1;
}

static void fence_prune(struct fence *f)
{
   unsigned out = 0;
   for (unsigned i = 0; i < f->n; i++)
      if (!leaf_signaled(&f->l[i]))
         f->l[out++] = f->l[i];
   f->n = (uint8_t)out;
}

/* a := a AND b */
static int fence_merge(struct fence *a, const struct fence *b)
{
   for (unsigned i = 0; i < b->n; i++) {
      unsigned j = 0;
      for (; j < a->n; j++) {
         if (a->l[j].group_id == b->l[i].group_id && a->l[j].gen == b->l[i].gen && a->l[j].queue == b->l[i].queue) {
            if (b->l[i].seqno > a->l[j].seqno)
               a->l[j].seqno = b->l[i].seqno;
            break;
         }
      }
      if (j == a->n) {
         if (a->n == MAX_LEAVES) {
            fence_prune(a);
            if (a->n == MAX_LEAVES)
               return -1;
         }
         a->l[a->n++] = b->l[i];
      }
   }
   return 0;
}

/* ======================================================================================================== syncobjs */

struct point {
   uint64_t value;
   struct fence f;
};
struct syncobj {
   int used;
   int has_binary;
   struct fence binary;
   uint64_t completed;       /* the timeline points up to this one are done */
   struct point *points;     /* pending ones, ascending */
   unsigned npoints, cap;
};
static struct syncobj *g_syncobjs;   /* handle - 1 */
static unsigned g_nsyncobjs;

static struct syncobj *syncobj_get(uint32_t handle)
{
   return (handle && handle <= g_nsyncobjs && g_syncobjs[handle - 1].used) ? &g_syncobjs[handle - 1] : NULL;
}

/* timeline points are done in order: a point whose job is done still waits for the earlier ones (a chain of dma fences) */
static void syncobj_fold(struct syncobj *so)
{
   unsigned done = 0;
   while (done < so->npoints && fence_signaled(&so->points[done].f)) {
      if (so->points[done].value > so->completed)
         so->completed = so->points[done].value;
      done++;
   }
   if (done) {
      memmove(so->points, so->points + done, (so->npoints - done) * sizeof(struct point));
      so->npoints -= done;
   }
}

/* The fence a wait or a transfer sees for `point`. Locked. Returns 0 and fills *out, or -ENOENT when there is none yet. */
static int syncobj_find(struct syncobj *so, uint64_t point, struct fence *out)
{
   syncobj_fold(so);
   memset(out, 0, sizeof *out);
   if (point) {
      if (so->completed >= point)
         return 0;
      for (unsigned i = 0; i < so->npoints; i++) {
         if (so->points[i].value >= point) {
            for (unsigned j = 0; j <= i; j++)   /* the point, and everything before it */
               if (fence_merge(out, &so->points[j].f) < 0)
                  return -ENOMEM;
            return 0;
         }
      }
      return -ENOENT;
   }
   /* no point: a binary fence, else everything a timeline has pending (what a dma fence chain's head stands for) */
   if (so->has_binary) {
      *out = so->binary;
      return 0;
   }
   if (so->npoints || so->completed) {
      for (unsigned j = 0; j < so->npoints; j++)
         if (fence_merge(out, &so->points[j].f) < 0)
            return -ENOMEM;
      return 0;
   }
   return -ENOENT;
}

static int syncobj_attach(struct syncobj *so, uint64_t point, const struct fence *f)
{
   if (!point) {
      so->binary = *f;
      so->has_binary = 1;
      return 0;
   }
   if (so->npoints == so->cap) {
      unsigned cap = so->cap ? so->cap * 2 : 8;
      struct point *p = realloc(so->points, cap * sizeof *p);
      if (!p)
         return -ENOMEM;
      so->points = p;
      so->cap = cap;
   }
   unsigned at = so->npoints;
   while (at > 0 && so->points[at - 1].value > point)
      at--;
   memmove(so->points + at + 1, so->points + at, (so->npoints - at) * sizeof(struct point));
   so->points[at].value = point;
   so->points[at].f = *f;
   so->npoints++;
   syncobj_fold(so);
   return 0;
}

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   (void)fd;
   pthread_mutex_lock(&g_lock);
   unsigned i = 0;
   while (i < g_nsyncobjs && g_syncobjs[i].used)
      i++;
   if (i == g_nsyncobjs) {
      unsigned n = g_nsyncobjs ? g_nsyncobjs * 2 : 64;
      struct syncobj *p = realloc(g_syncobjs, n * sizeof *p);
      if (!p) {
         pthread_mutex_unlock(&g_lock);
         return -ENOMEM;
      }
      memset(p + g_nsyncobjs, 0, (n - g_nsyncobjs) * sizeof *p);
      g_syncobjs = p;
      g_nsyncobjs = n;
   }
   struct syncobj *so = &g_syncobjs[i];
   memset(so, 0, sizeof *so);
   so->used = 1;
   if (flags & DRM_SYNCOBJ_CREATE_SIGNALED)
      so->has_binary = 1;   /* a fence with no leaves: signaled */
   *handle = i + 1;
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int drmSyncobjDestroy(int fd, uint32_t handle)
{
   (void)fd;
   pthread_mutex_lock(&g_lock);
   struct syncobj *so = syncobj_get(handle);
   int ret = 0;
   if (so) {
      free(so->points);
      memset(so, 0, sizeof *so);
   } else {
      ret = -EINVAL;
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
   (void)fd;
   int ret = 0;
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; i < count; i++) {
      struct syncobj *so = syncobj_get(handles[i]);
      if (!so) {
         ret = -EINVAL;
         break;
      }
      so->has_binary = 0;
      so->npoints = 0;
      so->completed = 0;
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
   (void)fd;
   int ret = 0;
   struct fence done;
   memset(&done, 0, sizeof done);
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; i < count && !ret; i++) {
      struct syncobj *so = syncobj_get(handles[i]);
      ret = so ? syncobj_attach(so, 0, &done) : -EINVAL;
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
   (void)fd;
   int ret = 0;
   struct fence done;
   memset(&done, 0, sizeof done);
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; i < count && !ret; i++) {
      struct syncobj *so = syncobj_get(handles[i]);
      ret = so ? syncobj_attach(so, points ? points[i] : 0, &done) : -EINVAL;
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
   (void)fd;
   int ret = 0;
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; i < count; i++) {
      struct syncobj *so = syncobj_get(handles[i]);
      if (!so) {
         ret = -EINVAL;
         break;
      }
      syncobj_fold(so);
      /* DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED: the last point given to the syncobj, done or not */
      points[i] = (flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) && so->npoints ? so->points[so->npoints - 1].value : so->completed;
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
   return drmSyncobjQuery2(fd, handles, points, count, 0);
}

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   (void)fd;
   (void)flags;
   int ret;
   pthread_mutex_lock(&g_lock);
   struct syncobj *src = syncobj_get(src_handle), *dst = syncobj_get(dst_handle);
   struct fence f;
   if (!src || !dst) {
      ret = -ENOENT;
   } else if ((ret = syncobj_find(src, src_point, &f)) == 0) {
      ret = syncobj_attach(dst, dst_point, &f);
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

/* Wait for all (or any) of the handles. points may be NULL (binary). abs_ns: absolute CLOCK_MONOTONIC time, INT64_MAX = forever. */
static int syncobj_wait(const uint32_t *handles, const uint64_t *points, unsigned count, int64_t abs_ns, unsigned flags, uint32_t *first)
{
   if (!count)
      return -EINVAL;
   unsigned spins = 0;
   for (;;) {
      unsigned ready = 0;
      int first_ready = -1, missing = 0;
      pthread_mutex_lock(&g_lock);
      for (unsigned i = 0; i < count; i++) {
         struct syncobj *so = syncobj_get(handles[i]);
         struct fence f;
         if (!so) {
            pthread_mutex_unlock(&g_lock);
            return -EINVAL;
         }
         if (syncobj_find(so, points ? points[i] : 0, &f) != 0) {
            missing++;   /* nothing attached (yet) */
            continue;
         }
         if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) || fence_signaled(&f)) {
            ready++;
            if (first_ready < 0)
               first_ready = (int)i;
         }
      }
      pthread_mutex_unlock(&g_lock);
      const int all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
      if (all ? ready == count : ready > 0) {
         if (first)
            *first = all ? 0 : (uint32_t)first_ready;
         return 0;
      }
      if (missing && !(flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)))
         return -EINVAL;   /* waiting for a fence that was never submitted, and not told to wait for it */
      if (abs_ns != INT64_MAX && now_ns() >= abs_ns)
         return -ETIME;
      nap(&spins);
   }
}

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   (void)fd;
   return syncobj_wait(handles, NULL, num_handles, timeout_nsec, flags, first_signaled);
}

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                           uint32_t *first_signaled)
{
   (void)fd;
   return syncobj_wait(handles, points, num_handles, timeout_nsec, flags, first_signaled);
}

/* what is not supported: a syncobj as a file descriptor, shared with another process or made from a sync file */
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
   (void)fd; (void)handle; (void)obj_fd;
   return -ENOSYS;
}
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
   (void)fd; (void)obj_fd; (void)handle;
   return -ENOSYS;
}
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   return -ENOSYS;
}
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)fd; (void)handle; (void)sync_file_fd;
   return -ENOSYS;
}

/* ======================================================================================================== buffer objects over host pages */

/* VK_EXT_external_memory_host: BO_CREATE_HOST wires the caller's pages and makes a buffer object of them. The CPU side is the caller's own
 * mapping, so "mapping" such a BO gives the pointer back (and unmapping it must leave the caller's memory alone): the table below says which
 * handles are host BOs. */
#define MAX_HOST_BOS 4096
static struct {
   void *ptr;
   size_t size;
} g_host_bo[MAX_HOST_BOS];

int drm_darwin_bo_create_host(int fd, void *ptr, size_t size, uint32_t *handle)
{
   (void)fd;
   struct mali_bo_create_host_in in = {(uint64_t)(uintptr_t)ptr, size, 0, 0};
   struct mali_bo_create_out out = {0};
   kern_return_t kr = kcall(MALI_BO_CREATE_HOST, &in, sizeof in, &out, sizeof out);
   if (kr != KERN_SUCCESS) {
      errno = kr_errno(kr);
      return -1;
   }
   if (out.handle == 0 || out.handle > MAX_HOST_BOS) {
      struct mali_bo_destroy_in d = {out.handle, 0};
      (void)kcall(MALI_BO_DESTROY, &d, sizeof d, NULL, 0);
      errno = ENOMEM;
      return -1;
   }
   pthread_mutex_lock(&g_lock);
   g_host_bo[out.handle - 1].ptr = ptr;
   g_host_bo[out.handle - 1].size = size;
   pthread_mutex_unlock(&g_lock);
   *handle = out.handle;
   return 0;
}

static int host_bo_lookup(uint32_t handle, void **ptr, size_t *size)
{
   int found = 0;
   if (handle == 0 || handle > MAX_HOST_BOS)
      return 0;
   pthread_mutex_lock(&g_lock);
   if (g_host_bo[handle - 1].ptr) {
      *ptr = g_host_bo[handle - 1].ptr;
      *size = g_host_bo[handle - 1].size;
      found = 1;
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

/* ======================================================================================================== buffer object mappings */

struct mapping {
   void *addr;
   size_t size;
   uint32_t type;
   int used;
};
#define MAX_MAPPINGS 8192
static struct mapping *g_mappings;
static unsigned g_nmappings;

static int mapping_add(void *addr, size_t size, uint32_t type)
{
   pthread_mutex_lock(&g_lock);
   if (!g_mappings) {
      g_mappings = calloc(MAX_MAPPINGS, sizeof *g_mappings);
      if (!g_mappings) {
         pthread_mutex_unlock(&g_lock);
         return -1;
      }
   }
   unsigned i = 0;
   while (i < g_nmappings && g_mappings[i].used)
      i++;
   if (i == MAX_MAPPINGS) {
      pthread_mutex_unlock(&g_lock);
      return -1;
   }
   if (i == g_nmappings)
      g_nmappings++;
   g_mappings[i] = (struct mapping){addr, size, type, 1};
   pthread_mutex_unlock(&g_lock);
   return 0;
}

#define MAPPING_HOST 0xffffffffu   /* mapping record of a host BO: nothing to unmap */
#define USER_MMIO_OFFSET_64 (1ull << 56)
#define USER_MMIO_OFFSET_32 (1ull << 43)

/* os_mmap of Mesa. A buffer object: the offset is what BO_MMAP_OFFSET returned (handle << 12); the flush id page: a page of zeroes. */
void *drm_darwin_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   if (!is_drm_fd(fd))
      return mmap(addr, length, prot, flags, fd, offset);
   if ((uint64_t)offset >= USER_MMIO_OFFSET_32) {   /* DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET and the other user MMIO pages */
      void *page = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (page != MAP_FAILED && !(prot & PROT_WRITE))
         mprotect(page, length, PROT_READ);
      return page;
   }
   if (flags & MAP_FIXED) {   /* placed mappings (VK_EXT_map_memory_placed) are not offered */
      errno = ENOTSUP;
      return MAP_FAILED;
   }
   const uint32_t handle = (uint32_t)((uint64_t)offset >> 12);
   size_t size = 0;
   {
      void *host = NULL;
      size_t host_size = 0;
      if (host_bo_lookup(handle, &host, &host_size)) {
         if (host_size < length || mapping_add(host, host_size, MAPPING_HOST) != 0) {
            errno = ENOMEM;
            return MAP_FAILED;
         }
         return host;
      }
   }
   void *p = kmap(handle, &size);
   if (!p || size < length) {
      if (p)
         IOConnectUnmapMemory64(g_conn, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)p);
      errno = ENOMEM;
      return MAP_FAILED;
   }
   if (mapping_add(p, size, handle) != 0) {
      IOConnectUnmapMemory64(g_conn, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)p);
      errno = ENOMEM;
      return MAP_FAILED;
   }
   return p;
}

int drm_darwin_munmap(void *addr, size_t length)
{
   uint32_t type = 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   for (unsigned i = 0; i < g_nmappings; i++) {
      if (g_mappings[i].used && g_mappings[i].addr == addr) {
         type = g_mappings[i].type;
         g_mappings[i].used = 0;
         found = 1;
         break;
      }
   }
   pthread_mutex_unlock(&g_lock);
   if (!found)
      return munmap(addr, length);
   if (type == MAPPING_HOST)
      return 0;
   const uint64_t began = stat_now();
   const kern_return_t kr = IOConnectUnmapMemory64(g_conn, type, mach_task_self(), (mach_vm_address_t)(uintptr_t)addr);
   stat_add(STAT_UNMAP, began);
   return kr == KERN_SUCCESS ? 0 : -1;
}

/* ======================================================================================================== ioctls */

#define ARRAY_ELEMENT(dst, arr, i)                                                           \
   do {                                                                                      \
      memset(&(dst), 0, sizeof(dst));                                                        \
      memcpy(&(dst), (const char *)(uintptr_t)(arr).array + (size_t)(i) * (arr).stride,      \
             (arr).stride < sizeof(dst) ? (arr).stride : sizeof(dst));                       \
   } while (0)

/* The wait syncs of one operation: block on the CPU until each is done. */
static int process_waits(const struct drm_panthor_obj_array *syncs)
{
   for (uint32_t i = 0; i < syncs->count; i++) {
      struct drm_panthor_sync_op op;
      ARRAY_ELEMENT(op, *syncs, i);
      if (op.flags & DRM_PANTHOR_SYNC_OP_SIGNAL)
         continue;
      const uint64_t point = (op.flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK) == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ ? op.timeline_value : 0;
      int ret = syncobj_wait(&op.handle, &point, 1, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
      if (ret < 0)
         return ret;
   }
   return 0;
}

/* The signal syncs: the fence becomes the syncobj's (or the point's). */
static int process_signals(const struct drm_panthor_obj_array *syncs, const struct fence *f)
{
   int ret = 0;
   pthread_mutex_lock(&g_lock);
   for (uint32_t i = 0; i < syncs->count && !ret; i++) {
      struct drm_panthor_sync_op op;
      ARRAY_ELEMENT(op, *syncs, i);
      if (!(op.flags & DRM_PANTHOR_SYNC_OP_SIGNAL))
         continue;
      struct syncobj *so = syncobj_get(op.handle);
      if (!so) {
         ret = -EINVAL;
         break;
      }
      const uint64_t point = (op.flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK) == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ ? op.timeline_value : 0;
      ret = syncobj_attach(so, point, f);
   }
   pthread_mutex_unlock(&g_lock);
   return ret;
}

static int ioctl_dev_query(struct drm_panthor_dev_query *q)
{
   union {
      struct drm_panthor_gpu_info gpu;
      struct drm_panthor_csif_info csif;
      struct drm_panthor_timestamp_info ts;
      struct drm_panthor_group_priorities_info prio;
   } u;
   size_t size;
   memset(&u, 0, sizeof u);
   switch (q->type) {
   case DRM_PANTHOR_DEV_QUERY_GPU_INFO:
      u.gpu.gpu_id = g_info.gpu_id;
      u.gpu.gpu_rev = g_info.gpu_rev;
      u.gpu.csf_id = g_info.csf_id;
      u.gpu.l2_features = g_info.l2_features;
      u.gpu.tiler_features = g_info.tiler_features;
      u.gpu.mem_features = g_info.mem_features;
      u.gpu.mmu_features = g_info.mmu_features;
      u.gpu.thread_features = g_info.thread_features;
      u.gpu.max_threads = g_info.max_threads;
      u.gpu.thread_max_workgroup_size = g_info.thread_max_workgroup_size;
      u.gpu.thread_max_barrier_size = g_info.thread_max_barrier_size;
      u.gpu.coherency_features = g_info.coherency_features;
      memcpy(u.gpu.texture_features, g_info.texture_features, sizeof u.gpu.texture_features);
      u.gpu.as_present = g_info.as_present;
      u.gpu.selected_coherency = DRM_PANTHOR_GPU_COHERENCY_NONE;   /* the CPU and this GPU do not share caches */
      u.gpu.shader_present = g_info.shader_present;
      u.gpu.l2_present = g_info.l2_present;
      u.gpu.tiler_present = g_info.tiler_present;
      u.gpu.core_features = g_info.core_features;
      size = sizeof u.gpu;
      break;
   case DRM_PANTHOR_DEV_QUERY_CSIF_INFO:
      u.csif.csg_slot_count = g_info.csg_slot_count;
      u.csif.cs_slot_count = g_info.cs_slot_count;
      u.csif.cs_reg_count = g_info.cs_reg_count;
      u.csif.scoreboard_slot_count = g_info.scoreboard_slot_count;
      u.csif.unpreserved_cs_reg_count = g_info.unpreserved_cs_reg_count;
      size = sizeof u.csif;
      break;
   case DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO: {
      uint64_t cnt;
      __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(cnt));
      u.ts.timestamp_frequency = g_info.timestamp_frequency;
      u.ts.current_timestamp = cnt;
      size = sizeof u.ts;
      break;
   }
   case DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO:
      u.prio.allowed_mask = (1u << PANTHOR_GROUP_PRIORITY_LOW) | (1u << PANTHOR_GROUP_PRIORITY_MEDIUM);
      size = sizeof u.prio;
      break;
   default:
      return -EINVAL;
   }
   if (q->pointer) {
      memcpy((void *)(uintptr_t)q->pointer, &u, q->size < size ? q->size : size);
   }
   q->size = (uint32_t)size;
   return 0;
}

static int ioctl_vm_bind(struct drm_panthor_vm_bind *b)
{
   for (uint32_t i = 0; i < b->ops.count; i++) {
      struct drm_panthor_vm_bind_op op;
      ARRAY_ELEMENT(op, b->ops, i);
      int ret = process_waits(&op.syncs);
      if (ret < 0)
         return ret;
      const uint32_t type = op.flags & DRM_PANTHOR_VM_BIND_OP_TYPE_MASK;
      if (type == DRM_PANTHOR_VM_BIND_OP_TYPE_MAP) {
         struct mali_vm_bind_in in = {b->vm_id, MALI_BIND_MAP, op.bo_handle, op.flags & 7u, op.bo_offset, op.va, op.size};
         kern_return_t kr = kcall(MALI_VM_BIND, &in, sizeof in, NULL, 0);
         if (kr != KERN_SUCCESS)
            return -kr_errno(kr);
      } else if (type == DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP) {
         struct mali_vm_bind_in in = {b->vm_id, MALI_BIND_UNMAP, 0, 0, 0, op.va, op.size};
         kern_return_t kr = kcall(MALI_VM_BIND, &in, sizeof in, NULL, 0);
         if (kr != KERN_SUCCESS)
            return -kr_errno(kr);
      } else if (type != DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY) {
         return -EINVAL;
      }
      struct fence done;   /* the bind is done: the kernel does it before it returns */
      memset(&done, 0, sizeof done);
      ret = process_signals(&op.syncs, &done);
      if (ret < 0)
         return ret;
   }
   return 0;
}

static int ioctl_group_create(struct drm_panthor_group_create *gc)
{
   if (gc->queues.count == 0 || gc->queues.count > MALI_MAX_QUEUES)
      return -EINVAL;
   struct mali_group_create_in in;
   memset(&in, 0, sizeof in);
   in.vm = gc->vm_id;
   in.queue_count = gc->queues.count;
   for (uint32_t q = 0; q < gc->queues.count; q++) {
      struct drm_panthor_queue_create qc;
      ARRAY_ELEMENT(qc, gc->queues, q);
      in.ring_size[q] = qc.ringbuf_size;
      in.priority[q] = qc.priority;
   }
   in.compute_core_mask = gc->compute_core_mask;
   in.fragment_core_mask = gc->fragment_core_mask;
   in.tiler_core_mask = gc->tiler_core_mask;
   in.max_compute_cores = gc->max_compute_cores;
   in.max_fragment_cores = gc->max_fragment_cores;
   in.max_tiler_cores = gc->max_tiler_cores;
   in.group_priority = (uint8_t)(3 + 4 * (gc->priority > 3 ? 3 : gc->priority));   /* LOW 3, MEDIUM 7, HIGH 11, REALTIME 15 of the firmware's 0..15 */
   struct mali_group_create_out out = {0};
   kern_return_t kr = kcall(MALI_GROUP_CREATE, &in, sizeof in, &out, sizeof out);
   if (kr != KERN_SUCCESS)
      return -kr_errno(kr);
   size_t len = 0;
   volatile uint64_t *sync = kmap(MALI_MAP_SYNCOBJ | out.group, &len);
   pthread_mutex_lock(&g_lock);
   struct group *g = NULL;
   for (int i = 0; i < MAX_GROUPS && !g; i++)
      if (!g_groups[i].used)
         g = &g_groups[i];
   if (!sync || !g) {
      pthread_mutex_unlock(&g_lock);
      struct mali_group_destroy_in gd = {out.group, 0};
      kcall(MALI_GROUP_DESTROY, &gd, sizeof gd, NULL, 0);
      return -ENOMEM;
   }
   memset(g, 0, sizeof *g);
   g->used = 1;
   g->id = out.group;
   g->gen = ++g_group_gen;
   g->queues = gc->queues.count;
   g->sync = sync;
   pthread_mutex_unlock(&g_lock);
   gc->group_handle = out.group;
   return 0;
}

static int ioctl_group_destroy(struct drm_panthor_group_destroy *gd)
{
   struct mali_group_destroy_in in = {gd->group_handle, 0};
   kern_return_t kr = kcall(MALI_GROUP_DESTROY, &in, sizeof in, NULL, 0);
   if (kr != KERN_SUCCESS)
      return -kr_errno(kr);
   pthread_mutex_lock(&g_lock);
   struct group *g = group_by_id(gd->group_handle);
   volatile uint64_t *sync = g ? g->sync : NULL;
   if (g)
      memset(g, 0, sizeof *g);
   pthread_mutex_unlock(&g_lock);
   if (sync)
      IOConnectUnmapMemory64(g_conn, MALI_MAP_SYNCOBJ | gd->group_handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)sync);
   return 0;
}

static int ioctl_group_submit(struct drm_panthor_group_submit *gs)
{
   pthread_mutex_lock(&g_lock);
   struct group *g = group_by_id(gs->group_handle);
   const uint32_t gen = g ? g->gen : 0;
   const uint32_t queues = g ? g->queues : 0;
   pthread_mutex_unlock(&g_lock);
   if (!g)
      return -EINVAL;

   for (uint32_t i = 0; i < gs->queue_submits.count; i++) {
      struct drm_panthor_queue_submit qs;
      ARRAY_ELEMENT(qs, gs->queue_submits, i);
      if (qs.queue_index >= queues)
         return -EINVAL;
      int ret = process_waits(&qs.syncs);
      if (ret < 0)
         return ret;

      struct fence f;
      memset(&f, 0, sizeof f);
      if (qs.stream_size) {
         __asm__ volatile("dsb sy" ::: "memory");   /* the command stream and whatever it points at were written through write-combined mappings */
         struct mali_group_submit_in in = {gs->group_handle, qs.queue_index, qs.stream_addr, qs.stream_size, qs.latest_flush};
         struct mali_group_submit_out out = {0};
         kern_return_t kr = kcall(MALI_GROUP_SUBMIT, &in, sizeof in, &out, sizeof out);
         if (kr != KERN_SUCCESS)
            return -kr_errno(kr);
         pthread_mutex_lock(&g_lock);
         g = group_by_id(gs->group_handle);
         if (g && g->gen == gen)
            g->last[qs.queue_index] = out.seqno;
         pthread_mutex_unlock(&g_lock);
         f.n = 1;
         f.l[0] = (struct leaf){gs->group_handle, gen, qs.queue_index, out.seqno};
      } else {
         /* a synchronization point: done when everything given to the queue before it is */
         pthread_mutex_lock(&g_lock);
         g = group_by_id(gs->group_handle);
         if (g && g->gen == gen && g->last[qs.queue_index]) {
            f.n = 1;
            f.l[0] = (struct leaf){gs->group_handle, gen, qs.queue_index, g->last[qs.queue_index]};
         }
         pthread_mutex_unlock(&g_lock);
      }
      ret = process_signals(&qs.syncs, &f);
      if (ret < 0)
         return ret;
   }
   return 0;
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   int ret;
   switch ((int)request) {
   case DRM_IOCTL_PANTHOR_DEV_QUERY:
      ret = ioctl_dev_query(arg);
      break;
   case DRM_IOCTL_PANTHOR_VM_CREATE: {
      struct drm_panthor_vm_create *a = arg;
      struct mali_vm_create_in in = {a->user_va_range};
      struct mali_vm_create_out out = {0};
      kern_return_t kr = kcall(MALI_VM_CREATE, &in, sizeof in, &out, sizeof out);
      ret = kr == KERN_SUCCESS ? 0 : -kr_errno(kr);
      if (!ret) {
         a->id = out.vm;
         a->user_va_range = out.user_va_range;
      }
      break;
   }
   case DRM_IOCTL_PANTHOR_VM_DESTROY: {
      struct drm_panthor_vm_destroy *a = arg;
      struct mali_vm_destroy_in in = {a->id, 0};
      kern_return_t kr = kcall(MALI_VM_DESTROY, &in, sizeof in, NULL, 0);
      ret = kr == KERN_SUCCESS ? 0 : -kr_errno(kr);
      break;
   }
   case DRM_IOCTL_PANTHOR_VM_BIND:
      ret = ioctl_vm_bind(arg);
      break;
   case DRM_IOCTL_PANTHOR_VM_GET_STATE: {
      struct drm_panthor_vm_get_state *a = arg;
      a->state = DRM_PANTHOR_VM_STATE_USABLE;
      ret = 0;
      break;
   }
   case DRM_IOCTL_PANTHOR_BO_CREATE: {
      struct drm_panthor_bo_create *a = arg;
      struct mali_bo_create_in in = {a->size, a->flags, 0};
      struct mali_bo_create_out out = {0};
      kern_return_t kr = kcall(MALI_BO_CREATE, &in, sizeof in, &out, sizeof out);
      ret = kr == KERN_SUCCESS ? 0 : -kr_errno(kr);
      if (!ret) {
         a->handle = out.handle;
         a->size = out.size;
      }
      break;
   }
   case DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET: {
      struct drm_panthor_bo_mmap_offset *a = arg;
      a->offset = (uint64_t)a->handle << 12;
      ret = 0;
      break;
   }
   case DRM_IOCTL_PANTHOR_GROUP_CREATE:
      ret = ioctl_group_create(arg);
      break;
   case DRM_IOCTL_PANTHOR_GROUP_DESTROY:
      ret = ioctl_group_destroy(arg);
      break;
   case DRM_IOCTL_PANTHOR_GROUP_SUBMIT:
      ret = ioctl_group_submit(arg);
      break;
   case DRM_IOCTL_PANTHOR_GROUP_GET_STATE: {
      struct drm_panthor_group_get_state *a = arg;
      a->state = 0;
      a->fatal_queues = 0;
      ret = 0;
      break;
   }
   case DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE: {
      struct drm_panthor_tiler_heap_create *a = arg;
      struct mali_heap_create_in in = {a->vm_id, a->initial_chunk_count, a->chunk_size, a->max_chunks, a->target_in_flight, 0};
      struct mali_heap_create_out out = {0};
      kern_return_t kr = kcall(MALI_HEAP_CREATE, &in, sizeof in, &out, sizeof out);
      ret = kr == KERN_SUCCESS ? 0 : -kr_errno(kr);
      if (!ret) {
         a->handle = out.handle;
         a->tiler_heap_ctx_gpu_va = out.ctx_va;
         a->first_heap_chunk_gpu_va = out.first_chunk_va;
      }
      break;
   }
   case DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY: {
      struct drm_panthor_tiler_heap_destroy *a = arg;
      struct mali_heap_destroy_in in = {a->handle, 0};
      kern_return_t kr = kcall(MALI_HEAP_DESTROY, &in, sizeof in, NULL, 0);
      ret = kr == KERN_SUCCESS ? 0 : -kr_errno(kr);
      break;
   }
   case DRM_IOCTL_PANTHOR_BO_SET_LABEL:
   case DRM_IOCTL_PANTHOR_SET_USER_MMIO_OFFSET:
   case DRM_IOCTL_PANTHOR_BO_SYNC:   /* the CPU side of every mapping is write-combined: nothing to clean or invalidate */
      ret = 0;
      break;
   case DRM_IOCTL_PANTHOR_BO_QUERY_INFO: {
      struct drm_panthor_bo_query_info *a = arg;
      a->extra_flags = 0;
      a->create_flags = 0;
      ret = 0;
      break;
   }
   default:
      ret = -ENOTTY;
      break;
   }
   SHIM_LOG("ioctl %#lx -> %d\n", request, ret);
   if (ret < 0) {
      errno = -ret;
      return -1;
   }
   return 0;
}

/* ======================================================================================================== the rest of libdrm */

drmVersionPtr drmGetVersion(int fd)
{
   (void)fd;
   drmVersionPtr v = calloc(1, sizeof(*v));
   if (!v)
      return NULL;
   /* pan_kmod_dev_create() picks its back end by the name "panthor". 1.4: timestamps and group priorities, no user MMIO offset ioctl,
    * no WB mappings, no BO sync ioctl: what a write-combined, non-coherent GPU can serve. */
   v->version_major = 1;
   v->version_minor = 4;
   v->version_patchlevel = 0;
   v->name = strdup("panthor");
   v->name_len = (int)strlen(v->name);
   v->date = strdup("20260101");
   v->date_len = (int)strlen(v->date);
   v->desc = strdup("Mali (Panthor uAPI over IOKit)");
   v->desc_len = (int)strlen(v->desc);
   return v;
}

void drmFreeVersion(drmVersionPtr v)
{
   if (!v)
      return;
   free(v->name);
   free(v->date);
   free(v->desc);
   free(v);
}

static drmDevicePtr make_device(void)
{
   drmDevicePtr d = calloc(1, sizeof(*d));
   if (!d)
      return NULL;
   d->nodes = calloc(DRM_NODE_MAX, sizeof(char *));
   d->businfo.platform = calloc(1, sizeof(*d->businfo.platform));
   d->deviceinfo.platform = calloc(1, sizeof(*d->deviceinfo.platform));
   if (!d->nodes || !d->businfo.platform || !d->deviceinfo.platform) {
      drmFreeDevice(&d);
      return NULL;
   }
   d->nodes[DRM_NODE_RENDER] = strdup("mali:render");
   d->available_nodes = 1 << DRM_NODE_RENDER;
   d->bustype = DRM_BUS_PLATFORM;
   strlcpy(d->businfo.platform->fullname, "fb000000.gpu", sizeof(d->businfo.platform->fullname));
   d->deviceinfo.platform->compatible = calloc(3, sizeof(char *));
   if (d->deviceinfo.platform->compatible) {
      d->deviceinfo.platform->compatible[0] = strdup("rockchip,rk3588-mali");
      d->deviceinfo.platform->compatible[1] = strdup("arm,mali-valhall-csf");
   }
   return d;
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd;
   (void)flags;
   *device = make_device();
   return *device ? 0 : -ENOMEM;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   (void)flags;
   /* only offer the device when the service is there: a machine without the driver has no GPU for PanVK to find */
   if (!connect_once())
      return 0;
   if (max_devices <= 0 || !devices)
      return 1;
   devices[0] = make_device();
   return devices[0] ? 1 : -ENOMEM;
}

void drmFreeDevice(drmDevicePtr *device)
{
   if (!device || !*device)
      return;
   drmDevicePtr d = *device;
   if (d->nodes) {
      for (int i = 0; i < DRM_NODE_MAX; i++)
         free(d->nodes[i]);
      free(d->nodes);
   }
   free(d->businfo.platform);
   if (d->deviceinfo.platform) {
      if (d->deviceinfo.platform->compatible) {
         for (char **c = d->deviceinfo.platform->compatible; *c; c++)
            free(*c);
         free(d->deviceinfo.platform->compatible);
      }
      free(d->deviceinfo.platform);
   }
   free(d);
   *device = NULL;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
   for (int i = 0; devices && i < count; i++)
      drmFreeDevice(&devices[i]);
}

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
   return a && b && a->bustype == b->bustype;
}

int drmGetMagic(int fd, drm_magic_t *magic)
{
   (void)fd;
   *magic = 0;
   return 0;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   switch (capability) {
   case DRM_CAP_SYNCOBJ:
   case DRM_CAP_SYNCOBJ_TIMELINE:
      *value = 1;
      return 0;
   default:
      *value = 0;
      return 0;
   }
}

int drmCloseBufferHandle(int fd, uint32_t handle)
{
   (void)fd;
   if (handle && handle <= MAX_HOST_BOS) {
      pthread_mutex_lock(&g_lock);
      g_host_bo[handle - 1].ptr = NULL;
      g_host_bo[handle - 1].size = 0;
      pthread_mutex_unlock(&g_lock);
   }
   struct mali_bo_destroy_in in = {handle, 0};
   kern_return_t kr = kcall(MALI_BO_DESTROY, &in, sizeof in, NULL, 0);
   if (kr != KERN_SUCCESS) {
      errno = kr_errno(kr);
      return -1;
   }
   return 0;
}

/* PRIME: no dma-bufs here. */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   return -ENOSYS;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd; (void)prime_fd; (void)handle;
   return -ENOSYS;
}

/* ---------- <sys/sysinfo.h> ---------- */

int sysinfo(struct sysinfo *info)
{
   uint64_t mem = 0;
   size_t len = sizeof(mem);
   if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0)
      return -1;
   memset(info, 0, sizeof(*info));
   info->totalram = (unsigned long)mem;
   info->freeram = (unsigned long)(mem / 2); /* PanVK only looks at totalram */
   info->mem_unit = 1;
   return 0;
}
