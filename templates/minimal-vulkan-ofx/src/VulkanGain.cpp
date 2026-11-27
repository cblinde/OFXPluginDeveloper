// SPDX-License-Identifier: BSD-3-Clause
#include "VulkanGain.h"

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "gain_spv.h"

namespace vkgain {
namespace {

// 256 pixels per workgroup, matching `layout(local_size_x = 256)` in
// shaders/gain.comp. A mismatch is not a compile error anywhere: the shader
// would simply cover the wrong slice of the frame and leave the tail untouched,
// which is why the suite dispatches an odd pixel count (a 61-pixel frame) and
// compares every one of them.
constexpr unsigned int kLocalSize = 256;

// Push constants: the gain and the pixel count. 8 bytes, both 4-byte aligned.
struct PushConstants {
    float gain;
    unsigned int count;
};

const char *vkName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VK_ERROR_<other>";
    }
}

void *openLoader()
{
#ifdef _WIN32
    return (void *)LoadLibraryW(L"vulkan-1.dll");
#else
    // Linux and macOS: the ICD loader is what the driver installs.
    return dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
}

void *symbol(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

void closeLoader(void *lib)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

double nowMs()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

} // namespace

struct Engine::Impl {
    // Loader and the entry points this file calls. Loading them by hand keeps
    // vulkan-1.lib out of the build: the bundle links nothing Vulkan, so a
    // machine without a Vulkan driver fails here at run time with a reason
    // (`reason()`), not at load time with a missing DLL.
    void *loader = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    unsigned int family = 0;

    // Which arrangement the buffers below are in. Switching tears both down and
    // builds the other one: the suite switches on purpose, a plug-in never does.
    Memory mode = Memory::Auto;
    bool buffersBuilt = false;

    // Both modes: the buffer the shader reads and writes.
    VkBuffer storage = VK_NULL_HANDLE;
    VkDeviceMemory storageMemory = VK_NULL_HANDLE;
    // Device-local mode only: the host-visible half of the transfer.
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // staging in device-local mode, storage in host mode
    bool stagingIsSharing = false; // host memory is the staging buffer, not the storage

    unsigned int deviceLocalType = UINT32_MAX;
    unsigned int hostType = UINT32_MAX;
    unsigned int maxGroups = 0;

    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties = nullptr;

    PFN_vkGetDeviceQueue getQueue = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements bufferRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBuffer = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateShaderModule createShader = nullptr;
    PFN_vkDestroyShaderModule destroyShader = nullptr;
    PFN_vkCreateDescriptorSetLayout createSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroySetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines createCompute = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool createPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateSet = nullptr;
    PFN_vkUpdateDescriptorSets updateSets = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommands = nullptr;
    PFN_vkResetCommandBuffer resetCommand = nullptr;
    PFN_vkBeginCommandBuffer beginCommand = nullptr;
    PFN_vkCmdBindPipeline bindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets bindSet = nullptr;
    PFN_vkCmdPushConstants pushConstants = nullptr;
    PFN_vkCmdDispatch dispatch = nullptr;
    PFN_vkCmdCopyBuffer copyBuffer = nullptr;
    PFN_vkCmdPipelineBarrier pipelineBarrier = nullptr;
    PFN_vkEndCommandBuffer endCommand = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
};

Engine::Engine()
    : _impl(new Impl)
{
}

Engine::~Engine()
{
    destroy();
    delete _impl;
    _impl = nullptr;
}

bool Engine::ready()
{
    if (_impl->device != VK_NULL_HANDLE) {
        return true;
    }
    if (!_reason.empty()) {
        return false; // a failed attempt is not retried every frame
    }
    return create();
}

unsigned int Engine::maxPixels()
{
    if (!ready()) {
        return 0;
    }
    return _impl->maxGroups * kLocalSize;
}

bool Engine::create()
{
    Impl &d = *_impl;

    d.loader = openLoader();
    if (d.loader == nullptr) {
        _reason = "no Vulkan loader (vulkan-1.dll / libvulkan.so.1) - the driver does not provide one";
        return false;
    }
    d.gipa = (PFN_vkGetInstanceProcAddr)symbol(d.loader, "vkGetInstanceProcAddr");
    if (d.gipa == nullptr) {
        _reason = "vulkan-1.dll has no vkGetInstanceProcAddr";
        return false;
    }

    // 1.0 is enough for a compute pipeline, and older drivers answer
    // vkEnumerateInstanceVersion with nothing at all, so it is looked up and
    // treated as optional.
    unsigned int instanceVersion = VK_API_VERSION_1_0;
    if (PFN_vkEnumerateInstanceVersion enumerateVersion =
            (PFN_vkEnumerateInstanceVersion)d.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")) {
        enumerateVersion(&instanceVersion);
    }

    PFN_vkCreateInstance createInstance =
        (PFN_vkCreateInstance)d.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (createInstance == nullptr) {
        _reason = "vkCreateInstance missing from the loader";
        return false;
    }
    d.destroyInstance = (PFN_vkDestroyInstance)d.gipa(VK_NULL_HANDLE, "vkDestroyInstance");

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "minimal-vulkan-ofx";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "minimal-vulkan-ofx";
    appInfo.apiVersion = instanceVersion < VK_API_VERSION_1_1 ? instanceVersion : VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    VkResult result = createInstance(&instanceInfo, nullptr, &d.instance);
    if (result != VK_SUCCESS) {
        _reason = std::string("vkCreateInstance failed: ") + vkName(result);
        return false;
    }

    PFN_vkEnumeratePhysicalDevices enumerateDevices =
        (PFN_vkEnumeratePhysicalDevices)d.gipa(d.instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)d.gipa(d.instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)d.gipa(d.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice createDevice =
        (PFN_vkCreateDevice)d.gipa(d.instance, "vkCreateDevice");
    d.memoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)d.gipa(d.instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!enumerateDevices || !properties || !families || !createDevice || !d.memoryProperties) {
        _reason = "the Vulkan loader is missing instance entry points";
        return false;
    }

    unsigned int deviceCount = 0;
    if (enumerateDevices(d.instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0) {
        _reason = "no Vulkan physical device (no driver installed for this GPU)";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    enumerateDevices(d.instance, &deviceCount, devices.data());

    // First device with a compute queue wins; a machine with two GPUs gets the
    // one the driver lists first, which is what a hostless device choice can do
    // honestly. Choosing by name or by discrete-vs-integrated is a policy, and
    // policies belong to whoever ships the plug-in.
    for (unsigned int i = 0; i < deviceCount && d.physical == VK_NULL_HANDLE; ++i) {
        unsigned int familyCount = 0;
        families(devices[i], &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
        families(devices[i], &familyCount, queueFamilies.data());
        for (unsigned int f = 0; f < familyCount; ++f) {
            if (queueFamilies[f].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                d.physical = devices[i];
                d.family = f;
                break;
            }
        }
    }
    if (d.physical == VK_NULL_HANDLE) {
        _reason = "no Vulkan device with a compute queue";
        return false;
    }

    VkPhysicalDeviceProperties props{};
    properties(d.physical, &props);
    d.maxGroups = props.limits.maxComputeWorkGroupCount[0];

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = d.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    result = createDevice(d.physical, &deviceInfo, nullptr, &d.device);
    if (result != VK_SUCCESS) {
        _reason = std::string("vkCreateDevice failed: ") + vkName(result);
        return false;
    }

    d.gdpa = (PFN_vkGetDeviceProcAddr)d.gipa(d.instance, "vkGetDeviceProcAddr");
    d.destroyDevice = (PFN_vkDestroyDevice)d.gipa(d.instance, "vkDestroyDevice");
    if (d.gdpa == nullptr) {
        _reason = "no vkGetDeviceProcAddr";
        return false;
    }

    struct Entry {
        const char *name;
        void **slot;
    };
    const Entry entries[] = {
        {"vkGetDeviceQueue", (void **)&d.getQueue},
        {"vkCreateBuffer", (void **)&d.createBuffer},
        {"vkDestroyBuffer", (void **)&d.destroyBuffer},
        {"vkGetBufferMemoryRequirements", (void **)&d.bufferRequirements},
        {"vkAllocateMemory", (void **)&d.allocateMemory},
        {"vkFreeMemory", (void **)&d.freeMemory},
        {"vkBindBufferMemory", (void **)&d.bindBuffer},
        {"vkMapMemory", (void **)&d.mapMemory},
        {"vkUnmapMemory", (void **)&d.unmapMemory},
        {"vkCreateShaderModule", (void **)&d.createShader},
        {"vkDestroyShaderModule", (void **)&d.destroyShader},
        {"vkCreateDescriptorSetLayout", (void **)&d.createSetLayout},
        {"vkDestroyDescriptorSetLayout", (void **)&d.destroySetLayout},
        {"vkCreatePipelineLayout", (void **)&d.createPipelineLayout},
        {"vkDestroyPipelineLayout", (void **)&d.destroyPipelineLayout},
        {"vkCreateComputePipelines", (void **)&d.createCompute},
        {"vkDestroyPipeline", (void **)&d.destroyPipeline},
        {"vkCreateDescriptorPool", (void **)&d.createPool},
        {"vkDestroyDescriptorPool", (void **)&d.destroyPool},
        {"vkAllocateDescriptorSets", (void **)&d.allocateSet},
        {"vkUpdateDescriptorSets", (void **)&d.updateSets},
        {"vkCreateCommandPool", (void **)&d.createCommandPool},
        {"vkDestroyCommandPool", (void **)&d.destroyCommandPool},
        {"vkAllocateCommandBuffers", (void **)&d.allocateCommands},
        {"vkResetCommandBuffer", (void **)&d.resetCommand},
        {"vkBeginCommandBuffer", (void **)&d.beginCommand},
        {"vkCmdBindPipeline", (void **)&d.bindPipeline},
        {"vkCmdBindDescriptorSets", (void **)&d.bindSet},
        {"vkCmdPushConstants", (void **)&d.pushConstants},
        {"vkCmdDispatch", (void **)&d.dispatch},
        {"vkCmdCopyBuffer", (void **)&d.copyBuffer},
        {"vkCmdPipelineBarrier", (void **)&d.pipelineBarrier},
        {"vkEndCommandBuffer", (void **)&d.endCommand},
        {"vkQueueSubmit", (void **)&d.submit},
        {"vkCreateFence", (void **)&d.createFence},
        {"vkDestroyFence", (void **)&d.destroyFence},
        {"vkWaitForFences", (void **)&d.waitFences},
        {"vkResetFences", (void **)&d.resetFences},
    };
    for (const Entry &entry : entries) {
        *entry.slot = (void *)d.gdpa(d.device, entry.name);
        if (*entry.slot == nullptr) {
            _reason = std::string("device entry point missing: ") + entry.name;
            return false;
        }
    }

    d.getQueue(d.device, d.family, 0, &d.queue);

    // Both memory types are resolved once, from the device's own table and the
    // buffer's own requirements (asked again when a buffer is created, because
    // requirements depend on size and usage).
    VkPhysicalDeviceMemoryProperties memory{};
    d.memoryProperties(d.physical, &memory);
    for (unsigned int i = 0; i < memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (d.hostType == UINT32_MAX &&
            (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            d.hostType = i;
        }
        if (d.deviceLocalType == UINT32_MAX && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            d.deviceLocalType = i;
        }
    }
    if (d.deviceLocalType == UINT32_MAX) {
        // An integrated GPU often has one memory type that is both. It is still
        // device-local in the sense that matters, but the staging copy then has
        // nothing to do; `memory()` says which arrangement ran.
        for (unsigned int i = 0; i < memory.memoryTypeCount; ++i) {
            if ((memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                d.deviceLocalType = i;
                break;
            }
        }
    }

    // Descriptor set layout: one storage buffer, bound at 0, compute stage only.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (d.createSetLayout(d.device, &setInfo, nullptr, &d.setLayout) != VK_SUCCESS) {
        _reason = "vkCreateDescriptorSetLayout failed";
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &d.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (d.createPipelineLayout(d.device, &layoutInfo, nullptr, &d.pipelineLayout) != VK_SUCCESS) {
        _reason = "vkCreatePipelineLayout failed";
        return false;
    }

    // The SPIR-V is embedded at build time (gain_spv.h), so the bundle carries
    // no .spv file to lose and no shader compiler at run time.
    VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kGainSpv);
    shaderInfo.pCode = reinterpret_cast<const uint32_t *>(kGainSpv);
    VkShaderModule shader = VK_NULL_HANDLE;
    if (d.createShader(d.device, &shaderInfo, nullptr, &shader) != VK_SUCCESS) {
        _reason = "vkCreateShaderModule rejected the embedded SPIR-V";
        return false;
    }
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = d.pipelineLayout;
    result = d.createCompute(d.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &d.pipeline);
    d.destroyShader(d.device, shader, nullptr);
    if (result != VK_SUCCESS) {
        _reason = std::string("vkCreateComputePipelines failed: ") + vkName(result);
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (d.createPool(d.device, &poolInfo, nullptr, &d.pool) != VK_SUCCESS) {
        _reason = "vkCreateDescriptorPool failed";
        return false;
    }
    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = d.pool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &d.setLayout;
    if (d.allocateSet(d.device, &allocateInfo, &d.set) != VK_SUCCESS) {
        _reason = "vkAllocateDescriptorSets failed";
        return false;
    }

    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = d.family;
    if (d.createCommandPool(d.device, &commandPoolInfo, nullptr, &d.commandPool) != VK_SUCCESS) {
        _reason = "vkCreateCommandPool failed";
        return false;
    }
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = d.commandPool;
    commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandInfo.commandBufferCount = 1;
    if (d.allocateCommands(d.device, &commandInfo, &d.commands) != VK_SUCCESS) {
        _reason = "vkAllocateCommandBuffers failed";
        return false;
    }

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (d.createFence(d.device, &fenceInfo, nullptr, &d.fence) != VK_SUCCESS) {
        _reason = "vkCreateFence failed";
        return false;
    }

    char label[256];
    std::snprintf(label, sizeof(label), "%s (Vulkan %u.%u.%u, queue family %u, %u px per dispatch)",
                  props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), d.family,
                  d.maxGroups * kLocalSize);
    _device = label;
    return true;
}

bool Engine::apply(float *pixels, unsigned int count, float gain, std::string &error)
{
    return apply(pixels, count, gain, Memory::Auto, error);
}

bool Engine::apply(float *pixels, unsigned int count, float gain, Memory how, std::string &error)
{
    if (!ready()) {
        error = _reason.empty() ? "no Vulkan device" : _reason;
        return false;
    }
    if (pixels == nullptr || count == 0) {
        error = "nothing to dispatch";
        return false;
    }
    if (count > maxPixels()) {
        // Saying so beats dispatching a truncated frame: the shader's guard
        // keeps the tail *it* sees correct, and nothing keeps the pixels past
        // maxComputeWorkGroupCount from being untouched.
        error = "frame needs more workgroups than this device allows; tile it";
        return false;
    }

    Impl &d = *_impl;
    const VkDeviceSize needed = (VkDeviceSize)count * sizeof(float) * 4;

    Memory chosen = how;
    if (chosen == Memory::Auto) {
        chosen = d.deviceLocalType != UINT32_MAX ? Memory::DeviceLocal : Memory::HostVisible;
    }
    if (chosen == Memory::DeviceLocal && d.deviceLocalType == UINT32_MAX) {
        chosen = Memory::HostVisible; // no device-local memory on this device at all
    }

    const bool rebuild = !d.buffersBuilt || d.mode != chosen || needed > d.size;
    if (rebuild) {
        // Grow once per size and once per arrangement: the host hands the same
        // frame size every time, so this runs on the first frame of a new size
        // and never again. Reusing a bigger buffer is cheaper than allocating
        // per frame and is what makes the per-frame path a copy, a submit and a
        // wait.
        if (d.buffersBuilt) {
            if (d.mapped != nullptr) {
                d.unmapMemory(d.device, d.stagingIsSharing ? d.storageMemory : d.stagingMemory);
                d.mapped = nullptr;
            }
            if (d.storage != VK_NULL_HANDLE) {
                d.destroyBuffer(d.device, d.storage, nullptr);
                d.freeMemory(d.device, d.storageMemory, nullptr);
                d.storage = VK_NULL_HANDLE;
                d.storageMemory = VK_NULL_HANDLE;
            }
            if (d.staging != VK_NULL_HANDLE) {
                d.destroyBuffer(d.device, d.staging, nullptr);
                d.freeMemory(d.device, d.stagingMemory, nullptr);
                d.staging = VK_NULL_HANDLE;
                d.stagingMemory = VK_NULL_HANDLE;
            }
            d.buffersBuilt = false;
            d.size = 0;
        }
        d.mode = chosen;
        d.stagingIsSharing = chosen == Memory::HostVisible;

        auto createBuffer = [&](VkDeviceSize bytes, unsigned int typeIndex, VkBuffer *buffer,
                                VkDeviceMemory *memory, const char *what) -> bool {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (d.createBuffer(d.device, &info, nullptr, buffer) != VK_SUCCESS) {
                error = std::string("vkCreateBuffer failed for the ") + what;
                return false;
            }
            VkMemoryRequirements requirements{};
            d.bufferRequirements(d.device, *buffer, &requirements);
            if ((requirements.memoryTypeBits & (1u << typeIndex)) == 0) {
                error = std::string("the ") + what + " cannot live in the chosen memory type";
                return false;
            }
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = typeIndex;
            if (d.allocateMemory(d.device, &allocation, nullptr, memory) != VK_SUCCESS) {
                error = std::string("vkAllocateMemory failed for the ") + what;
                return false;
            }
            if (d.bindBuffer(d.device, *buffer, *memory, 0) != VK_SUCCESS) {
                error = std::string("vkBindBufferMemory failed for the ") + what;
                return false;
            }
            return true;
        };

        if (chosen == Memory::DeviceLocal) {
            if (!createBuffer(needed, d.deviceLocalType, &d.storage, &d.storageMemory, "storage buffer")) {
                return false;
            }
            if (!createBuffer(needed, d.hostType, &d.staging, &d.stagingMemory, "staging buffer")) {
                return false;
            }
            if (d.mapMemory(d.device, d.stagingMemory, 0, VK_WHOLE_SIZE, 0, &d.mapped) != VK_SUCCESS) {
                error = "vkMapMemory failed for the staging buffer";
                return false;
            }
        } else {
            if (!createBuffer(needed, d.hostType, &d.storage, &d.storageMemory, "host-visible buffer")) {
                return false;
            }
            if (d.mapMemory(d.device, d.storageMemory, 0, VK_WHOLE_SIZE, 0, &d.mapped) != VK_SUCCESS) {
                error = "vkMapMemory failed for the host-visible buffer";
                return false;
            }
        }
        d.size = needed;
        d.buffersBuilt = true;

        // The descriptor points at the buffer the shader reads and writes - the
        // device-local one in that mode, never the staging copy.
        VkDescriptorBufferInfo bufferDescriptor{};
        bufferDescriptor.buffer = d.storage;
        bufferDescriptor.offset = 0;
        bufferDescriptor.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = d.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferDescriptor;
        d.updateSets(d.device, 1, &write, 0, nullptr);
    }

    _memory = d.stagingIsSharing ? "host-visible coherent memory"
                                 : "device-local storage + host-visible staging";

    auto barrier = [&](VkBuffer buffer, VkAccessFlags from, VkAccessFlags to,
                       VkPipelineStageFlags fromStage, VkPipelineStageFlags toStage) {
        VkBufferMemoryBarrier range{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        range.srcAccessMask = from;
        range.dstAccessMask = to;
        range.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        range.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        range.buffer = buffer;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        d.pipelineBarrier(d.commands, fromStage, toStage, 0, 0, nullptr, 1, &range, 0, nullptr);
    };

    // 1. the frame into memory the device can read
    const double beforeUpload = nowMs();
    std::memcpy(d.mapped, pixels, (size_t)needed);
    _stats.uploadMs = nowMs() - beforeUpload;
    _stats.bytes = (unsigned long long)needed;

    VkResult result = d.resetFences(d.device, 1, &d.fence);
    if (result != VK_SUCCESS) {
        error = std::string("vkResetFences failed: ") + vkName(result);
        return false;
    }
    result = d.resetCommand(d.commands, 0);
    if (result != VK_SUCCESS) {
        error = std::string("vkResetCommandBuffer failed: ") + vkName(result);
        return false;
    }
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.beginCommand(d.commands, &beginInfo) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed";
        return false;
    }

    // 2. the transfers and the dispatch, with the barriers between them: without
    // them the shader may read the staging buffer before the copy lands and the
    // CPU may read the frame before the dispatch writes it, which on coherent
    // memory looks like "works most of the time" rather than like a failure.
    if (!d.stagingIsSharing) {
        barrier(d.staging, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy whole{};
        whole.size = needed;
        d.copyBuffer(d.commands, d.staging, d.storage, 1, &whole);
        barrier(d.storage, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    } else {
        barrier(d.storage, VK_ACCESS_HOST_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    d.bindPipeline(d.commands, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
    d.bindSet(d.commands, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipelineLayout, 0, 1, &d.set, 0, nullptr);
    PushConstants push{gain, count};
    d.pushConstants(d.commands, d.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    sizeof(push), &push);
    d.dispatch(d.commands, (count + kLocalSize - 1) / kLocalSize, 1, 1);

    if (!d.stagingIsSharing) {
        barrier(d.storage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy whole{};
        whole.size = needed;
        d.copyBuffer(d.commands, d.storage, d.staging, 1, &whole);
        barrier(d.staging, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
    } else {
        barrier(d.storage, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
    }

    if (d.endCommand(d.commands) != VK_SUCCESS) {
        error = "vkEndCommandBuffer failed";
        return false;
    }

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &d.commands;
    const double beforeDispatch = nowMs();
    if (d.submit(d.queue, 1, &submitInfo, d.fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        return false;
    }
    // Waits on the fence, never on the device: a device-wide wait would idle
    // every other plug-in sharing the GPU (references/cuda-streams-and-memory.md
    // section 1, same rule for a Vulkan queue).
    result = d.waitFences(d.device, 1, &d.fence, VK_TRUE, UINT64_MAX);
    _stats.dispatchMs = nowMs() - beforeDispatch;
    if (result != VK_SUCCESS) {
        error = std::string("vkWaitForFences failed: ") + vkName(result);
        return false;
    }

    // 3. and back out. Coherent memory needs no invalidate, which is why the
    // memory type asks for it: a non-coherent type would need a flush/invalidate
    // pair and a decision about who owns the range.
    const double beforeDownload = nowMs();
    std::memcpy(pixels, d.mapped, (size_t)needed);
    _stats.downloadMs = nowMs() - beforeDownload;
    return true;
}

void Engine::destroy()
{
    Impl &d = *_impl;
    if (d.device != VK_NULL_HANDLE) {
        if (d.buffersBuilt && d.mapped != nullptr) {
            d.unmapMemory(d.device, d.stagingIsSharing ? d.storageMemory : d.stagingMemory);
            d.mapped = nullptr;
        }
        if (d.storage != VK_NULL_HANDLE) {
            d.destroyBuffer(d.device, d.storage, nullptr);
            d.freeMemory(d.device, d.storageMemory, nullptr);
            d.storage = VK_NULL_HANDLE;
        }
        if (d.staging != VK_NULL_HANDLE) {
            d.destroyBuffer(d.device, d.staging, nullptr);
            d.freeMemory(d.device, d.stagingMemory, nullptr);
            d.staging = VK_NULL_HANDLE;
        }
        d.destroyFence(d.device, d.fence, nullptr);
        d.destroyCommandPool(d.device, d.commandPool, nullptr);
        d.destroyPool(d.device, d.pool, nullptr);
        if (d.pipeline != VK_NULL_HANDLE) {
            d.destroyPipeline(d.device, d.pipeline, nullptr);
        }
        if (d.pipelineLayout != VK_NULL_HANDLE) {
            d.destroyPipelineLayout(d.device, d.pipelineLayout, nullptr);
        }
        if (d.setLayout != VK_NULL_HANDLE) {
            d.destroySetLayout(d.device, d.setLayout, nullptr);
        }
        d.destroyDevice(d.device, nullptr);
        d.device = VK_NULL_HANDLE;
    }
    if (d.instance != VK_NULL_HANDLE && d.destroyInstance != nullptr) {
        d.destroyInstance(d.instance, nullptr);
        d.instance = VK_NULL_HANDLE;
    }
    if (d.loader != nullptr) {
        closeLoader(d.loader);
        d.loader = nullptr;
    }
}

} // namespace vkgain
