// Exercise the layer's actual interception code with a deterministic Vulkan dispatch table.
#include "../vulkan/layer.cpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

const auto oldSwapchain = reinterpret_cast<VkSwapchainKHR>(201);
const auto otherSwapchain = reinterpret_cast<VkSwapchainKHR>(202);
const auto replacement = reinterpret_cast<VkSwapchainKHR>(203);
const auto surface = reinterpret_cast<VkSurfaceKHR>(301);
VkResult driverCreateResult = VK_SUCCESS;
VkResult driverPresentResult = VK_SUCCESS;
VkResult driverSwapchainResult = VK_SUCCESS;
unsigned presents = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL driverCreate(VkDevice, const VkSwapchainCreateInfoKHR*,
    const VkAllocationCallbacks*, VkSwapchainKHR* output) {
    if (driverCreateResult == VK_SUCCESS) *output = replacement;
    return driverCreateResult;
}

VKAPI_ATTR VkResult VKAPI_CALL driverImages(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* images) {
    *count = 1;
    if (images) images[0] = reinterpret_cast<VkImage>(501);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL driverPresent(VkQueue, const VkPresentInfoKHR* info) {
    require(info->swapchainCount == 1 && info->pSwapchains[0] == oldSwapchain, "Retired present reaches Vulkan unchanged");
    ++presents;
    if (info->pResults) info->pResults[0] = driverSwapchainResult;
    return driverPresentResult;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL driverDispatch(VkDevice, const char* name) {
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(driverCreate);
    if (std::strcmp(name, "vkGetSwapchainImagesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(driverImages);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(driverPresent);
    require(false, "The layer called an unexpected Vulkan command");
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL setLoaderData(VkDevice, void*) { return VK_SUCCESS; }

bool oldEndpointAvailable(HWND window) {
    wchar_t path[128] = {};
    swprintf_s(path, L"\\\\.\\pipe\\Muxivo.GameCapture.Vulkan.v1.%lu.%llx", GetCurrentProcessId(),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window)));
    return WaitNamedPipeW(path, 1) != FALSE;
}

struct DeviceFixture {
    void* dispatchKeyValue = this;
    const VkDevice handle = reinterpret_cast<VkDevice>(&dispatchKeyValue);
    const VkQueue queue = reinterpret_cast<VkQueue>(&dispatchKeyValue);
    const std::shared_ptr<Device> state = std::make_shared<Device>();
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
        WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    DeviceFixture() {
        require(window && windowPermitsCapture(window), "The fixture owns a visible unprotected HWND");
        state->handle = handle;
        state->dispatch = driverDispatch;
        state->loaderData = setLoaderData;
        state->instance = std::make_shared<Instance>();
        state->instance->windows.emplace(surface, window);
        auto& old = state->swapchains[oldSwapchain];
        old.window = window;
        old.extent = { 640, 360 };
        old.images.push_back(reinterpret_cast<VkImage>(501));
        old.muxivoRunning = true;
        old.nextPresenceCheck = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        old.pipe = std::make_unique<muxivo::VulkanFrameEndpoint>(window, old.extent, old.format);
        old.pipe->wantsFrame();
        require(oldEndpointAvailable(window), "Old capture endpoint is initially advertised");
        state->swapchains.try_emplace(otherSwapchain);
        state->families.resize(1);
        state->families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
        state->queues.emplace(queue, QueueInfo{ 0, true });
        devices.emplace(dispatchKeyValue, state);
    }

    ~DeviceFixture() {
        devices.erase(dispatchKeyValue);
        state->swapchains.clear();
        DestroyWindow(window);
    }
};

VkSwapchainCreateInfoKHR createInfo() {
    VkSwapchainCreateInfoKHR create = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    create.surface = surface;
    create.oldSwapchain = oldSwapchain;
    create.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    create.imageColorSpace = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    create.imageExtent = { 960, 540 };
    return create;
}

void vulkan_replacement_retires_old_capture_and_preserves_other_swapchains() {
    DeviceFixture fixture;
    driverCreateResult = VK_SUCCESS;
    auto create = createInfo();
    VkSwapchainKHR output = VK_NULL_HANDLE;
    require(createSwapchain(fixture.handle, &create, nullptr, &output) == VK_SUCCESS, "Replacement returned to game");
    require(output == replacement, "Game receives the driver's replacement handle");
    require(!fixture.state->swapchains.count(oldSwapchain), "Retired swapchain cannot publish another frame");
    require(!oldEndpointAvailable(fixture.window), "Retired swapchain releases its named pipe immediately");
    require(fixture.state->swapchains.count(otherSwapchain) == 1, "Unrelated swapchain keeps its capture");
    const auto& next = fixture.state->swapchains.at(replacement);
    require(next.window == fixture.window && next.extent.width == 960 && next.extent.height == 540,
        "Only the replacement geometry is retained for the same HWND");
    require(next.format == muxivo::FrameFormat::ScRgb && !next.pipe && !next.readback,
        "Replacement uses its own color format and starts without the previous capture resources");

    const uint32_t imageIndex = 0;
    VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.swapchainCount = 1;
    present.pSwapchains = &oldSwapchain;
    present.pImageIndices = &imageIndex;
    require(queuePresent(fixture.queue, &present) == VK_SUCCESS && presents == 1,
        "Previously acquired images may still be presented by the game");
}

void vulkan_suboptimal_presentation_keeps_the_endpoint_and_driver_errors_retire_it() {
    for (const auto overall : { VK_SUCCESS, VK_SUBOPTIMAL_KHR }) {
        DeviceFixture fixture;
        const uint32_t imageIndex = 0;
        VkResult perSwapchain = VK_SUCCESS;
        VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.swapchainCount = 1;
        present.pSwapchains = &oldSwapchain;
        present.pImageIndices = &imageIndex;
        present.pResults = &perSwapchain;
        driverPresentResult = overall;
        driverSwapchainResult = VK_SUBOPTIMAL_KHR;
        require(queuePresent(fixture.queue, &present) == overall && perSwapchain == VK_SUBOPTIMAL_KHR,
            "Suboptimal presentation reports the driver's success codes to the game");
        require(oldEndpointAvailable(fixture.window), "Suboptimal presentation keeps the selected Vulkan endpoint");
        driverPresentResult = VK_ERROR_OUT_OF_DATE_KHR;
        driverSwapchainResult = VK_ERROR_OUT_OF_DATE_KHR;
        require(queuePresent(fixture.queue, &present) == VK_ERROR_OUT_OF_DATE_KHR,
            "The game still receives a rejected presentation");
        require(!oldEndpointAvailable(fixture.window), "Rejected presentation withdraws the capture endpoint");
    }
}

void vulkan_protected_queue_retires_fresh_presentation_and_pipe() {
    DeviceFixture fixture;
    driverPresentResult = VK_SUCCESS;
    driverSwapchainResult = VK_SUCCESS;
    const uint32_t imageIndex = 0;
    VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.swapchainCount = 1;
    present.pSwapchains = &oldSwapchain;
    present.pImageIndices = &imageIndex;

    wchar_t name[128] = {};
    swprintf_s(name, L"Local\\Muxivo.GameCapture.Vulkan.Present.v2.%lu.%llx", GetCurrentProcessId(),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fixture.window)));
    const muxivo::UniqueHandle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
    require(mapping != nullptr, "The endpoint owns a presentation record");
    const auto* timestamp = static_cast<const volatile LONG64*>(MapViewOfFile(mapping.get(), FILE_MAP_READ,
        0, 0, sizeof(LONG64)));
    require(timestamp && *timestamp == 0, "An unpresented window is not yet eligible");
    require(queuePresent(fixture.queue, &present) == VK_SUCCESS && (*timestamp >> 8) > 0 && (*timestamp & 0xff) == 1,
        "A valid presentation establishes freshness without requesting a capture frame");

    fixture.state->queues.at(fixture.queue).unprotected = false;
    require(queuePresent(fixture.queue, &present) == VK_SUCCESS, "Protection leaves the game's result intact");
    require(*timestamp == 0 && !oldEndpointAvailable(fixture.window),
        "Protection retires both the live freshness record and the frame endpoint");
    UnmapViewOfFile(const_cast<const LONG64*>(timestamp));
}

void vulkan_rejected_replacement_cannot_keep_retired_capture_alive() {
    for (const VkResult driverResult : { VK_ERROR_OUT_OF_DATE_KHR, VK_SUCCESS }) {
        DeviceFixture fixture;
        driverCreateResult = driverResult;
        auto create = createInfo();
        if (driverResult == VK_SUCCESS) create.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
        VkSwapchainKHR output = VK_NULL_HANDLE;
        require(createSwapchain(fixture.handle, &create, nullptr, &output) == driverResult, "Game keeps its driver result");
        require(!fixture.state->swapchains.count(oldSwapchain), "Driver failure or protection must retire the old capture");
        require(!oldEndpointAvailable(fixture.window), "Rejected replacement withdraws the old pipe");
        require(!fixture.state->swapchains.count(replacement), "No failed or protected replacement is tracked");
        require(fixture.state->swapchains.count(otherSwapchain) == 1, "Unrelated capture is retained");
    }
}

void vulkan_capture_cadence_keeps_sixty_hz_without_drifting_on_present_jitter() {
    using namespace std::chrono;
    muxivo::CaptureCadence cadence;
    const muxivo::CaptureCadence::Clock::time_point start{};
    require(cadence.admit(start, true), "Initial Present is admitted immediately");
    for (int frame = 1; frame < 600; ++frame) {
        const auto elapsed = nanoseconds(1'000'000'000LL * frame / 60);
        const auto jitter = microseconds(frame % 2 ? -800 : 800);
        require(cadence.admit(start + elapsed + jitter, true), "A slightly early or late 60 Hz frame must not be skipped");
    }
}

void vulkan_capture_cadence_limits_fast_producers_and_drops_missed_slots() {
    using namespace std::chrono;
    muxivo::CaptureCadence cadence;
    const muxivo::CaptureCadence::Clock::time_point start{};
    int admitted = 0;
    for (int frame = 0; frame < 1200; ++frame) {
        const auto elapsed = nanoseconds(1'000'000'000LL * frame / 120);
        if (cadence.admit(start + elapsed, true)) ++admitted;
    }
    require(admitted == 600, "A continuously ready 120 Hz producer supplies 60 frames per second");

    const auto resumed = start + seconds(15);
    require(!cadence.admit(resumed, false), "No ready reader means no GPU copy");
    require(cadence.admit(resumed + milliseconds(2), true), "Backpressure does not consume the next slot");
    require(!cadence.admit(resumed + milliseconds(3), true), "A stall does not accumulate overdue captures");
    require(cadence.admit(resumed + milliseconds(19), true), "Normal pacing resumes after the stall");
}

void vulkan_readback_prefers_cached_coherent_memory_with_compatible_fallback() {
    VkPhysicalDeviceMemoryProperties memory = {};
    memory.memoryTypeCount = 4;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    memory.memoryTypes[3].propertyFlags = memory.memoryTypes[1].propertyFlags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    require(muxivo::readbackMemoryType(0b1111, memory) == 3u, "Readback prefers cached coherent memory");
    require(muxivo::readbackMemoryType(0b0111, memory) == 1u, "Use a compatible coherent type when cached coherent memory is unavailable");
    require(!muxivo::readbackMemoryType(0b0101, memory), "Use only advertised, host-visible coherent memory");
}

void vulkan_pipe_capacity_covers_bounded_frame_packets() {
    using muxivo::FrameFormat;
    using muxivo::vulkanFramePipeCapacity;
    constexpr VkExtent2D fullHd{1920, 1080};
    constexpr VkExtent2D ultraHd{3840, 2160};
    require(vulkanFramePipeCapacity(fullHd, FrameFormat::Bgra8) == 12 * 1024 * 1024,
        "Small packets retain the baseline pipe capacity");
    require(vulkanFramePipeCapacity(ultraHd, FrameFormat::Bgra8) == muxivo::frameHeaderSize + 3840 * 2160 * 4,
        "The 4K SDR quota includes the entire packet and its header");
    require(vulkanFramePipeCapacity(ultraHd, FrameFormat::ScRgb) == 32 * 1024 * 1024,
        "Oversized packets use the bounded quota and continue with overlapped writes");
    require(vulkanFramePipeCapacity({UINT32_MAX, UINT32_MAX}, FrameFormat::ScRgb) == 32 * 1024 * 1024,
        "Outsize frame arithmetic stays bounded without overflow");
}

std::vector<uint8_t> readOverlappedTestPacket(HANDLE pipe, std::size_t bytes) {
    std::vector<uint8_t> data(bytes);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::size_t offset = 0;
    while (offset < bytes) {
        DWORD available = 0;
        require(PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr), "The reader stays connected");
        if (available == 0) {
            require(std::chrono::steady_clock::now() < deadline, "The full packet arrives without any further presents");
            Sleep(1);
            continue;
        }
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes - offset, available));
        DWORD received = 0;
        require(ReadFile(pipe, data.data() + offset, chunk, &received, nullptr) && received > 0,
            "The reader receives the announced bytes");
        offset += received;
    }
    return data;
}

void vulkan_overlapped_pipe_delivers_large_packets_independently_and_retires_stalled_readers() {
    const auto window = reinterpret_cast<HWND>(0xa4);
    constexpr auto endpoint = L"Muxivo.GameCapture.Test.Overlapped.v1";
    wchar_t path[128] = {};
    swprintf_s(path, L"\\\\.\\pipe\\%ls.%lu.a4", endpoint, GetCurrentProcessId());
    const auto connect = [&] {
        muxivo::UniqueHandle reader(CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        require(reader.get() != INVALID_HANDLE_VALUE, "The test reader opens the local pipe");
        return reader;
    };
    auto writer = std::make_unique<muxivo::OverlappedFramePipeWriter>(window, endpoint, 4096);
    require(!writer->wantsFrame(), "A pipe awaiting a client does not claim readiness");
    auto fast = connect();
    auto stalled = connect();
    require(writer->wantsFrame(), "The writer accepts connected readers");

    std::vector<uint8_t> first(8 * 1024 * 1024, 0x61);
    first.back() = 0x73;
    auto buffer = writer->acquireFrameBuffer();
    auto firstBuffer = std::weak_ptr(buffer);
    *buffer = first;
    writer->publish(buffer);
    buffer.reset();
    require(readOverlappedTestPacket(fast.get(), first.size()) == first, "The complete large packet reaches the fast reader");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!writer->wantsFrame()) {
        require(std::chrono::steady_clock::now() < deadline, "A stalled reader does not delay the fast reader");
        Sleep(1);
    }
    const std::vector<uint8_t> second(4097, 0x90);
    buffer = writer->acquireFrameBuffer();
    {
        const auto pending = firstBuffer.lock();
        require(pending && pending != buffer && *pending == first, "The stalled reader owns an immutable first packet");
    }
    *buffer = second;
    auto reusable = std::weak_ptr(buffer);
    writer->publish(buffer);
    buffer.reset();
    require(readOverlappedTestPacket(fast.get(), second.size()) == second, "The next packet retains its byte boundaries");
    while (!writer->wantsFrame()) {
        require(std::chrono::steady_clock::now() < deadline, "The next write completes once the fast reader drains it");
        Sleep(1);
    }
    buffer = writer->acquireFrameBuffer();
    require(buffer == reusable.lock(), "A completed packet's storage is reused");

    writer.reset();
    require(firstBuffer.expired(), "Retirement releases the stalled reader's packet");
    fast.reset();
    stalled.reset();
    writer = std::make_unique<muxivo::OverlappedFramePipeWriter>(window, endpoint, 4096);
    fast = connect();
    require(writer->wantsFrame(), "A new session can reopen the retired endpoint");
    buffer = writer->acquireFrameBuffer();
    *buffer = second;
    writer->publish(buffer);
    buffer.reset();
    require(readOverlappedTestPacket(fast.get(), second.size()) == second, "A resumed endpoint exposes only the new packet");
}

} // namespace

int main() {
    vulkan_replacement_retires_old_capture_and_preserves_other_swapchains();
    vulkan_rejected_replacement_cannot_keep_retired_capture_alive();
    vulkan_suboptimal_presentation_keeps_the_endpoint_and_driver_errors_retire_it();
    vulkan_protected_queue_retires_fresh_presentation_and_pipe();
    vulkan_capture_cadence_keeps_sixty_hz_without_drifting_on_present_jitter();
    vulkan_capture_cadence_limits_fast_producers_and_drops_missed_slots();
    vulkan_readback_prefers_cached_coherent_memory_with_compatible_fallback();
    vulkan_pipe_capacity_covers_bounded_frame_packets();
    vulkan_overlapped_pipe_delivers_large_packets_independently_and_retires_stalled_readers();
}
