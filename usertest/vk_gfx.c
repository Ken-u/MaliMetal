/* The graphics half of the Vulkan test: a clear and a triangle into a 64x64 RGBA8 image (render pass through dynamic rendering, the tiler, the
 * fragment job), copied into a host-visible buffer and checked pixel by pixel. What the Metal frontend's render passes come down to. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "usertest.h"

static const uint32_t kVert[] = {
#include "vk_tri_vert.inc"
};
static const uint32_t kFrag[] = {
#include "vk_tri_frag.inc"
};

#define W 64u
#define H 64u

#define G(var, name)                                                                    \
   PFN_##name var = (PFN_##name)gdpa(dev, #name);                                       \
   if (!var) {                                                                          \
      say("vk-gfx: the device has no %s", #name);                                       \
      return 1;                                                                         \
   }
#define V(call)                                                                         \
   do {                                                                                 \
      VkResult r_ = (call);                                                             \
      if (r_ != VK_SUCCESS) {                                                           \
         say("vk-gfx: %s -> VkResult %d (line %d)", #call, (int)r_, __LINE__);          \
         return 1;                                                                      \
      }                                                                                 \
   } while (0)

static uint32_t pick(const VkPhysicalDeviceMemoryProperties *mem, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mem->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mem->memoryTypes[i].propertyFlags & want) == want)
         return i;
   return UINT32_MAX;
}

int vk_gfx(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkQueue queue, uint32_t family, const VkPhysicalDeviceMemoryProperties *mem)
{
   G(create_image, vkCreateImage)
   G(get_img_reqs, vkGetImageMemoryRequirements)
   G(create_buffer, vkCreateBuffer)
   G(get_buf_reqs, vkGetBufferMemoryRequirements)
   G(alloc_mem, vkAllocateMemory)
   G(bind_img, vkBindImageMemory)
   G(bind_buf, vkBindBufferMemory)
   G(map_mem, vkMapMemory)
   G(create_view, vkCreateImageView)
   G(create_shader, vkCreateShaderModule)
   G(create_pl, vkCreatePipelineLayout)
   G(create_gp, vkCreateGraphicsPipelines)
   G(create_cpool, vkCreateCommandPool)
   G(alloc_cb, vkAllocateCommandBuffers)
   G(begin_cb, vkBeginCommandBuffer)
   G(end_cb, vkEndCommandBuffer)
   G(cmd_barrier, vkCmdPipelineBarrier)
   G(cmd_begin_rendering, vkCmdBeginRendering)
   G(cmd_end_rendering, vkCmdEndRendering)
   G(cmd_bind_pipe, vkCmdBindPipeline)
   G(cmd_viewport, vkCmdSetViewport)
   G(cmd_scissor, vkCmdSetScissor)
   G(cmd_draw, vkCmdDraw)
   G(cmd_copy, vkCmdCopyImageToBuffer)
   G(create_fence, vkCreateFence)
   G(queue_submit, vkQueueSubmit)
   G(wait_fences, vkWaitForFences)

   const VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage image;
   V(create_image(dev, &ici, NULL, &image));
   VkMemoryRequirements ireq;
   get_img_reqs(dev, image, &ireq);
   uint32_t itype = pick(mem, ireq.memoryTypeBits, 0);
   if (itype == UINT32_MAX) {
      say("vk-gfx: no memory type for the image (%#x)", ireq.memoryTypeBits);
      return 1;
   }
   const VkMemoryAllocateInfo imai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ireq.size, .memoryTypeIndex = itype};
   VkDeviceMemory imem;
   V(alloc_mem(dev, &imai, NULL, &imem));
   V(bind_img(dev, image, imem, 0));
   say("vk-gfx: image %ux%u RGBA8 (%u bytes, align %u) in memory type %u", W, H, (unsigned)ireq.size, (unsigned)ireq.alignment, itype);

   const VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
   VkImageView view;
   V(create_view(dev, &vci, NULL, &view));

   const VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer;
   V(create_buffer(dev, &bci, NULL, &buffer));
   VkMemoryRequirements breq;
   get_buf_reqs(dev, buffer, &breq);
   const uint32_t btype = pick(mem, breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (btype == UINT32_MAX) {
      say("vk-gfx: no host memory type for the readback");
      return 1;
   }
   const VkMemoryAllocateInfo bmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = breq.size, .memoryTypeIndex = btype};
   VkDeviceMemory bmem;
   V(alloc_mem(dev, &bmai, NULL, &bmem));
   V(bind_buf(dev, buffer, bmem, 0));
   volatile uint32_t *px = NULL;
   V(map_mem(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
   for (uint32_t i = 0; i < W * H; i++)
      px[i] = 0xdeadbeef;

   const VkShaderModuleCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof kVert, .pCode = kVert};
   const VkShaderModuleCreateInfo fs = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof kFrag, .pCode = kFrag};
   VkShaderModule vmod, fmod;
   V(create_shader(dev, &vs, NULL, &vmod));
   V(create_shader(dev, &fs, NULL, &fmod));
   const VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl;
   V(create_pl(dev, &plci, NULL, &pl));

   const VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vmod, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = "main"}};
   const VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   const VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   const VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
   const VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
   const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   const VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                                   .pAttachments = &cba};
   const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
   const VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   const VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
   const VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = stages,
                                              .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                              .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = pl};
   VkPipeline pipe;
   say("vk-gfx: compiling the graphics pipeline ...");
   V(create_gp(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
   say("vk-gfx: pipeline ok");

   const VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
   VkCommandPool pool;
   V(create_cpool(dev, &cpci, NULL, &pool));
   const VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
   VkCommandBuffer cmd;
   V(alloc_cb(dev, &cbai, &cmd));
   const VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   V(begin_cb(cmd, &cbbi));

   const VkImageSubresourceRange all = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
   VkImageMemoryBarrier to_color = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                                    .subresourceRange = all};
   cmd_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &to_color);

   const VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                          .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = {.color = {.float32 = {0.0f, 0.0f, 1.0f, 1.0f}}}};
   const VkRenderingInfo rinfo = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {W, H}}, .layerCount = 1, .colorAttachmentCount = 1,
                                  .pColorAttachments = &att};
   cmd_begin_rendering(cmd, &rinfo);
   const VkViewport viewport = {0, 0, W, H, 0.0f, 1.0f};
   const VkRect2D scissor = {{0, 0}, {W, H}};
   cmd_bind_pipe(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   cmd_viewport(cmd, 0, 1, &viewport);
   cmd_scissor(cmd, 0, 1, &scissor);
   cmd_draw(cmd, 3, 1, 0, 0);
   cmd_end_rendering(cmd);

   VkImageMemoryBarrier to_src = to_color;
   to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   cmd_barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
   const VkBufferImageCopy copy = {.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1}, .imageExtent = {W, H, 1}};
   cmd_copy(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
   const VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   cmd_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
   V(end_cb(cmd));

   const VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   V(create_fence(dev, &fci, NULL, &fence));
   const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
   say("vk-gfx: vkQueueSubmit ...");
   V(queue_submit(queue, 1, &si, fence));
   VkResult r = wait_fences(dev, 1, &fence, VK_TRUE, 20ull * 1000000000ull);
   say("vk-gfx: vkWaitForFences -> %d", (int)r);
   if (r != VK_SUCCESS)
      return 1;

   /* RGBA8 bytes ff 00 00 ff (red) inside the triangle, 00 00 ff ff (blue) outside; the diagonal's pixels are on the edge and skipped. */
   unsigned bad = 0, red = 0, blue = 0, other = 0;
   for (uint32_t y = 0; y < H; y++)
      for (uint32_t x = 0; x < W; x++) {
         const uint32_t v = px[y * W + x];
         if (x + y == 63)
            continue;
         const bool inside = x + y < 63;
         const uint32_t want = inside ? 0xff0000ffu : 0xffff0000u;
         if (v == 0xff0000ffu)
            red++;
         else if (v == 0xffff0000u)
            blue++;
         else
            other++;
         if (v != want) {
            if (bad < 8)
               say("vk-gfx: pixel (%u,%u) = %#x, wanted %#x", x, y, v, want);
            bad++;
         }
      }
   say("vk-gfx: %u red, %u blue, %u other; %u of %u pixels wrong", red, blue, other, bad, W * H - 64);
   return bad ? 1 : 0;
}
