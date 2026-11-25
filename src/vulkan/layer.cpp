#include "readback.h"
#include "capture_cadence.h"
#include "frame_endpoint.h"
#include "vulkan_presence.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

struct Instance {
    std::mutex mutex;
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr dispatch = nullptr;
    PFN_GetPhysicalDeviceProcAddr physicalDispatch = nullptr;
    std::unordered_map<VkSurfaceKHR, HWND> windows;

    template<typename Function> Function command(const char* name) const {
        return reinterpret_cast<Function>(dispatch(handle, name));
    }
};

struct CaptureGateTrace {
    std::chrono::steady_clock::time_point started{};
    unsigned readerBusy = 0;
    unsigned cadenceWait = 0;
    unsigned admitted = 0;

    void record(HWND window, std::chrono::steady_clock::time_point now, bool readerReady, bool capture) {
        if (started == std::chrono::steady_clock::time_point{}) started = now;
        if (capture) ++admitted;
        else if (readerReady) ++cadenceWait;
        else ++readerBusy;

        if (now - started < std::chrono::seconds(1)) return;
        std::fprintf(stderr, "[Muxivo] Capture gates HWND=%p reader_busy=%u cadence_wait=%u admitted=%u\n",
            static_cast<void*>(window), readerBusy, cadenceWait, admitted);
        started = now;
        readerBusy = cadenceWait = admitted = 0;
    }
};

struct Swapchain {
    HWND window = nullptr;
    VkExtent2D extent = {};
    muxivo::FrameFormat format = muxivo::FrameFormat::Rgba8;
    bool permissionDiagnostic = false;
    std::vector<VkImage> images;
    std::unique_ptr<muxivo::VulkanFrameEndpoint> pipe;
    std::unique_ptr<muxivo::Readback> readback;
    muxivo::CaptureCadence cadence;
    CaptureGateTrace captureGateTrace;
    std::chrono::steady_clock::time_point nextPresenceCheck{};
    bool muxivoRunning = false;

    void suspend() { pipe.reset(); }
};

struct QueueInfo {
    uint32_t family;
    bool unprotected;
};

struct Device {
    std::mutex mutex;
    VkDevice handle = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr dispatch = nullptr;
    PFN_vkSetDeviceLoaderData loaderData = nullptr;
    std::shared_ptr<Instance> instance;
    VkPhysicalDeviceMemoryProperties memory = {};
    std::vector<VkQueueFamilyProperties> families;
    std::unordered_map<VkQueue, QueueInfo> queues;
    std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;

    template<typename Function> Function command(const char* name) const {
        return reinterpret_cast<Function>(dispatch(handle, name));
    }
};

struct LayerGlobals {
    std::mutex stateMutex;
    std::unordered_map<void*, std::shared_ptr<Instance>> instances;
    std::unordered_map<void*, std::shared_ptr<Device>> devices;
};

LayerGlobals& globals() {
    // Intentionally immortal: the Vulkan layer may stay loaded through process teardown,
    // and destroying ICD handles or overlapped pipes under DLL_PROCESS_DETACH risks loader-lock deadlock/crash.
    static auto* g = new LayerGlobals();
    return *g;
}

std::mutex& stateMutex = globals().stateMutex;
std::unordered_map<void*, std::shared_ptr<Instance>>& instances = globals().instances;
std::unordered_map<void*, std::shared_ptr<Device>>& devices = globals().devices;

bool traceEnabled() {
    static const bool enabled = GetEnvironmentVariableW(L"MUXIVO_VULKAN_CAPTURE_TRACE", nullptr, 0) != 0;
    return enabled;
}

// Dispatchable handles for one logical device or instance share the loader's dispatch key.
template<typename Handle> void* dispatchKey(Handle handle) {
    return handle ? *reinterpret_cast<void* const*>(handle) : nullptr;
}

template<typename State, typename Handle>
std::shared_ptr<State> findState(const std::unordered_map<void*, std::shared_ptr<State>>& table, Handle handle) {
    const auto it = table.find(dispatchKey(handle));
    return it == table.end() ? nullptr : it->second;
}

template<typename Info, typename Create>
Info* layerInfo(const Create* create, VkStructureType type, VkLayerFunction function) {
    for (auto* entry = static_cast<const VkBaseInStructure*>(create->pNext); entry; entry = entry->pNext) {
        if (entry->sType != type) continue;
        auto* info = reinterpret_cast<const Info*>(entry);
        if (info->function == function) return const_cast<Info*>(info);
    }
    return nullptr;
}

bool windowPermitsCapture(HWND window) {
    DWORD owner = 0;
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)
        || !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId()) {
        return false;
    }
    const HWND root = GetAncestor(window, GA_ROOT);
    if (!root || IsIconic(root)) return false;
    DWORD affinity = WDA_NONE;
    return GetWindowDisplayAffinity(root, &affinity) && affinity == WDA_NONE;
}

bool muxivoIsRunning(Swapchain& state, std::chrono::steady_clock::time_point now) {
    if (now < state.nextPresenceCheck) return state.muxivoRunning;
    state.nextPresenceCheck = now + std::chrono::milliseconds(250);
    const HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, muxivoVulkanPresenceEvent);
    state.muxivoRunning = event != nullptr;
    if (event) CloseHandle(event);
    return state.muxivoRunning;
}

VKAPI_ATTR VkResult VKAPI_CALL createInstance(
    const VkInstanceCreateInfo* create, const VkAllocationCallbacks* allocator, VkInstance* handle) {
    if (!create || !handle) return VK_ERROR_INITIALIZATION_FAILED;
    auto* linkInfo = layerInfo<VkLayerInstanceCreateInfo>(
        create, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!linkInfo || !linkInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto* link = linkInfo->u.pLayerInfo;
    auto next = reinterpret_cast<PFN_vkCreateInstance>(link->pfnNextGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    linkInfo->u.pLayerInfo = link->pNext;
    const auto result = next(create, allocator, handle);
    if (result != VK_SUCCESS) return result;
    try {
        auto instance = std::make_shared<Instance>();
        instance->handle = *handle;
        instance->dispatch = link->pfnNextGetInstanceProcAddr;
        instance->physicalDispatch = link->pfnNextGetPhysicalDeviceProcAddr;
        std::lock_guard lock(stateMutex);
        instances.emplace(dispatchKey(*handle), std::move(instance));
        return VK_SUCCESS;
    } catch (...) {
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(link->pfnNextGetInstanceProcAddr(*handle, "vkDestroyInstance"));
        if (destroy) destroy(*handle, allocator);
        *handle = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance handle, const VkAllocationCallbacks* allocator) {
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, handle);
        instances.erase(dispatchKey(handle));
    }
    if (instance) instance->command<PFN_vkDestroyInstance>("vkDestroyInstance")(handle, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* create,
    const VkAllocationCallbacks* allocator, VkDevice* handle) {
    if (!create || !handle) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, physical);
    }
    auto* linkInfo = layerInfo<VkLayerDeviceCreateInfo>(
        create, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    auto* loaderInfo = layerInfo<VkLayerDeviceCreateInfo>(
        create, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LOADER_DATA_CALLBACK);
    if (!instance || !linkInfo || !linkInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto* link = linkInfo->u.pLayerInfo;
    auto next = reinterpret_cast<PFN_vkCreateDevice>(
        link->pfnNextGetInstanceProcAddr(instance->handle, "vkCreateDevice"));
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    linkInfo->u.pLayerInfo = link->pNext;
    const auto result = next(physical, create, allocator, handle);
    if (result != VK_SUCCESS) return result;
    try {
        auto device = std::make_shared<Device>();
        device->handle = *handle;
        device->dispatch = link->pfnNextGetDeviceProcAddr;
        device->loaderData = loaderInfo ? loaderInfo->u.pfnSetDeviceLoaderData : nullptr;
        device->instance = instance;
        instance->command<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
            physical, &device->memory);
        auto properties = instance->command<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t count = 0;
        properties(physical, &count, nullptr);
        device->families.resize(count);
        properties(physical, &count, device->families.data());
        std::lock_guard lock(stateMutex);
        devices.emplace(dispatchKey(*handle), std::move(device));
        return VK_SUCCESS;
    } catch (...) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(link->pfnNextGetDeviceProcAddr(*handle, "vkDestroyDevice"));
        if (destroy) destroy(*handle, allocator);
        *handle = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice handle, const VkAllocationCallbacks* allocator) {
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
        devices.erase(dispatchKey(handle));
    }
    if (!device) return;
    {
        std::lock_guard devLock(device->mutex);
        device->swapchains.clear();
    }
    device->command<PFN_vkDestroyDevice>("vkDestroyDevice")(handle, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createSurface(VkInstance handle, const VkWin32SurfaceCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (!create || !surface) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, handle);
    }
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = instance->command<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR")(
        handle, create, allocator, surface);
    if (result == VK_SUCCESS) {
        if (traceEnabled()) std::fprintf(stderr, "[Muxivo] Surface HWND %p\n", static_cast<void*>(create->hwnd));
        try {
            std::lock_guard instLock(instance->mutex);
            instance->windows.emplace(*surface, create->hwnd);
        } catch (...) { /* The game surface remains valid without capture metadata. */ }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroySurface(VkInstance handle, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator) {
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, handle);
    }
    if (!instance) return;
    {
        std::lock_guard instLock(instance->mutex);
        instance->windows.erase(surface);
    }
    instance->command<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(handle, surface, allocator);
}

VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice handle, uint32_t family, uint32_t index, VkQueue* queue) {
    if (!queue) return;
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return;
    device->command<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(handle, family, index, queue);
    if (*queue) {
        try {
            std::lock_guard devLock(device->mutex);
            device->queues.emplace(*queue, QueueInfo{ family, true });
        } catch (...) { /* Capture is optional for a queue. */ }
    }
}

VKAPI_ATTR void VKAPI_CALL getQueue2(VkDevice handle, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    if (!info || !queue) return;
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return;
    device->command<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2")(handle, info, queue);
    if (*queue) {
        try {
            std::lock_guard devLock(device->mutex);
            device->queues.emplace(*queue, QueueInfo{ info->queueFamilyIndex, !(info->flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) });
        } catch (...) { /* Capture is optional for a queue. */ }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(VkDevice handle, const VkSwapchainCreateInfoKHR* create,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    if (!create || !swapchain) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = device->command<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(handle, create, allocator, swapchain);
    // Vulkan retires oldSwapchain even when creating its replacement fails.
    // Release the old capture endpoint before the new swapchain publishes for the same HWND.
    if (create->oldSwapchain) {
        std::lock_guard devLock(device->mutex);
        if (device->swapchains.erase(create->oldSwapchain) && traceEnabled()) {
            std::fprintf(stderr, "[Muxivo] Retired Vulkan capture swapchain; replacement result=%d\n", result);
        }
    }
    if (result != VK_SUCCESS) return result;
    std::optional<HWND> window;
    if (device->instance) {
        std::lock_guard instLock(device->instance->mutex);
        const auto it = device->instance->windows.find(create->surface);
        if (it != device->instance->windows.end()) window = it->second;
    }
    const auto format = muxivo::frameFormat(create->imageFormat, create->imageColorSpace);
    if (traceEnabled()) std::fprintf(stderr,
        "[Muxivo] Swapchain %ux%u format=%d color=%d use=%x flags=%x window=%d loader=%d\n",
        create->imageExtent.width, create->imageExtent.height, create->imageFormat, create->imageColorSpace,
        create->imageUsage, create->flags, window.has_value(), device->loaderData != nullptr);
    if (!window || !device->loaderData || !format || create->imageArrayLayers != 1
        || !(create->imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || (create->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        || !create->imageExtent.width || create->imageExtent.width > 3840
        || !create->imageExtent.height || create->imageExtent.height > 2160) return result;
    try {
        Swapchain state;
        state.window = *window;
        state.extent = create->imageExtent;
        state.format = *format;
        auto images = device->command<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
        uint32_t count = 0;
        if (images(handle, *swapchain, &count, nullptr) == VK_SUCCESS && count > 0 && count <= 16) {
            state.images.resize(count);
            if (images(handle, *swapchain, &count, state.images.data()) == VK_SUCCESS) {
                std::lock_guard devLock(device->mutex);
                device->swapchains.emplace(*swapchain, std::move(state));
            }
        }
    } catch (...) { /* Preserve the game's successfully created swapchain. */ }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroySwapchain(VkDevice handle, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator) {
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return;
    {
        std::lock_guard devLock(device->mutex);
        device->swapchains.erase(swapchain);
    }
    device->command<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(handle, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL acquireExclusive(VkDevice handle, VkSwapchainKHR swapchain) {
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    const auto result = device->command<PFN_vkAcquireFullScreenExclusiveModeEXT>("vkAcquireFullScreenExclusiveModeEXT")(
        handle, swapchain);
    if (traceEnabled()) {
        std::lock_guard devLock(device->mutex);
        const auto tracked = device->swapchains.find(swapchain);
        std::fprintf(stderr, "[Muxivo] Acquire exclusive result=%d tracked=%d\n",
            result, tracked != device->swapchains.end());
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL releaseExclusive(VkDevice handle, VkSwapchainKHR swapchain) {
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    {
        std::lock_guard devLock(device->mutex);
        const auto tracked = device->swapchains.find(swapchain);
        if (tracked != device->swapchains.end()) tracked->second.suspend();
    }
    return device->command<PFN_vkReleaseFullScreenExclusiveModeEXT>("vkReleaseFullScreenExclusiveModeEXT")(handle, swapchain);
}

bool prepareCapture(Device& device, Swapchain& state, const QueueInfo& queue) {
    if (!windowPermitsCapture(state.window)) {
        if (traceEnabled() && !state.permissionDiagnostic) {
            state.permissionDiagnostic = true;
            DWORD affinity = 0;
            const HWND root = GetAncestor(state.window, GA_ROOT);
            const BOOL readable = root ? GetWindowDisplayAffinity(root, &affinity) : FALSE;
            const DWORD error = readable ? ERROR_SUCCESS : GetLastError();
            std::fprintf(stderr, "[Muxivo] Capture guard HWND=%p visible=%d iconic=%d affinity=%lx error=%lu\n",
                static_cast<void*>(state.window),
                IsWindowVisible(state.window), IsIconic(state.window), affinity, error);
        }
        state.suspend();
        return false;
    }
    if (!queue.unprotected || queue.family >= device.families.size()
        || !(device.families[queue.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        state.suspend();
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!muxivoIsRunning(state, now)) {
        state.suspend();
        return false;
    }
    if (!state.pipe) {
        state.pipe = std::make_unique<muxivo::VulkanFrameEndpoint>(state.window, state.extent, state.format);
        if (traceEnabled()) std::fprintf(stderr, "[Muxivo] Vulkan frame endpoint ready for HWND %p\n",
            static_cast<void*>(state.window));
    }
    const bool readerReady = state.pipe->wantsFrame();
    const bool capture = state.cadence.admit(now, readerReady);
    if (traceEnabled()) state.captureGateTrace.record(state.window, now, readerReady, capture);
    if (!capture) return false;
    if (!state.readback) {
        auto readback = std::make_unique<muxivo::Readback>(device.handle, device.dispatch, device.loaderData);
        if (!readback->prepare(state.extent, state.format, queue.family, device.memory)) {
            state.suspend();
            return false;
        }
        state.readback = std::move(readback);
    }
    return state.readback->matches(queue.family);
}

bool presented(VkResult result) {
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

VKAPI_ATTR VkResult VKAPI_CALL queuePresent(VkQueue queue, const VkPresentInfoKHR* original) {
    if (!original) return VK_ERROR_INITIALIZATION_FAILED;
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, queue);
    }
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    auto present = device->command<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    if (original->swapchainCount != 1 || !original->pSwapchains || !original->pImageIndices) {
        return present(queue, original);
    }
    const VkSwapchainKHR swapchainHandle = original->pSwapchains[0];
    const uint32_t index = original->pImageIndices[0];
    VkPresentInfoKHR outgoing = *original;
    std::shared_ptr<std::vector<uint8_t>> packet;
    {
        std::lock_guard devLock(device->mutex);
        const auto tracked = device->swapchains.find(swapchainHandle);
        const auto family = device->queues.find(queue);
        if (tracked == device->swapchains.end() || family == device->queues.end()) {
            // Release lock and call downstream present unchanged.
        } else {
            auto& state = tracked->second;
            try {
                if (index < state.images.size() && prepareCapture(*device, state, family->second)) {
                    packet = state.pipe->acquireFrameBuffer();
                    const auto copyResult = state.readback->copy(queue, state.images[index], *original, *packet);
                    if (copyResult) {
                        if (*copyResult != VK_SUCCESS) {
                            state.suspend();
                            return *copyResult;
                        }
                        // The fenced copy already consumed the game's present wait semaphores.
                        outgoing.waitSemaphoreCount = 0;
                        outgoing.pWaitSemaphores = nullptr;
                    } else {
                        packet.reset();
                    }
                }
            } catch (...) {
                state.suspend();
                packet.reset();
            }
        }
    }
    const auto result = present(queue, &outgoing);
    {
        std::lock_guard devLock(device->mutex);
        const auto tracked = device->swapchains.find(swapchainHandle);
        if (tracked != device->swapchains.end()) {
            auto& state = tracked->second;
            if (!presented(result) || (original->pResults && !presented(original->pResults[0]))
                || index >= state.images.size()) {
                state.suspend();
            } else if (state.pipe) {
                try {
                    if (windowPermitsCapture(state.window)) state.pipe->presented(packet);
                    else state.suspend();
                } catch (...) { state.suspend(); }
            }
        }
    }
    return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance handle, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice handle, const char* name);

PFN_vkVoidFunction deviceHook(std::string_view name) {
    if (name == "vkGetDeviceProcAddr") return reinterpret_cast<PFN_vkVoidFunction>(deviceProc);
    if (name == "vkDestroyDevice") return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    if (name == "vkGetDeviceQueue") return reinterpret_cast<PFN_vkVoidFunction>(getQueue);
    if (name == "vkGetDeviceQueue2") return reinterpret_cast<PFN_vkVoidFunction>(getQueue2);
    if (name == "vkCreateSwapchainKHR") return reinterpret_cast<PFN_vkVoidFunction>(createSwapchain);
    if (name == "vkDestroySwapchainKHR") return reinterpret_cast<PFN_vkVoidFunction>(destroySwapchain);
    if (name == "vkQueuePresentKHR") return reinterpret_cast<PFN_vkVoidFunction>(queuePresent);
    if (name == "vkAcquireFullScreenExclusiveModeEXT") return reinterpret_cast<PFN_vkVoidFunction>(acquireExclusive);
    if (name == "vkReleaseFullScreenExclusiveModeEXT") return reinterpret_cast<PFN_vkVoidFunction>(releaseExclusive);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice handle, const char* name) {
    if (!name) return nullptr;
    std::shared_ptr<Device> device;
    {
        std::lock_guard lock(stateMutex);
        device = findState(devices, handle);
    }
    if (!device) return nullptr;
    const auto next = device->dispatch(handle, name);
    if (next) {
        if (const auto hook = deviceHook(name)) return hook;
    }
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance handle, const char* rawName) {
    if (!rawName) return nullptr;
    const std::string_view name(rawName);
    if (name == "vkGetInstanceProcAddr") return reinterpret_cast<PFN_vkVoidFunction>(instanceProc);
    if (name == "vkCreateInstance") return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, handle);
    }
    if (!instance) return nullptr;
    const auto next = instance->dispatch(handle, rawName);
    if (!next) return nullptr;
    if (name == "vkDestroyInstance") return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
    if (name == "vkCreateDevice") return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if (name == "vkCreateWin32SurfaceKHR") return reinterpret_cast<PFN_vkVoidFunction>(createSurface);
    if (name == "vkDestroySurfaceKHR") return reinterpret_cast<PFN_vkVoidFunction>(destroySurface);
    if (const auto hook = deviceHook(name)) return hook;
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL physicalProc(VkInstance handle, const char* name) {
    if (!name) return nullptr;
    std::shared_ptr<Instance> instance;
    {
        std::lock_guard lock(stateMutex);
        instance = findState(instances, handle);
    }
    return instance && instance->physicalDispatch ? instance->physicalDispatch(handle, name) : nullptr;
}

} // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = instanceProc;
    info->pfnGetDeviceProcAddr = deviceProc;
    info->pfnGetPhysicalDeviceProcAddr = physicalProc;
    return VK_SUCCESS;
}
