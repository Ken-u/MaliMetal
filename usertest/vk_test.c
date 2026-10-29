/*
 * PanVK, the Mesa Vulkan driver for Mali, on this board: dlopen() the ICD library, make an instance and a device, run a compute shader
 * and look at what it wrote. Everything the DRM stand-in (darwin-drm/drm_darwin.c) and the kernel driver (Mali) are for.
 *
 * It is called from gpu_test.c in a child process after the kernel API checks passed; output goes to /dev/console.
 * The shader is shaders/square.comp compiled with glslangValidator (build.sh makes vk_shader.inc).
 */
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include "usertest.h"

/* libSystem's own, to see what it does. */
extern void libsys_dcache_flush(void *start, size_t length) __asm__("_sys_dcache_flush");

/* Clean and invalidate the CPU's caches over a range, to the point of coherency (DC CIVAC; EL0 may, SCTLR_EL1.UCI, and reads CTR_EL0, UCT). */
static void sys_dcache_flush(void *start, size_t length) {
   uint64_t ctr;
   __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
   const size_t line = 4u << ((ctr >> 16) & 0xf);
   uintptr_t p = (uintptr_t)start & ~(uintptr_t)(line - 1);
   for (; p < (uintptr_t)start + length; p += line)
      __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
   __asm__ volatile("dsb sy" : : : "memory");
}

#define ICD_PATH "/private/var/evb10/libvulkan_panfrost.dylib"

static const uint32_t kShader[] = {
#include "vk_shader.inc"
};

#define COUNT 1024u

#define VK(call)                                                                        \
   do {                                                                                 \
      VkResult r_ = (call);                                                             \
      if (r_ != VK_SUCCESS) {                                                           \
         say("vk-test: %s -> VkResult %d (line %d)", #call, (int)r_, __LINE__);         \
         return 1;                                                                      \
      }                                                                                 \
   } while (0)

#define LOAD(var, name)                                                                 \
   PFN_##name var = (PFN_##name)gipa(instance, #name);                                  \
   if (!var) {                                                                          \
      say("vk-test: the ICD has no %s", #name);                                         \
      return 1;                                                                         \
   }


/* A crash in the ICD must say where: the signal, the faulting instruction, and the frame-pointer chain as image + offset (the ICD's link map,
 * $MESA_BUILD/panvk.map, turns an offset of libvulkan_panfrost.dylib into a function; an offset in a system image is a dyld cache
 * address: dladdr's symbol name is printed when there is one). */
static void where(const char *what, uint64_t pc)
{
   Dl_info info;
   if (dladdr((void *)pc, &info) && info.dli_fname)
      say("vk-test:   %s %#llx = %s+%#llx  (%s+%#llx)", what, (unsigned long long)pc, info.dli_sname ? info.dli_sname : "?",
          info.dli_saddr ? (unsigned long long)(pc - (uint64_t)info.dli_saddr) : 0ull, info.dli_fname, (unsigned long long)(pc - (uint64_t)info.dli_fbase));
   else
      say("vk-test:   %s %#llx", what, (unsigned long long)pc);
}

static void crash(int sig, siginfo_t *si, void *ctx)
{
   ucontext_t *uc = (ucontext_t *)ctx;
   uint64_t pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
   uint64_t lr = __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
   uint64_t fp = __darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
   say("vk-test: CRASH signal %d code %d address %p", sig, si->si_code, si->si_addr);
   where("pc", pc);
   if (sig == SIGILL || sig == SIGTRAP)
      say("vk-test:   instruction at pc: %08x", *(volatile uint32_t *)pc);
   where("lr", lr);
   for (int depth = 0; depth < 24 && fp && !(fp & 15); depth++) {
      uint64_t *frame = (uint64_t *)fp;
      uint64_t next = frame[0], ret = frame[1];
      if (!ret)
         break;
      where("frame", ret);
      if (next <= fp || next - fp > (1u << 22))
         break;
      fp = next;
   }
   _exit(100 + sig);
}

int vk_test(void)
{
   struct sigaction sa;
   memset(&sa, 0, sizeof sa);
   sa.sa_sigaction = crash;
   sa.sa_flags = SA_SIGINFO;
   sigemptyset(&sa.sa_mask);
   const int sigs[] = {SIGILL, SIGSEGV, SIGBUS, SIGTRAP, SIGABRT, SIGFPE};
   for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
      sigaction(sigs[i], &sa, NULL);
   {
      void *tiny = dlopen("/private/var/evb10/libtiny.dylib", RTLD_NOW | RTLD_LOCAL);
      if (!tiny) {
         say("vk-test: dlopen(libtiny) failed: %s", dlerror());
      } else {
         int (*f)(int) = (int (*)(int))dlsym(tiny, "tiny_answer");
         say("vk-test: dlopen(libtiny) ok, tiny_answer(1) = %d", f ? f(1) : -1);
      }
   }
   /* What the ICD and the DRM stand-in tell about their start-up and every ioctl. */
   setenv("PANVK_DEBUG", "startup", 1);
   setenv("DRM_DARWIN_TRACE", "1", 1);
   say("vk-test: dlopen(%s)", ICD_PATH);
   void *lib = dlopen(ICD_PATH, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      say("vk-test: dlopen failed: %s", dlerror());
      return 2;
   }
   say("vk-test: loaded");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   uint32_t (*negotiate)(uint32_t *) = (uint32_t(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   if (!gipa || !negotiate) {
      say("vk-test: the ICD entry points are missing (%p %p)", (void *)gipa, (void *)negotiate);
      return 2;
   }
   uint32_t icd_version = 5;
   VkResult r = (VkResult)negotiate(&icd_version);
   say("vk-test: ICD interface version %u (%d)", icd_version, (int)r);

   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!create_instance) {
      say("vk-test: no vkCreateInstance");
      return 2;
   }
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "evb10-vk-test", .apiVersion = VK_API_VERSION_1_3};
   const VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance instance = VK_NULL_HANDLE;
   VK(create_instance(&ici, NULL, &instance));
   say("vk-test: vkCreateInstance ok");

   LOAD(enum_pd, vkEnumeratePhysicalDevices)
   LOAD(get_props, vkGetPhysicalDeviceProperties)
   LOAD(get_mem_props, vkGetPhysicalDeviceMemoryProperties)
   LOAD(get_queue_props, vkGetPhysicalDeviceQueueFamilyProperties)
   LOAD(create_device, vkCreateDevice)
   uint32_t n = 0;
   VK(enum_pd(instance, &n, NULL));
   say("vk-test: %u physical device(s)", n);
   if (!n)
      return 1;
   n = 1;
   VkPhysicalDevice pd;
   r = enum_pd(instance, &n, &pd);
   if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
      say("vk-test: vkEnumeratePhysicalDevices -> %d", (int)r);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   get_props(pd, &props);
   say("vk-test: %s, api %u.%u.%u, driver %#x, vendor %#x device %#x, type %d", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion, props.vendorID, props.deviceID, (int)props.deviceType);

   VkPhysicalDeviceMemoryProperties mem;
   get_mem_props(pd, &mem);
   for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
      say("vk-test: memory type %u: flags %#x heap %u", i, mem.memoryTypes[i].propertyFlags, mem.memoryTypes[i].heapIndex);
   uint32_t nq = 0;
   get_queue_props(pd, &nq, NULL);
   VkQueueFamilyProperties qprops[8];
   if (nq > 8)
      nq = 8;
   get_queue_props(pd, &nq, qprops);
   uint32_t family = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++) {
      say("vk-test: queue family %u: flags %#x, %u queues", i, qprops[i].queueFlags, qprops[i].queueCount);
      if (family == UINT32_MAX && (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
         family = i;
   }
   if (family == UINT32_MAX) {
      say("vk-test: no compute queue");
      return 1;
   }

   const float prio = 1.0f;
   const VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio};
   /* VK_EXT_external_memory_host (metal2vulkan's no-copy import of IOSurface pages) when the driver has it. */
   bool have_host = false;
   {
      LOAD(enum_dev_ext, vkEnumerateDeviceExtensionProperties)
      uint32_t ne = 0;
      enum_dev_ext(pd, NULL, &ne, NULL);
      VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof *ext);
      if (ext && enum_dev_ext(pd, NULL, &ne, ext) == VK_SUCCESS)
         for (uint32_t i = 0; i < ne; i++)
            if (!strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
               have_host = true;
      free(ext);
      say("vk-test: VK_EXT_external_memory_host %s", have_host ? "offered" : "not offered");
   }
   const char *host_ext[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
   const VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
   const VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                   .enabledExtensionCount = have_host ? 1u : 0u, .ppEnabledExtensionNames = host_ext};
   VkDevice dev = VK_NULL_HANDLE;
   say("vk-test: vkCreateDevice ...");
   VK(create_device(pd, &dci, NULL, &dev));
   say("vk-test: vkCreateDevice ok");

   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define DLOAD(var, name)                                                                \
   PFN_##name var = (PFN_##name)gdpa(dev, #name);                                       \
   if (!var) {                                                                          \
      say("vk-test: the device has no %s", #name);                                      \
      return 1;                                                                         \
   }
   DLOAD(get_queue, vkGetDeviceQueue)
   DLOAD(create_buffer, vkCreateBuffer)
   DLOAD(get_buf_reqs, vkGetBufferMemoryRequirements)
   DLOAD(alloc_mem, vkAllocateMemory)
   DLOAD(bind_buf, vkBindBufferMemory)
   DLOAD(map_mem, vkMapMemory)
   DLOAD(create_shader, vkCreateShaderModule)
   DLOAD(create_dsl, vkCreateDescriptorSetLayout)
   DLOAD(create_pl, vkCreatePipelineLayout)
   DLOAD(create_cp, vkCreateComputePipelines)
   DLOAD(create_dpool, vkCreateDescriptorPool)
   DLOAD(alloc_ds, vkAllocateDescriptorSets)
   DLOAD(update_ds, vkUpdateDescriptorSets)
   DLOAD(create_cpool, vkCreateCommandPool)
   DLOAD(alloc_cb, vkAllocateCommandBuffers)
   DLOAD(begin_cb, vkBeginCommandBuffer)
   DLOAD(cmd_bind_pipe, vkCmdBindPipeline)
   DLOAD(cmd_bind_ds, vkCmdBindDescriptorSets)
   DLOAD(cmd_dispatch, vkCmdDispatch)
   DLOAD(end_cb, vkEndCommandBuffer)
   DLOAD(create_fence, vkCreateFence)
   DLOAD(queue_submit, vkQueueSubmit)
   DLOAD(wait_fences, vkWaitForFences)
   DLOAD(device_wait_idle, vkDeviceWaitIdle)
   DLOAD(destroy_device, vkDestroyDevice)

   VkQueue queue;
   get_queue(dev, family, 0, &queue);

   const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = COUNT * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer;
   VK(create_buffer(dev, &bci, NULL, &buffer));
   VkMemoryRequirements reqs;
   get_buf_reqs(dev, buffer, &reqs);
   uint32_t type = UINT32_MAX;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
      if ((reqs.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) {
         type = i;
         break;
      }
   if (type == UINT32_MAX) {
      say("vk-test: no host visible, coherent memory type for the buffer (bits %#x)", reqs.memoryTypeBits);
      return 1;
   }
   const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = reqs.size, .memoryTypeIndex = type};
   VkDeviceMemory memory;
   VK(alloc_mem(dev, &mai, NULL, &memory));
   VK(bind_buf(dev, buffer, memory, 0));
   volatile uint32_t *out = NULL;
   VK(map_mem(dev, memory, 0, VK_WHOLE_SIZE, 0, (void **)&out));
   for (uint32_t i = 0; i < COUNT; i++)
      out[i] = 0xdeadbeef;
   say("vk-test: buffer of %u bytes in memory type %u mapped at %p", (unsigned)reqs.size, type, (void *)out);

   const VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof kShader, .pCode = kShader};
   VkShaderModule shader;
   VK(create_shader(dev, &smci, NULL, &shader));
   const VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   const VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
   VkDescriptorSetLayout dsl;
   VK(create_dsl(dev, &dslci, NULL, &dsl));
   const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl};
   VkPipelineLayout pl;
   VK(create_pl(dev, &plci, NULL, &pl));
   const VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
      .layout = pl};
   VkPipeline pipeline;
   say("vk-test: compiling the compute pipeline ...");
   VK(create_cp(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
   say("vk-test: pipeline ok");

   const VkDescriptorPoolSize psize = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   const VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psize};
   VkDescriptorPool dpool;
   VK(create_dpool(dev, &dpci, NULL, &dpool));
   const VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   VK(alloc_ds(dev, &dsai, &ds));
   const VkDescriptorBufferInfo dbi = {.buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE};
   const VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
   update_ds(dev, 1, &wds, 0, NULL);

   const VkCommandPoolCreateInfo cpoolci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
   VkCommandPool cpool;
   VK(create_cpool(dev, &cpoolci, NULL, &cpool));
   const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
   VkCommandBuffer cb;
   VK(alloc_cb(dev, &cbai, &cb));
   const VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK(begin_cb(cb, &cbbi));
   cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   cmd_bind_ds(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   cmd_dispatch(cb, COUNT / 64, 1, 1);
   VK(end_cb(cb));

   const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK(create_fence(dev, &fci, NULL, &fence));
   const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
   say("vk-test: vkQueueSubmit ...");
   VK(queue_submit(queue, 1, &si, fence));
   r = wait_fences(dev, 1, &fence, VK_TRUE, 20ull * 1000000000ull);
   say("vk-test: vkWaitForFences -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   unsigned bad = 0;
   for (uint32_t i = 0; i < COUNT; i++) {
      const uint32_t want_value = i * i + 7u;
      if (out[i] != want_value) {
         if (bad < 8)
            say("vk-test: d[%u] = %#x, wanted %#x", i, out[i], want_value);
         bad++;
      }
   }
   say("vk-test: compute result: %u of %u words wrong", bad, COUNT);
   VK(device_wait_idle(dev));
   if (bad) {
      destroy_device(dev, NULL);
      return 1;
   }
   say("vk-test: compute OK (PanVK: instance, device, compute pipeline, dispatch, fence, readback)");

   /* The same dispatch over a buffer made of imported host pages: whole pages of this process (anonymous memory here, an IOSurface's for the
    * Metal frontend). The CPU's caches are this test's to maintain (sys_dcache_flush = clean + invalidate by address), as the frontend's. */
   if (have_host) {
      DLOAD(get_host_props, vkGetMemoryHostPointerPropertiesEXT)
      DLOAD(reset_fences, vkResetFences)
      DLOAD(free_mem, vkFreeMemory)
      DLOAD(destroy_buffer, vkDestroyBuffer)
      const size_t bytes = 16384;
      uint32_t *host = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (host == MAP_FAILED) {
         say("vk-test: mmap of host pages failed: %s", strerror(errno));
         return 1;
      }
      for (uint32_t i = 0; i < COUNT; i++)
         host[i] = 0xdeadbeef;
      sys_dcache_flush(host, bytes);
      VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
      VK(get_host_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp));
      say("vk-test: host pointer %p importable into memory types %#x", (void *)host, hp.memoryTypeBits);
      const VkExternalMemoryBufferCreateInfo ebci = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                     .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
      const VkBufferCreateInfo bci2 = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebci, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
      VkBuffer buffer2;
      VK(create_buffer(dev, &bci2, NULL, &buffer2));
      VkMemoryRequirements reqs2;
      get_buf_reqs(dev, buffer2, &reqs2);
      const uint32_t bits = reqs2.memoryTypeBits & hp.memoryTypeBits;
      uint32_t type2 = UINT32_MAX;
      for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
         if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) {
            type2 = i;
            break;
         }
      if (type2 == UINT32_MAX) {
         say("vk-test: no memory type for the import (buffer %#x, pointer %#x)", reqs2.memoryTypeBits, hp.memoryTypeBits);
         return 1;
      }
      const VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = host};
      const VkMemoryAllocateInfo mai2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = bytes, .memoryTypeIndex = type2};
      VkDeviceMemory memory2;
      VK(alloc_mem(dev, &mai2, NULL, &memory2));
      VK(bind_buf(dev, buffer2, memory2, 0));
      say("vk-test: imported %zu bytes of host pages as memory type %u", bytes, type2);
      const VkDescriptorBufferInfo dbi2 = {.buffer = buffer2, .offset = 0, .range = COUNT * 4};
      const VkWriteDescriptorSet wds2 = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi2};
      update_ds(dev, 1, &wds2, 0, NULL);
      VkCommandBuffer cb2;
      VK(alloc_cb(dev, &cbai, &cb2));
      VK(begin_cb(cb2, &cbbi));
      cmd_bind_pipe(cb2, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      cmd_bind_ds(cb2, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
      cmd_dispatch(cb2, COUNT / 64, 1, 1);
      VK(end_cb(cb2));
      VK(reset_fences(dev, 1, &fence));
      const VkSubmitInfo si2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb2};
      VK(queue_submit(queue, 1, &si2, fence));
      r = wait_fences(dev, 1, &fence, VK_TRUE, 20ull * 1000000000ull);
      say("vk-test: vkWaitForFences (imported) -> %d", (int)r);
      if (r != VK_SUCCESS)
         return 1;
      /* First with libSystem's sys_dcache_flush, then with the explicit DC CIVAC loop. */
      libsys_dcache_flush(host, bytes);
      unsigned bad_libsys = 0;
      for (uint32_t i = 0; i < COUNT; i++)
         if (host[i] != i * i + 7u)
            bad_libsys++;
      say("vk-test: after libSystem sys_dcache_flush: %u of %u words wrong", bad_libsys, COUNT);
      sys_dcache_flush(host, bytes);
      bad = 0;
      for (uint32_t i = 0; i < COUNT; i++) {
         const uint32_t want_value = i * i + 7u;
         if (host[i] != want_value) {
            if (bad < 8)
               say("vk-test: host[%u] = %#x, wanted %#x", i, host[i], want_value);
            bad++;
         }
      }
      say("vk-test: imported-memory result: %u of %u words wrong", bad, COUNT);
      VK(device_wait_idle(dev));
      destroy_buffer(dev, buffer2, NULL);
      free_mem(dev, memory2, NULL);
      munmap(host, bytes);
      if (bad) {
         destroy_device(dev, NULL);
         return 1;
      }
      say("vk-test: host-pointer import OK (VK_EXT_external_memory_host)");
   }
   int gfx = vk_gfx(gdpa, dev, queue, family, &mem);
   say("vk-test: graphics: %s", gfx ? "FAILED" : "OK (clear + triangle through the tiler and the fragment job)");
   destroy_device(dev, NULL);
   if (gfx)
      return 1;
   say("vk-test: ALL OK");
   return 0;
}
