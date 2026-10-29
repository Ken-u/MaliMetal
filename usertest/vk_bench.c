/* What a render pass and a full-screen fill cost on this GPU, without CoreAnimation, Metal or metal2vulkan in the way: render passes into 1072x1904
 * BGRA8 targets (the size of the panel), one fill = one triangle that covers the target, wall-clock time of a command buffer of PASSES passes.
 * Called from vk_test.c after the graphics test, as the init process (evb10-exec=1), so the GPU has nothing else to do.
 *
 * Why: CoreAnimation's frames spend most of their GPU time in passes of a few opaque full-screen fills (Metal pipelines "PBGRAXm_A2Xhf", two color
 * attachments of which the second is memoryless and write-masked), about 1.6 ms for one although Mesa counts 0.7 cycles of arithmetic for the
 * shader; this finds out whether that is the hardware's fill rate, the second target, the pass's load/store, or forward pixel kill not working.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan_core.h>

#include "usertest.h"

static const uint32_t kVert[] = {
#include "vk_bench_vert.inc"
};
static const uint32_t kFrag1[] = {
#include "vk_bench1.inc"
};
static const uint32_t kFrag2[] = {
#include "vk_bench2.inc"
};
static const uint32_t kFragH1[] = {
#include "vk_benchh1.inc"
};
static const uint32_t kFragH2[] = {
#include "vk_benchh2.inc"
};
static const uint32_t kFragB[] = {
#include "vk_benchb.inc"
};

#define W 1072u
#define H 1904u
#define PASSES 6u
#define REPS 5u

static struct {
#define F(name) PFN_vk##name name;
   F(CreateImage) F(GetImageMemoryRequirements) F(AllocateMemory) F(BindImageMemory) F(CreateImageView) F(CreateShaderModule)
   F(CreatePipelineLayout) F(CreateGraphicsPipelines) F(CreateCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer)
   F(ResetCommandBuffer) F(CmdPipelineBarrier) F(CmdBeginRendering) F(CmdEndRendering) F(CmdBindPipeline) F(CmdSetViewport) F(CmdSetScissor)
   F(CmdDraw) F(CreateFence) F(ResetFences) F(QueueSubmit) F(WaitForFences) F(MapMemory) F(GetImageSubresourceLayout)
#undef F
} vk;

static VkDevice gdev;
static VkQueue gqueue;
static VkCommandBuffer gcmd;
static VkFence gfence;
static VkPipelineLayout gpl;
static const VkPhysicalDeviceMemoryProperties *gmem;

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

/* WANT: memory properties the image's memory must have (0: any); MEMORY gets the memory when not NULL */
static bool make_image(VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags want, VkImage *image, VkImageView *view, VkDeviceMemory *memory_out)
{
   const VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
                                  .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = tiling, .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkResult r = vk.CreateImage(gdev, &ici, NULL, image);
   if (r != VK_SUCCESS) {
      say("vk-bench: vkCreateImage(tiling %d, usage %#x) -> %d", (int)tiling, (unsigned)usage, (int)r);
      return false;
   }
   VkMemoryRequirements req;
   vk.GetImageMemoryRequirements(gdev, *image, &req);
   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < gmem->memoryTypeCount; i++)
      if ((req.memoryTypeBits & (1u << i)) && (gmem->memoryTypes[i].propertyFlags & want) == want) {
         type = i;
         break;
      }
   if (type == UINT32_MAX)
      return false;
   const VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
   VkDeviceMemory memory;
   if (vk.AllocateMemory(gdev, &mai, NULL, &memory) != VK_SUCCESS || vk.BindImageMemory(gdev, *image, memory, 0) != VK_SUCCESS)
      return false;
   if (memory_out)
      *memory_out = memory;
   const VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = VK_FORMAT_B8G8R8A8_UNORM,
                                      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
   return vk.CreateImageView(gdev, &vci, NULL, view) == VK_SUCCESS;
}

static VkShaderModule make_shader(const uint32_t *code, size_t bytes)
{
   const VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
   VkShaderModule m = VK_NULL_HANDLE;
   if (vk.CreateShaderModule(gdev, &ci, NULL, &m) != VK_SUCCESS)
      say("vk-bench: vkCreateShaderModule failed");
   return m;
}

/* A pipeline of RTS color targets (BGRA8): target 0 writes MASK0 (source-over blended when BLEND), target 1 writes MASK1. */
static VkPipeline make_pipe(VkShaderModule vs, VkShaderModule fs, unsigned rts, uint32_t mask0, uint32_t mask1, bool blend)
{
   const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   const VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   const VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   const VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   const VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba[2] = {
      {.blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
       .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
       .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = mask0},
      {.colorWriteMask = mask1}};
   const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = rts,
                                                   .pAttachments = cba};
   const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   const VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   const VkFormat fmt[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
   const VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = rts,
                                             .pColorAttachmentFormats = fmt};
   const VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = stages,
                                              .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                              .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = gpl};
   VkPipeline pipe = VK_NULL_HANDLE;
   const VkResult r = vk.CreateGraphicsPipelines(gdev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
   if (r != VK_SUCCESS)
      say("vk-bench: vkCreateGraphicsPipelines(rts %u masks %#x %#x blend %d) -> %d", rts, (unsigned)mask0, (unsigned)mask1, blend, (int)r);
   return pipe;
}

/* A command buffer of PASSES render passes, each with FILLS draws of the pipeline over SCISSOR. LOAD0/STORE0 are target 0's actions; target 1 (when RTS
 * is 2) is never loaded or stored (what Metal calls memoryless). */
static void record(VkPipeline pipe, VkImageView v0, VkImageView v1, VkImage i0, VkImage i1, unsigned rts, unsigned fills, VkRect2D scissor,
                   VkAttachmentLoadOp load0, VkAttachmentStoreOp store0)
{
   vk.ResetCommandBuffer(gcmd, 0);
   const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   vk.BeginCommandBuffer(gcmd, &bi);
   const VkImageSubresourceRange all = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
   VkImageMemoryBarrier to[2];
   for (unsigned i = 0; i < 2; i++)
      to[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = i ? i1 : i0, .subresourceRange = all};
   vk.CmdPipelineBarrier(gcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, rts, to);
   const VkMemoryBarrier waw = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
   const VkViewport viewport = {0, 0, W, H, 0.0f, 1.0f};
   for (unsigned p = 0; p < PASSES; p++) {
      const VkRenderingAttachmentInfo att[2] = {
         {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v0, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .loadOp = load0, .storeOp = store0, .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}},
         {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v1, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE}};
      const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = rts,
                                  .pColorAttachments = att};
      vk.CmdBeginRendering(gcmd, &ri);
      vk.CmdBindPipeline(gcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vk.CmdSetViewport(gcmd, 0, 1, &viewport);
      vk.CmdSetScissor(gcmd, 0, 1, &scissor);
      for (unsigned f = 0; f < fills; f++)
         vk.CmdDraw(gcmd, 3, 1, 0, 0);
      vk.CmdEndRendering(gcmd);
      vk.CmdPipelineBarrier(gcmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &waw, 0, NULL, 0, NULL);
   }
   vk.EndCommandBuffer(gcmd);
}

/* The best of REPS submissions of the recorded command buffer, per pass, in milliseconds (the submit and the wait are in it: a fraction of a ms). */
static double run(void)
{
   double best = 1e30;
   for (unsigned r = 0; r < REPS + 1; r++) {
      vk.ResetFences(gdev, 1, &gfence);
      const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &gcmd};
      const double t0 = now_ms();
      if (vk.QueueSubmit(gqueue, 1, &si, gfence) != VK_SUCCESS || vk.WaitForFences(gdev, 1, &gfence, VK_TRUE, 20ull * 1000000000ull) != VK_SUCCESS) {
         say("vk-bench: submit/wait failed");
         return -1;
      }
      const double t = now_ms() - t0;
      if (r && t < best)   /* the first one warms the pipelines and the memory up */
         best = t;
   }
   return best / PASSES;
}

typedef struct {
   const char *name;
   VkPipeline pipe;
   unsigned rts;
   bool linear;   /* target 0 is the linear image (the layout of an IOSurface) */
} Config;

/* One render pass of two targets: a draw of KEEP (which writes target 1 only), then NFILL draws of FILL (which write target 0 only, over the same area).
 * Target 1 is cleared to zero and stored, so the host can read what KEEP left in it; target 0 is not kept. */
static void record_keep(VkPipeline keep, VkPipeline fill, unsigned nfill, VkImageView v0, VkImageView v1, VkImage i0, VkImage i1)
{
   vk.ResetCommandBuffer(gcmd, 0);
   const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   vk.BeginCommandBuffer(gcmd, &bi);
   const VkImageSubresourceRange all = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
   VkImageMemoryBarrier to[2];
   for (unsigned i = 0; i < 2; i++)
      to[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = i ? i1 : i0, .subresourceRange = all};
   vk.CmdPipelineBarrier(gcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2, to);
   const VkViewport viewport = {0, 0, W, H, 0.0f, 1.0f};
   const VkRect2D full = {{0, 0}, {W, H}};
   const VkRenderingAttachmentInfo att[2] = {
      {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v0, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE},
      {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v1, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
   const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 2,
                               .pColorAttachments = att};
   vk.CmdBeginRendering(gcmd, &ri);
   vk.CmdSetViewport(gcmd, 0, 1, &viewport);
   vk.CmdSetScissor(gcmd, 0, 1, &full);
   vk.CmdBindPipeline(gcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, keep);
   vk.CmdDraw(gcmd, 3, 1, 0, 0);
   vk.CmdBindPipeline(gcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fill);
   for (unsigned f = 0; f < nfill; f++)
      vk.CmdDraw(gcmd, 3, 1, 0, 0);
   vk.CmdEndRendering(gcmd);
   vk.EndCommandBuffer(gcmd);
}

static bool submit_wait(void)
{
   vk.ResetFences(gdev, 1, &gfence);
   const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &gcmd};
   return vk.QueueSubmit(gqueue, 1, &si, gfence) == VK_SUCCESS && vk.WaitForFences(gdev, 1, &gfence, VK_TRUE, 20ull * 1000000000ull) == VK_SUCCESS;
}

/* The driver's two ways with forward pixel kill (PANVK_FPK): "bound" (a draw kills only if it writes every bound target, what it required before) and
 * the default, where the hardware's own rule applies. */
static void set_fpk(const char *mode)
{
   if (!strcmp(mode, "bound"))
      setenv("PANVK_FPK", "bound", 1);
   else
      unsetenv("PANVK_FPK");
}

/* Does forward pixel kill (PANVK_FPK) cost or keep what it should? A draw of KEEP (an ALU-heavy shader, so its fragments are still on their way when
 * the fills arrive) writes target 1, then fills write target 0 only. Where a fill kills a fragment of KEEP, target 1 keeps its cleared value: that is the
 * damage a draw that leaves target 1 unwritten does when an earlier draw wrote it. KEEP is tried writing target 1 only and writing both targets.
 * "bound": no draw may kill when it leaves a bound target unwritten (what the driver required before); "default": whatever the hardware lets it. */
static void fpk_section(VkShaderModule vs, VkShaderModule f1, VkShaderModule h1, VkShaderModule h2, VkImageView v0, VkImageView v1, VkImage i0, VkImage i1,
                        VkImageView vkeep, VkImage ikeep, VkDeviceMemory mkeep)
{
   const VkPipeline fill = make_pipe(vs, f1, 2, 0xf, 0x0, false);    /* constant colour into target 0, target 1 masked */
   const VkPipeline heavy = make_pipe(vs, h1, 2, 0xf, 0x0, false);   /* the same with an ALU-heavy shader: what CoreAnimation's fill is to the driver */
   const VkPipeline keeps[2] = {make_pipe(vs, h2, 2, 0x0, 0xf, false),    /* ALU-heavy, target 0 masked: writes target 1 only */
                                make_pipe(vs, h2, 2, 0xf, 0xf, false)};   /* ALU-heavy, writes both targets */
   static const char *const keep_names[2] = {"writes target 1 only", "writes both targets"};
   if (!fill || !heavy || !keeps[0] || !keeps[1])
      return;
   void *map = NULL;
   if (vk.MapMemory(gdev, mkeep, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
      say("vk-bench: cannot map the readable target");
      return;
   }
   const VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   VkSubresourceLayout layout;
   vk.GetImageSubresourceLayout(gdev, ikeep, &sub, &layout);
   const uint8_t *pixels = (const uint8_t *)map + layout.offset;
   uint32_t *ref[2] = {malloc((size_t)W * H * 4), malloc((size_t)W * H * 4)};
   if (!ref[0] || !ref[1])
      return;

   for (unsigned k = 0; k < 2; k++) {
      record_keep(keeps[k], fill, 0, v0, vkeep, i0, ikeep);
      if (!submit_wait()) {
         say("vk-bench: the reference pass failed");
         return;
      }
      unsigned lit = 0;
      for (unsigned y = 0; y < H; y++) {
         memcpy(ref[k] + (size_t)y * W, pixels + y * layout.rowPitch, W * 4);
         for (unsigned x = 0; x < W; x++)
            lit += ref[k][(size_t)y * W + x] != 0;
      }
      say("vk-bench: reference, KEEP %s: %u of %u pixels of target 1 are non-zero", keep_names[k], lit, W * H);
   }

   static const char *const modes[] = {"bound", "default"};
   static const unsigned counts[3] = {1, 4, 8};
   for (unsigned m = 0; m < 2; m++) {
      set_fpk(modes[m]);
      double c[3], a[3];
      for (unsigned k = 0; k < 3; k++) {
         record(fill, v0, v1, i0, i1, 2, counts[k], (VkRect2D){{0, 0}, {W, H}}, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
         c[k] = run();
         record(heavy, v0, v1, i0, i1, 2, counts[k], (VkRect2D){{0, 0}, {W, H}}, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
         a[k] = run();
      }
      say("vk-bench: FPK %-7s: fills into target 0 (target 1 masked), ms per pass at 1 / 4 / 8 fills: constant %6.3f %6.3f %6.3f   ALU-heavy %6.3f %6.3f %6.3f",
          modes[m], c[0], c[1], c[2], a[0], a[1], a[2]);

      for (unsigned k = 0; k < 2; k++) {
         record_keep(keeps[k], fill, 4, v0, vkeep, i0, ikeep);
         const double t0 = now_ms();
         if (!submit_wait()) {
            say("vk-bench: FPK %s: the pass failed", modes[m]);
            continue;
         }
         const double ms = now_ms() - t0;
         unsigned differ = 0;
         for (unsigned y = 0; y < H; y++) {
            const uint32_t *row = (const uint32_t *)(pixels + y * layout.rowPitch);
            for (unsigned x = 0; x < W; x++)
               differ += row[x] != ref[k][(size_t)y * W + x];
         }
         say("vk-bench: FPK %-7s: KEEP %-20s + 4 fills of target 0: %7u of %u pixels of target 1 differ from KEEP alone (%5.2f ms)", modes[m], keep_names[k],
             differ, W * H, ms);
      }
   }
   unsetenv("PANVK_FPK");
   free(ref[0]);
   free(ref[1]);
}

typedef struct {
   VkImage image;
   VkImageView view;
   VkDeviceMemory memory;
   const uint8_t *pixels;
   VkSubresourceLayout layout;
} Readable;

static bool make_readable(Readable *r)
{
   void *map = NULL;
   if (!make_image(VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   &r->image, &r->view, &r->memory) ||
       vk.MapMemory(gdev, r->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
      return false;
   const VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
   vk.GetImageSubresourceLayout(gdev, r->image, &sub, &r->layout);
   r->pixels = (const uint8_t *)map + r->layout.offset;
   return true;
}

/* One render pass over two readable targets, both cleared to zero and stored: a draw of VICTIM, then NKILL draws of KILLER. */
static void record_pair(VkPipeline victim, VkPipeline killer, unsigned nkill, const Readable *t0, const Readable *t1)
{
   vk.ResetCommandBuffer(gcmd, 0);
   const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   vk.BeginCommandBuffer(gcmd, &bi);
   const VkImageSubresourceRange all = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
   VkImageMemoryBarrier to[2];
   for (unsigned i = 0; i < 2; i++)
      to[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                     .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = i ? t1->image : t0->image, .subresourceRange = all};
   vk.CmdPipelineBarrier(gcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 2, to);
   const VkViewport viewport = {0, 0, W, H, 0.0f, 1.0f};
   const VkRect2D full = {{0, 0}, {W, H}};
   const VkRenderingAttachmentInfo att[2] = {
      {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t0->view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}},
      {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = t1->view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
   const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 2,
                               .pColorAttachments = att};
   vk.CmdBeginRendering(gcmd, &ri);
   vk.CmdSetViewport(gcmd, 0, 1, &viewport);
   vk.CmdSetScissor(gcmd, 0, 1, &full);
   vk.CmdBindPipeline(gcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, victim);
   vk.CmdDraw(gcmd, 3, 1, 0, 0);
   vk.CmdBindPipeline(gcmd, VK_PIPELINE_BIND_POINT_GRAPHICS, killer);
   for (unsigned f = 0; f < nkill; f++)
      vk.CmdDraw(gcmd, 3, 1, 0, 0);
   vk.CmdEndRendering(gcmd);
   vk.EndCommandBuffer(gcmd);
}

/* The best of 3 submissions of the recorded command buffer, in milliseconds. */
static double time_once(void)
{
   double best = 1e30;
   for (unsigned r = 0; r < 3; r++) {
      const double t0 = now_ms();
      if (!submit_wait())
         return -1;
      const double t = now_ms() - t0;
      if (t < best)
         best = t;
   }
   return best;
}

/* Which targets does a draw have to write for the hardware to let it kill an earlier draw's fragments? A heavy VICTIM writes the targets of a set
 * (0, 1 or both), then four constant KILLERs write the targets of another. In "bound" mode a killer that leaves a bound target alone never kills; in the
 * default mode the driver lets it try, and the hardware decides. Where the killer leaves a target alone that the victim wrote, a kill would lose the
 * victim's write there: the target is then compared with the one the victim leaves alone. A victim that is killed shows in the time (the heavy draw is
 * not done), one that is not, in the time it takes plus nothing lost. */
static void fpk_matrix(VkShaderModule vs, VkShaderModule f2, VkShaderModule h2, const Readable *t0, const Readable *t1)
{
   static const char *const set_names[3] = {"{0}  ", "{1}  ", "{0,1}"};
   static const uint32_t set_masks[3][2] = {{0xf, 0}, {0, 0xf}, {0xf, 0xf}};
   static const char *const modes[] = {"bound", "default"};
   VkPipeline victim[3], killer[3];
   for (unsigned i = 0; i < 3; i++) {
      victim[i] = make_pipe(vs, h2, 2, set_masks[i][0], set_masks[i][1], false);
      killer[i] = make_pipe(vs, f2, 2, set_masks[i][0], set_masks[i][1], false);
      if (!victim[i] || !killer[i])
         return;
   }
   uint32_t *ref[2] = {malloc((size_t)W * H * 4), malloc((size_t)W * H * 4)};
   if (!ref[0] || !ref[1])
      return;
   const Readable *t[2] = {t0, t1};
   say("vk-bench: kill matrix: a heavy victim draw writes the targets of one set, 4 constant killers the targets of another (ms for the pass; targets "
       "only the victim writes: pixels that differ from the victim alone)");
   for (unsigned m = 0; m < 2; m++) {
      set_fpk(modes[m]);
      for (unsigned v = 0; v < 3; v++) {
         record_pair(victim[v], killer[0], 0, t0, t1);
         const double alone = time_once();
         for (unsigned k = 0; k < 2; k++)
            for (unsigned y = 0; y < H; y++)
               memcpy(ref[k] + (size_t)y * W, t[k]->pixels + y * t[k]->layout.rowPitch, W * 4);
         for (unsigned kk = 0; kk < 3; kk++) {
            record_pair(victim[v], killer[kk], 4, t0, t1);
            const double ms = time_once();
            char only[64] = "";
            for (unsigned k = 0; k < 2; k++) {
               if (!set_masks[v][k] || set_masks[kk][k])
                  continue;
               unsigned differ = 0;
               for (unsigned y = 0; y < H; y++) {
                  const uint32_t *row = (const uint32_t *)(t[k]->pixels + y * t[k]->layout.rowPitch);
                  for (unsigned x = 0; x < W; x++)
                     differ += row[x] != ref[k][(size_t)y * W + x];
               }
               snprintf(only + strlen(only), sizeof only - strlen(only), " target %u: %u of %u differ", k, differ, W * H);
            }
            say("vk-bench: FPK %-7s victim %s killers %s: %6.2f ms (victim alone %5.2f ms);%s", modes[m], set_names[v], set_names[kk], ms, alone,
                only[0] ? only : " no target only the victim writes");
         }
      }
   }
   unsetenv("PANVK_FPK");
   free(ref[0]);
   free(ref[1]);
}

int vk_bench(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkQueue queue, uint32_t family, const VkPhysicalDeviceMemoryProperties *mem)
{
#define L(name)                                                                         \
   vk.name = (PFN_vk##name)gdpa(dev, "vk" #name);                                       \
   if (!vk.name) {                                                                      \
      say("vk-bench: the device has no vk%s", #name);                                   \
      return 1;                                                                         \
   }
   L(CreateImage) L(GetImageMemoryRequirements) L(AllocateMemory) L(BindImageMemory) L(CreateImageView) L(CreateShaderModule)
   L(CreatePipelineLayout) L(CreateGraphicsPipelines) L(CreateCommandPool) L(AllocateCommandBuffers) L(BeginCommandBuffer) L(EndCommandBuffer)
   L(ResetCommandBuffer) L(CmdPipelineBarrier) L(CmdBeginRendering) L(CmdEndRendering) L(CmdBindPipeline) L(CmdSetViewport) L(CmdSetScissor)
   L(CmdDraw) L(CreateFence) L(ResetFences) L(QueueSubmit) L(WaitForFences) L(MapMemory) L(GetImageSubresourceLayout)
#undef L
   gdev = dev;
   gqueue = queue;
   gmem = mem;

   const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   VkImage img0, img1, imgl;
   VkImageView view0, view1, viewl;
   if (!make_image(VK_IMAGE_TILING_OPTIMAL, color, 0, &img0, &view0, NULL) ||
       !make_image(VK_IMAGE_TILING_OPTIMAL, color | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, 0, &img1, &view1, NULL)) {
      say("vk-bench: no images");
      return 1;
   }
   const bool have_linear = make_image(VK_IMAGE_TILING_LINEAR, color, 0, &imgl, &viewl, NULL);
   VkImage imgk;
   VkImageView viewk;
   VkDeviceMemory memk;
   const bool have_keep = make_image(VK_IMAGE_TILING_LINEAR, color, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &imgk, &viewk, &memk);
   say("vk-bench: %ux%u BGRA8 targets: optimal + transient%s", W, H, have_linear ? " + linear" : " (no linear one: the driver refuses it)");

   const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   if (vk.CreatePipelineLayout(dev, &plci, NULL, &gpl) != VK_SUCCESS)
      return 1;
   const VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = family};
   VkCommandPool pool;
   if (vk.CreateCommandPool(dev, &cpci, NULL, &pool) != VK_SUCCESS)
      return 1;
   const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   if (vk.AllocateCommandBuffers(dev, &cbai, &gcmd) != VK_SUCCESS)
      return 1;
   const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   if (vk.CreateFence(dev, &fci, NULL, &gfence) != VK_SUCCESS)
      return 1;

   VkShaderModule vs = make_shader(kVert, sizeof kVert), f1 = make_shader(kFrag1, sizeof kFrag1), f2 = make_shader(kFrag2, sizeof kFrag2),
                  h1 = make_shader(kFragH1, sizeof kFragH1), h2 = make_shader(kFragH2, sizeof kFragH2), fb = make_shader(kFragB, sizeof kFragB);
   say("vk-bench: compiling the pipelines ...");
   const Config cfg[] = {
      {"1 target, constant colour          ", make_pipe(vs, f1, 1, 0xf, 0xf, false), 1, false},
      {"1 target, constant colour, linear  ", make_pipe(vs, f1, 1, 0xf, 0xf, false), 1, true},
      {"1 target, source-over blend        ", make_pipe(vs, fb, 1, 0xf, 0xf, true), 1, false},
      {"2 targets, writes both, #1 masked  ", make_pipe(vs, f2, 2, 0xf, 0x0, false), 2, false},
      {"2 targets, writes #0 only, #1 mask ", make_pipe(vs, f1, 2, 0xf, 0x0, false), 2, false},
      {"2 targets, writes both, #1 written ", make_pipe(vs, f2, 2, 0xf, 0xf, false), 2, false},
      {"1 target, ALU-heavy shader         ", make_pipe(vs, h1, 1, 0xf, 0xf, false), 1, false},
      {"2 targets, ALU-heavy, #1 masked    ", make_pipe(vs, h2, 2, 0xf, 0x0, false), 2, false},
      {"2 targets, ALU-heavy, #1 masked, lin", make_pipe(vs, h2, 2, 0xf, 0x0, false), 2, true},
   };
   say("vk-bench: pipelines ok; ms per render pass (%u passes per command buffer, best of %u), by fills per pass", PASSES, REPS);
   const VkRect2D full = {{0, 0}, {W, H}};
   const VkRect2D quarter = {{0, 0}, {W / 2, H / 2}};
   const unsigned fills[] = {0, 1, 2, 4, 8};
   for (unsigned c = 0; c < sizeof cfg / sizeof cfg[0]; c++) {
      if (!cfg[c].pipe || (cfg[c].linear && !have_linear)) {
         say("vk-bench: %s: skipped", cfg[c].name);
         continue;
      }
      double ms[5];
      for (unsigned k = 0; k < 5; k++) {
         record(cfg[c].pipe, cfg[c].linear ? viewl : view0, view1, cfg[c].linear ? imgl : img0, img1, cfg[c].rts, fills[k], full,
                VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
         ms[k] = run();
      }
      say("vk-bench: %s  fills 0: %6.3f  1: %6.3f  2: %6.3f  4: %6.3f  8: %6.3f   per fill %5.3f ms (%4.2f Gpx/s)", cfg[c].name, ms[0], ms[1], ms[2], ms[3],
          ms[4], (ms[4] - ms[0]) / 8, W * (double)H * 8 / ((ms[4] - ms[0]) * 1e6));
   }
   if (have_keep)
      fpk_section(vs, f1, h1, h2, view0, view1, img0, img1, viewk, imgk, memk);
   else
      say("vk-bench: no host-readable linear target: the forward pixel kill check is skipped");
   Readable ra, rb;
   if (make_readable(&ra) && make_readable(&rb))
      fpk_matrix(vs, f2, h2, &ra, &rb);
   else
      say("vk-bench: no two host-readable linear targets: the kill matrix is skipped");
   /* the pass itself: what the load and the store of target 0 cost with nothing drawn */
   static const struct {
      const char *name;
      VkAttachmentLoadOp load;
      VkAttachmentStoreOp store;
   } floors[] = {
      {"load + store", VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE},
      {"clear + store", VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE},
      {"dont-care + store", VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE},
      {"load + dont-care", VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE},
   };
   for (unsigned k = 0; k < sizeof floors / sizeof floors[0]; k++) {
      record(cfg[0].pipe, view0, view1, img0, img1, 1, 0, full, floors[k].load, floors[k].store);
      const double a = run();
      record(cfg[0].pipe, view0, view1, img0, img1, 1, 1, full, floors[k].load, floors[k].store);
      const double b = run();
      say("vk-bench: pass floor, target 0 %-18s: %6.3f ms with no draw, %6.3f ms with one fill", floors[k].name, a, b);
   }
   /* a quarter of the area: does a fill cost what it covers? */
   record(cfg[0].pipe, view0, view1, img0, img1, 1, 8, quarter, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
   const double q8 = run();
   record(cfg[0].pipe, view0, view1, img0, img1, 1, 0, quarter, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
   const double q0 = run();
   say("vk-bench: a quarter of the target: 0 fills %6.3f ms, 8 fills %6.3f ms (%5.3f ms per fill)", q0, q8, (q8 - q0) / 8);
   say("vk-bench: done");
   return 0;
}
