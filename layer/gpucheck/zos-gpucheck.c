/*
 * zos-gpucheck: what the Linux layer's graphics drivers see, one line each.
 *
 *     vulkan NAME|DRIVER|TYPE      for every Vulkan device
 *     opengl RENDERER|VERSION      OpenGL through EGL, without a window
 *     error WHAT                   when one of them can't start
 *
 * The Steam app runs it through layerrun to show which GPU games will use,
 * and it is the quickest way to check a new layer. Built by layer/build.sh
 * against the layer's own Vulkan loader and libEGL.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char *device_type(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
	default: return "other";
	}
}

static int vulkan(void)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "zos-gpucheck",
				  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance inst;
	VkResult r = vkCreateInstance(&info, NULL, &inst);
	if (r != VK_SUCCESS) {
		printf("error vulkan instance (%d)\n", r);
		return 0;
	}
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(inst, &n, NULL);
	VkPhysicalDevice *devs = calloc(n ? n : 1, sizeof *devs);
	vkEnumeratePhysicalDevices(inst, &n, devs);
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
		VkPhysicalDeviceProperties2 p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
		vkGetPhysicalDeviceProperties2(devs[i], &p);
		printf("vulkan %s|%s|%s\n", p.properties.deviceName, drv.driverName[0] ? drv.driverName : "?",
		       device_type(p.properties.deviceType));
	}
	free(devs);
	vkDestroyInstance(inst, NULL);
	return (int)n;
}

typedef const unsigned char *(*GetString)(unsigned int);
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static int opengl(void)
{
	/* no window needed: Mesa's surfaceless platform, then the default one */
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	EGLint major, minor;
	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
		d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
			printf("error egl (0x%x)\n", eglGetError());
			return 0;
		}
	}
	eglBindAPI(EGL_OPENGL_API);
	EGLint attrs[] = { EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
		printf("error opengl context (0x%x)\n", eglGetError());
		eglTerminate(d);
		return 0;
	}
	GetString get = (GetString)eglGetProcAddress("glGetString");
	if (get)
		printf("opengl %s|%s\n", get(GL_RENDERER), get(GL_VERSION));
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(d, c);
	eglTerminate(d);
	return get != NULL;
}

int main(int argc, char **argv)
{
	int found = 0;
	if (argc < 2 || strcmp(argv[1], "--opengl") != 0)
		found += vulkan();
	if (argc < 2 || strcmp(argv[1], "--vulkan") != 0)
		found += opengl();
	fflush(stdout);
	return found ? 0 : 1;
}
