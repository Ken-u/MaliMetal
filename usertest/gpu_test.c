/*
 * The first user task that talks to the Mali-G610 driver (Mali, package/Mali): everything the kernel-side self-test of
 * M3b does through externalMethod() from kernel_task, here through IOKit from a real process, with memory mapped by
 * IOConnectMapMemory64 into this task. It stands for what the libdrm stand-in (darwin-drm/drm_darwin.c) will do for PanVK.
 *
 * It is the init process of the board when boot-arg evb10-exec=1 (UniXNU-evb10 patches/xnu/evb10-userland-mounts.patch): nothing else is running, the output goes
 * to /dev/console, which is the serial line. It never exits (an exiting init panics the kernel): after the last line it sleeps.
 *
 * Build: usertest/build.sh (clang-20 -target arm64-apple-ios17.0, signed ad hoc, cdhash into the owner trust cache).
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "mali_uapi.h"
#include "usertest.h"

int console = 2;

void say(const char *format, ...)
{
   char line[512];
   va_list ap;
   va_start(ap, format);
   int n = vsnprintf(line, sizeof line - 1, format, ap);
   va_end(ap);
   if (n < 0)
      return;
   if (n > (int)sizeof line - 2)
      n = (int)sizeof line - 2;
   line[n++] = '\n';
   (void)write(console, line, (size_t)n);
}

static uint64_t now_us(void)
{
   static mach_timebase_info_data_t tb;
   if (!tb.denom)
      mach_timebase_info(&tb);
   return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* The command stream instructions the kernel test uses (Mesa's v10.xml): r0/r1 an address, r2.. data. */
static uint64_t cs_move48(unsigned dst, uint64_t imm) { return (1ull << 56) | ((uint64_t)dst << 48) | (imm & 0xffffffffffffull); }
static uint64_t cs_move32(unsigned dst, uint32_t imm) { return (2ull << 56) | ((uint64_t)dst << 48) | imm; }
static uint64_t cs_wait(unsigned mask) { return (3ull << 56) | ((uint64_t)mask << 16); }
static uint64_t cs_store(unsigned base, unsigned addr, unsigned mask, unsigned offset)
{
   return (21ull << 56) | ((uint64_t)base << 48) | ((uint64_t)addr << 40) | ((uint64_t)mask << 16) | (offset & 0xffff);
}
static uint64_t cs_set_sb(unsigned endpoint, unsigned other) { return (23ull << 56) | endpoint | (other << 4); }

static io_connect_t conn;

static kern_return_t call(unsigned sel, const void *in, size_t in_size, void *out, size_t out_size)
{
   size_t out_len = out_size;
   return IOConnectCallStructMethod(conn, sel, in, in_size, out, out ? &out_len : NULL);
}

static int failures;
#define CHECK(cond, ...)           \
   do {                            \
      if (!(cond)) {               \
         failures++;               \
         say("gpu-test: FAILED: " __VA_ARGS__); \
      }                            \
   } while (0)

static void *map_type(uint32_t type, size_t *size)
{
   mach_vm_address_t addr = 0;
   mach_vm_size_t len = 0;
   kern_return_t kr = IOConnectMapMemory64(conn, type, mach_task_self(), &addr, &len, kIOMapAnywhere | kIOMapWriteCombineCache);
   if (kr != KERN_SUCCESS) {
      say("gpu-test: IOConnectMapMemory64(type %#x) failed: %#x", type, kr);
      return NULL;
   }
   if (size)
      *size = (size_t)len;
   return (void *)(uintptr_t)addr;
}

#define CODE_VA 0x10000000ull
#define DATA_VA 0x10100000ull

int main(void)
{
   int c = open("/dev/console", O_WRONLY);
   if (c >= 0)
      console = c;
   say("gpu-test: start, pid %d uid %d", getpid(), getuid());

   /* The kernel starts the GPU driver (power, firmware, MCU) from the display probe, some 20 s into the boot, and registers the service last. */
   mach_port_t main_port = MACH_PORT_NULL;
   io_service_t svc = IO_OBJECT_NULL;
   for (unsigned waited = 0; waited < 300; waited++) {
      svc = IOServiceGetMatchingService(main_port, IOServiceMatching(MALI_SERVICE));
      if (svc)
         break;
      if (waited % 10 == 0)
         say("gpu-test: waiting for the %s service (%u s)", MALI_SERVICE, waited);
      sleep(1);
   }
   say("gpu-test: IOServiceGetMatchingService(%s) -> %#x", MALI_SERVICE, svc);
   if (!svc)
      goto idle;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   say("gpu-test: IOServiceOpen -> %#x, connection %#x", kr, conn);
   if (kr != KERN_SUCCESS)
      goto idle;

   struct mali_dev_info info;
   memset(&info, 0, sizeof info);
   kr = call(MALI_DEV_QUERY, NULL, 0, &info, sizeof info);
   say("gpu-test: DEV_QUERY %#x: gpu id %#x, %u groups of %u queues, %u registers, shaders %#llx, timestamp %llu Hz, firmware %#x", kr, info.gpu_id,
       info.csg_slot_count, info.cs_slot_count, info.cs_reg_count, (unsigned long long)info.shader_present, (unsigned long long)info.timestamp_frequency, info.fw_version);
   CHECK(kr == KERN_SUCCESS && info.gpu_id == 0xa8670005, "DEV_QUERY");

   /* PanVK's VM: half of the 48 bit address space for the user, the other half for the kernel's objects (rings, heap chunks). */
   struct mali_vm_create_in vm_in = {1ull << 47};
   struct mali_vm_create_out vm = {0};
   kr = call(MALI_VM_CREATE, &vm_in, sizeof vm_in, &vm, sizeof vm);
   say("gpu-test: VM_CREATE %#x: vm %u, user range %#llx", kr, vm.vm, (unsigned long long)vm.user_va_range);
   CHECK(kr == KERN_SUCCESS, "VM_CREATE");

   struct mali_bo_create_in bo_in = {0x4000, 0, 0};
   struct mali_bo_create_out code_bo = {0}, data_bo = {0};
   kr = call(MALI_BO_CREATE, &bo_in, sizeof bo_in, &code_bo, sizeof code_bo);
   CHECK(kr == KERN_SUCCESS, "BO_CREATE code");
   bo_in.size = 0x100000;
   kr = call(MALI_BO_CREATE, &bo_in, sizeof bo_in, &data_bo, sizeof data_bo);
   CHECK(kr == KERN_SUCCESS, "BO_CREATE data");
   say("gpu-test: BO_CREATE: code handle %u (%#llx bytes), data handle %u (%#llx bytes)", code_bo.handle, (unsigned long long)code_bo.size, data_bo.handle,
       (unsigned long long)data_bo.size);

   size_t code_len = 0, data_len = 0;
   volatile uint64_t *code = map_type(code_bo.handle, &code_len);
   volatile uint32_t *data = map_type(data_bo.handle, &data_len);
   say("gpu-test: mapped into this task: code %p (%zu bytes), data %p (%zu bytes)", (void *)code, code_len, (void *)data, data_len);
   CHECK(code && data, "mapping");
   if (!code || !data)
      goto idle;
   for (size_t i = 0; i < data_len / 4; i++)
      data[i] = 0xdeadbeef;
   for (size_t i = 0; i < code_len / 8; i++)
      code[i] = 0;

   struct mali_vm_bind_in bind = {vm.vm, MALI_BIND_MAP, code_bo.handle, MALI_BIND_READONLY | MALI_BIND_UNCACHED, 0, CODE_VA, 0x4000};
   kr = call(MALI_VM_BIND, &bind, sizeof bind, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "VM_BIND code %#x", kr);
   bind = (struct mali_vm_bind_in){vm.vm, MALI_BIND_MAP, data_bo.handle, MALI_BIND_UNCACHED | MALI_BIND_NOEXEC, 0, DATA_VA, 0x100000};
   kr = call(MALI_VM_BIND, &bind, sizeof bind, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "VM_BIND data %#x", kr);

   struct mali_group_create_in g_in;
   memset(&g_in, 0, sizeof g_in);
   g_in.vm = vm.vm;
   g_in.queue_count = 3;
   for (unsigned q = 0; q < 3; q++)
      g_in.ring_size[q] = 0x1000;
   g_in.compute_core_mask = g_in.fragment_core_mask = info.shader_present;
   g_in.tiler_core_mask = info.tiler_present;
   g_in.max_compute_cores = g_in.max_fragment_cores = 4;
   g_in.max_tiler_cores = 1;
   g_in.group_priority = 15;
   struct mali_group_create_out grp = {0};
   kr = call(MALI_GROUP_CREATE, &g_in, sizeof g_in, &grp, sizeof grp);
   say("gpu-test: GROUP_CREATE %#x: group %u in slot %u", kr, grp.group, grp.csg_slot);
   CHECK(kr == KERN_SUCCESS, "GROUP_CREATE");
   if (kr != KERN_SUCCESS)
      goto idle;

   size_t sync_len = 0;
   volatile uint64_t *sync = map_type(MALI_MAP_SYNCOBJ | grp.group, &sync_len);
   say("gpu-test: the syncobj page mapped at %p (%zu bytes)", (void *)sync, sync_len);
   CHECK(sync != NULL, "syncobj page");

   /* Jobs: store two words, submit, wait through FENCE_WAIT (even jobs) or by polling the mapped syncobj page (odd ones). */
   unsigned jobs = 0, bad = 0;
   uint64_t total_us = 0, worst_us = 0;
   for (unsigned j = 0; j < 120; j++) {
      const unsigned q = j % 3;
      const unsigned slot = j % 64;                       /* the program of this job: 64 bytes each */
      const uint32_t off = 0x100 + j * 8;
      const uint32_t v0 = 0xa5000000 + j, v1 = 0x5a000000 + j;
      volatile uint64_t *p = code + slot * 8;
      const uint64_t prog[8] = {cs_set_sb(3, 0), cs_move48(0, DATA_VA + off), cs_move32(2, v0), cs_move32(3, v1), cs_store(2, 0, 3, 0), cs_wait(1u << 0), 0, 0};
      for (unsigned i = 0; i < 8; i++)
         p[i] = prog[i];
      __asm__ volatile("dsb sy" ::: "memory");
      struct mali_group_submit_in s = {grp.group, q, CODE_VA + slot * 64, 64, 0};
      struct mali_group_submit_out so = {0};
      const uint64_t t0 = now_us();
      kr = call(MALI_GROUP_SUBMIT, &s, sizeof s, &so, sizeof so);
      bool done = false;
      if (kr == KERN_SUCCESS) {
         if (j % 2 == 0) {
            struct mali_fence_wait_in w = {grp.group, q, so.seqno, 1000000};
            struct mali_fence_wait_out wo = {0};
            kr = call(MALI_FENCE_WAIT, &w, sizeof w, &wo, sizeof wo);
            done = kr == KERN_SUCCESS && wo.signaled;
         } else if (sync) {
            const uint64_t limit = now_us() + 1000000;
            while (!(done = sync[q * 2] >= so.seqno) && now_us() < limit)
               ;
         }
      }
      const uint64_t dt = now_us() - t0;
      total_us += dt;
      if (dt > worst_us)
         worst_us = dt;
      jobs++;
      if (!done || data[off / 4] != v0 || data[off / 4 + 1] != v1) {
         bad++;
         if (bad <= 5)
            say("gpu-test: job %u on queue %u: kr %#x done %d data %#x %#x (wanted %#x %#x)", j, q, kr, done, data[off / 4], data[off / 4 + 1], v0, v1);
      }
   }
   say("gpu-test: %u jobs on three queues (FENCE_WAIT and polling the mapped syncobj page): %u bad; %llu us each on average, %llu us at worst", jobs, bad,
       (unsigned long long)(total_us / (jobs ? jobs : 1)), (unsigned long long)worst_us);
   CHECK(bad == 0, "jobs");

   /* A tiler heap: PanVK makes one for every queue it creates, even for compute work. */
   struct mali_heap_create_in h_in = {vm.vm, 3, 0x200000, 8, 65535, 0};
   struct mali_heap_create_out heap = {0};
   kr = call(MALI_HEAP_CREATE, &h_in, sizeof h_in, &heap, sizeof heap);
   say("gpu-test: HEAP_CREATE %#x: handle %u, context at %#llx, first chunk at %#llx", kr, heap.handle, (unsigned long long)heap.ctx_va,
       (unsigned long long)heap.first_chunk_va);
   CHECK(kr == KERN_SUCCESS && heap.handle && heap.ctx_va && heap.first_chunk_va && !(heap.first_chunk_va & 0x1fffff), "HEAP_CREATE");
   struct mali_heap_create_in h_bad = {vm.vm, 3, 0x1000, 8, 65535, 0};   /* chunk too small */
   struct mali_heap_create_out h_bad_out = {0};
   kr = call(MALI_HEAP_CREATE, &h_bad, sizeof h_bad, &h_bad_out, sizeof h_bad_out);
   CHECK(kr != KERN_SUCCESS, "HEAP_CREATE with a 4 KiB chunk was accepted");
   h_bad = (struct mali_heap_create_in){vm.vm, 9, 0x200000, 8, 65535, 0};   /* more initial chunks than the maximum */
   kr = call(MALI_HEAP_CREATE, &h_bad, sizeof h_bad, &h_bad_out, sizeof h_bad_out);
   CHECK(kr != KERN_SUCCESS, "HEAP_CREATE with initial > max was accepted");
   struct mali_heap_destroy_in hd = {0x7f, 0};
   kr = call(MALI_HEAP_DESTROY, &hd, sizeof hd, NULL, 0);
   CHECK(kr != KERN_SUCCESS, "HEAP_DESTROY of a handle that is not ours was accepted");
   hd.handle = heap.handle;
   kr = call(MALI_HEAP_DESTROY, &hd, sizeof hd, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "HEAP_DESTROY %#x", kr);
   kr = call(MALI_HEAP_DESTROY, &hd, sizeof hd, NULL, 0);
   CHECK(kr != KERN_SUCCESS, "HEAP_DESTROY twice");
   kr = call(MALI_HEAP_CREATE, &h_in, sizeof h_in, &heap, sizeof heap);   /* one more, left for VM_DESTROY to free */
   CHECK(kr == KERN_SUCCESS, "HEAP_CREATE again %#x", kr);

   /* Strangers' handles and out-of-range arguments are refused for a real process too. */
   struct mali_bo_destroy_in bd = {0x7fff, 0};
   kr = call(MALI_BO_DESTROY, &bd, sizeof bd, NULL, 0);
   say("gpu-test: BO_DESTROY of a handle that is not ours: %#x (not success: %s)", kr, kr != KERN_SUCCESS ? "OK" : "WRONG");
   CHECK(kr != KERN_SUCCESS, "foreign handle accepted");

   struct mali_group_destroy_in gd = {grp.group, 0};
   kr = call(MALI_GROUP_DESTROY, &gd, sizeof gd, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "GROUP_DESTROY %#x", kr);
   struct mali_vm_destroy_in vd = {vm.vm, 0};
   kr = call(MALI_VM_DESTROY, &vd, sizeof vd, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "VM_DESTROY %#x", kr);
   struct mali_bo_destroy_in bd2 = {code_bo.handle, 0};
   kr = call(MALI_BO_DESTROY, &bd2, sizeof bd2, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "BO_DESTROY code %#x", kr);
   bd2.handle = data_bo.handle;
   kr = call(MALI_BO_DESTROY, &bd2, sizeof bd2, NULL, 0);
   CHECK(kr == KERN_SUCCESS, "BO_DESTROY data %#x", kr);
   IOServiceClose(conn);
   IOObjectRelease(svc);
   if (failures)
      say("gpu-test: %d CHECKS FAILED", failures);
   else
      say("gpu-test: ALL OK (user task: open, map, bind, submit, fence, destroy)");

   /* The Vulkan driver, in a child process: a crash there must not take init (and the kernel) with it. */
   if (!failures) {
      fflush(NULL);
      pid_t child = fork();
      if (child == 0) {
         dup2(console, 1);
         dup2(console, 2);
         _exit(vk_test());
      } else if (child > 0) {
         int status = 0;
         waitpid(child, &status, 0);
         say("gpu-test: the Vulkan test process %d ended: status %#x (exit %d, signal %d)", child, status, WEXITSTATUS(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
      } else {
         say("gpu-test: fork failed (%s): running the Vulkan test in this process", strerror(errno));
         dup2(console, 1);
         dup2(console, 2);
         vk_test();
      }
   }

   /* What the original Metal discovery finds: the accelerator the driver registered (boot-arg mali-metal=1). */
   {
      io_service_t acc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("IOAcceleratorES"));
      if (!acc) {
         say("gpu-test: no IOAcceleratorES in the registry");
      } else {
         io_name_t cls;
         IOObjectGetClass(acc, cls);
         say("gpu-test: IOAcceleratorES found, class %s", cls);
         const char *keys[] = {"MetalPluginName", "MetalPluginClassName", "NativeSandboxGrantProcess"};
         for (unsigned k = 0; k < 3; k++) {
            CFStringRef key = CFStringCreateWithCString(NULL, keys[k], kCFStringEncodingUTF8);
            CFTypeRef value = IORegistryEntryCreateCFProperty(acc, key, NULL, 0);
            char text[128] = "(none)";
            if (value && CFGetTypeID(value) == CFStringGetTypeID())
               CFStringGetCString((CFStringRef)value, text, sizeof text, kCFStringEncodingUTF8);
            say("gpu-test:   %s = %s", keys[k], text);
            if (value)
               CFRelease(value);
            CFRelease(key);
         }
         IOObjectRelease(acc);
      }
   }

   /* The Metal plugin as the addon put it into /System/Library/Extensions (boot-arg evb10-gpu-addon=1): the bridge's self-test over PanVK. */
   {
      const char *plugin = "/System/Library/Extensions/NativeMaliMetal.bundle/NativeMaliMetal";
      if (access(plugin, R_OK) != 0) {
         say("gpu-test: %s is not there (%s)", plugin, strerror(errno));
      } else {
         say("gpu-test: %s is there", plugin);
         fflush(NULL);
         pid_t child = fork();
         if (child == 0) {
            dup2(console, 1);
            dup2(console, 2);
            void *lib = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
            if (!lib) {
               say("gpu-test: dlopen(plugin) failed: %s", dlerror());
               _exit(2);
            }
            int (*selftest)(int) = (int (*)(int))dlsym(lib, "nmm_selftest");
            if (!selftest) {
               say("gpu-test: no nmm_selftest in the plugin");
               _exit(3);
            }
            say("gpu-test: the plugin is loaded, running its self-test");
            int code = selftest(0);
            say("gpu-test: the plugin's self-test returned %#x", code);
            _exit(code ? 4 : 0);
         } else if (child > 0) {
            int status = 0;
            waitpid(child, &status, 0);
            say("gpu-test: the plugin test process %d ended: status %#x (exit %d, signal %d)", child, status, WEXITSTATUS(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
         }
      }
   }

idle:
   for (unsigned n = 0;; n++) {
      sleep(10);
      say("gpu-test: idle, %u", n);
   }
}
