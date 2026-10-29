#ifndef USERTEST_H
#define USERTEST_H
extern int console;
void say(const char *format, ...);
int vk_test(void);   /* vk_test.c: PanVK through dlopen; 0 when everything worked */
#ifdef VK_VERSION_1_0
int vk_gfx(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, VkQueue queue, uint32_t family, const VkPhysicalDeviceMemoryProperties *mem);   /* vk_gfx.c */
#endif
#endif
