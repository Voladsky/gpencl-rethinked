#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void throwVk(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
    {
        std::ostringstream oss;
        oss << what << " (VkResult=" << result << ")";
        throw std::runtime_error(oss.str());
    }
}

static std::string readFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open file: " + filename);

    const std::streamsize size = file.tellg();
    file.seekg(std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (!file.read(&buffer[0], size))
        throw std::runtime_error("Failed to read file: " + filename);

    return buffer;
}

// Compile GLSL file to SPIR-V using shaderc
static std::vector<uint32_t> compileShaderFile(const std::string& filename)
{
    const std::string source = readFile(filename);

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    // Set the source language to GLSL and target Vulkan environment
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(source, shaderc_compute_shader, filename.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        std::cerr << "Shader compilation failed (" << filename << "): "
                  << result.GetErrorMessage() << std::endl;
        throw std::runtime_error("Shader compilation failed");
    }

    return {result.cbegin(), result.cend()};
}

static std::vector<uint8_t> parseHexBytesStrict(const std::string& hex)
{
    if (hex.size() % 2 != 0)
        throw std::runtime_error("Hex string must have even length");

    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2)
    {
        unsigned int byte = 0;
        if (std::sscanf(hex.substr(i, 2).c_str(), "%02x", &byte) != 1)
            throw std::runtime_error("Invalid hex input");
        out.push_back(static_cast<uint8_t>(byte));
    }

    return out;
}

static std::vector<uint8_t> parseTag16(const std::string& hex)
{
    auto bytes = parseHexBytesStrict(hex);
    if (bytes.size() != 16)
        throw std::runtime_error("Poly1305 tag must be exactly 16 bytes");
    return bytes;
}

static std::vector<uint32_t> packBytesToWords(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
        return std::vector<uint32_t>(1, 0u);

    const size_t wordCount = (bytes.size() + 3) / 4;
    std::vector<uint32_t> words(wordCount, 0);

    for (size_t i = 0; i < bytes.size(); ++i)
    {
        const size_t w = i / 4;
        const size_t shift = (i % 4) * 8;
        words[w] |= (uint32_t(bytes[i]) << shift);
    }

    return words;
}

static void unpackWordsToBytes(const std::vector<uint32_t>& words, std::vector<uint8_t>& bytesOut, size_t byteCount)
{
    bytesOut.resize(byteCount);
    for (size_t i = 0; i < byteCount; ++i)
    {
        const size_t w = i / 4;
        const size_t shift = (i % 4) * 8;
        bytesOut[i] = uint8_t((words[w] >> shift) & 0xFFu);
    }
}

static std::string bytesToHex(const std::vector<uint8_t>& bytes)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes)
        oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

struct alignas(16) GpuJob
{
    uint32_t inputWordOffset;
    uint32_t outputWordOffset;
    uint32_t byteSize;
    uint32_t wordCount;
    uint32_t keyIndex;
    uint32_t nonce0;
    uint32_t nonce1;
    uint32_t nonce2;
    uint32_t counter0;
    uint32_t aadWordOffset;
    uint32_t aadByteSize;
};

struct alignas(16) GpuKey256
{
    uint32_t k[8];
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cout << "Usage:\n";
        std::cout << "  " << argv[0] << " enc <plaintext_hex> [aad_hex]\n";
        std::cout << "  " << argv[0] << " dec <ciphertext_hex> <tag_hex> [aad_hex]\n";
        return 1;
    }

    const std::string mode = argv[1];
    const bool encryptMode = (mode == "enc" || mode == "encrypt");
    const bool decryptMode = (mode == "dec" || mode == "decrypt");
    if (!encryptMode && !decryptMode)
    {
        std::cerr << "First argument must be enc or dec.\n";
        return 1;
    }

    std::string dataHex;
    std::string tagHex;
    std::string aadHex;
    if (encryptMode)
    {
        dataHex = argv[2];
        if (argc >= 4)
            aadHex = argv[3];
    }
    else
    {
        if (argc < 4)
        {
            std::cerr << "Decrypt mode requires ciphertext_hex and tag_hex.\n";
            return 1;
        }
        dataHex = argv[2];
        tagHex = argv[3];
        if (argc >= 5)
            aadHex = argv[4];
    }

    const std::vector<uint8_t> inputData = parseHexBytesStrict(dataHex);
    const std::vector<uint8_t> aadData = aadHex.empty() ? std::vector<uint8_t>{} : parseHexBytesStrict(aadHex);
    if (inputData.empty())
    {
        std::cerr << "No input data provided.\n";
        return 1;
    }

    std::vector<uint8_t> expectedTag;
    if (decryptMode)
        expectedTag = parseTag16(tagHex);
    const std::vector<uint32_t> inputWords = packBytesToWords(inputData);
    const std::vector<uint32_t> aadWords = packBytesToWords(aadData);

    const VkDeviceSize dataWordBytes = VkDeviceSize(inputWords.size() * sizeof(uint32_t));
    const VkDeviceSize aadWordBytes  = VkDeviceSize(aadWords.size() * sizeof(uint32_t));
    const VkDeviceSize keyBytes      = VkDeviceSize(sizeof(GpuKey256));
    const VkDeviceSize jobBytes      = VkDeviceSize(sizeof(GpuJob));
    const VkDeviceSize tagBytes      = VkDeviceSize(16);

    GpuKey256 key{};
    key.k[0] = 0x03020101;
    key.k[1] = 0x07060504;
    key.k[2] = 0x0b0a0908;
    key.k[3] = 0x0f0e0d0c;
    key.k[4] = 0x13121110;
    key.k[5] = 0x17161514;
    key.k[6] = 0x1b1a1918;
    key.k[7] = 0x1f1e1d1c;

    GpuJob job{};
    job.inputWordOffset = 0;
    job.outputWordOffset = 0;
    job.byteSize = static_cast<uint32_t>(inputData.size());
    job.wordCount = static_cast<uint32_t>(inputWords.size());
    job.keyIndex = 0;
    job.nonce0 = 0x09000000;
    job.nonce1 = 0x4a000000;
    job.nonce2 = 0x00000000;
    job.counter0 = 1u;
    job.aadWordOffset = 0;
    job.aadByteSize = static_cast<uint32_t>(aadData.size());

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t computeQueueFamily = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;

    VkCommandPool oneTimeCommandPool = VK_NULL_HANDLE;

    VkBuffer inputBuffer = VK_NULL_HANDLE;
    VkBuffer outputBuffer = VK_NULL_HANDLE;
    VkBuffer authBuffer = VK_NULL_HANDLE;
    VkBuffer keyBuffer = VK_NULL_HANDLE;
    VkBuffer jobBuffer = VK_NULL_HANDLE;
    VkBuffer aadBuffer = VK_NULL_HANDLE;
    VkBuffer tagBuffer = VK_NULL_HANDLE;

    VkBuffer stagingInputBuffer = VK_NULL_HANDLE;
    VkBuffer stagingOutputBuffer = VK_NULL_HANDLE;
    VkBuffer stagingKeyBuffer = VK_NULL_HANDLE;
    VkBuffer stagingJobBuffer = VK_NULL_HANDLE;
    VkBuffer stagingAadBuffer = VK_NULL_HANDLE;
    VkBuffer stagingTagBuffer = VK_NULL_HANDLE;

    VkDeviceMemory inputMemory = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkDeviceMemory authMemory = VK_NULL_HANDLE;
    VkDeviceMemory keyMemory = VK_NULL_HANDLE;
    VkDeviceMemory jobMemory = VK_NULL_HANDLE;
    VkDeviceMemory aadMemory = VK_NULL_HANDLE;
    VkDeviceMemory tagMemory = VK_NULL_HANDLE;

    VkDeviceMemory stagingInputMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingOutputMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingKeyMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingJobMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingAadMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingTagMemory = VK_NULL_HANDLE;

    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule chachaShaderModule = VK_NULL_HANDLE;
    VkShaderModule polyShaderModule = VK_NULL_HANDLE;
    VkPipeline chachaPipeline = VK_NULL_HANDLE;
    VkPipeline polyPipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet cryptoDescSet = VK_NULL_HANDLE;
    VkDescriptorSet authDescSet = VK_NULL_HANDLE;

    auto destroyBuffer = [&](VkBuffer& buf, VkDeviceMemory& mem)
    {
        if (buf != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, buf, nullptr);
            buf = VK_NULL_HANDLE;
        }
        if (mem != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, mem, nullptr);
            mem = VK_NULL_HANDLE;
        }
    };

    auto cleanupDeviceResources = [&]()
    {
        if (device == VK_NULL_HANDLE)
            return;

        if (chachaShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, chachaShaderModule, nullptr);
        if (polyShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, polyShaderModule, nullptr);
        if (chachaPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, chachaPipeline, nullptr);
        if (polyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, polyPipeline, nullptr);
        if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descPool, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
        if (oneTimeCommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, oneTimeCommandPool, nullptr);

        destroyBuffer(inputBuffer, inputMemory);
        destroyBuffer(outputBuffer, outputMemory);
        destroyBuffer(authBuffer, authMemory);
        destroyBuffer(keyBuffer, keyMemory);
        destroyBuffer(jobBuffer, jobMemory);
        destroyBuffer(aadBuffer, aadMemory);
        destroyBuffer(tagBuffer, tagMemory);

        destroyBuffer(stagingInputBuffer, stagingInputMemory);
        destroyBuffer(stagingOutputBuffer, stagingOutputMemory);
        destroyBuffer(stagingKeyBuffer, stagingKeyMemory);
        destroyBuffer(stagingJobBuffer, stagingJobMemory);
        destroyBuffer(stagingAadBuffer, stagingAadMemory);
        destroyBuffer(stagingTagBuffer, stagingTagMemory);
    };

    auto shutdownAndDestroy = [&]()
    {
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);
        cleanupDeviceResources();
        if (device != VK_NULL_HANDLE)
        {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    };

    try
    {
        // 1. Create Vulkan instance
        {
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "ChaCha20-Poly1305 Compute";
            appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.pEngineName = "NoEngine";
            appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.apiVersion = VK_API_VERSION_1_0;

            VkInstanceCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            createInfo.pApplicationInfo = &appInfo;

            throwVk(vkCreateInstance(&createInfo, nullptr, &instance), "Failed to create Vulkan instance");
        }

        // 2. Pick a physical device with a compute queue and shaderInt64 support
        {
            uint32_t devCount = 0;
            throwVk(vkEnumeratePhysicalDevices(instance, &devCount, nullptr), "vkEnumeratePhysicalDevices failed");
            if (devCount == 0)
                throw std::runtime_error("No Vulkan devices found");

            std::vector<VkPhysicalDevice> devices(devCount);
            throwVk(vkEnumeratePhysicalDevices(instance, &devCount, devices.data()), "vkEnumeratePhysicalDevices failed");

            for (auto dev : devices)
            {
                VkPhysicalDeviceFeatures supported{};
                vkGetPhysicalDeviceFeatures(dev, &supported);
                if (!supported.shaderInt64)
                    continue;

                uint32_t qCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
                std::vector<VkQueueFamilyProperties> qprops(qCount);
                vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());

                for (uint32_t i = 0; i < qCount; ++i)
                {
                    if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                    {
                        physicalDevice = dev;
                        computeQueueFamily = i;
                        break;
                    }
                }

                if (physicalDevice != VK_NULL_HANDLE)
                    break;
            }

            if (physicalDevice == VK_NULL_HANDLE)
                throw std::runtime_error("No device with compute queue and shaderInt64 support found");
        }

        // 3. Create logical device and get compute queue
        {
            float queuePriority = 1.0f;

            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = computeQueueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;

            VkPhysicalDeviceFeatures deviceFeatures{};
            deviceFeatures.shaderInt64 = VK_TRUE;

            VkDeviceCreateInfo devCreateInfo{};
            devCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            devCreateInfo.queueCreateInfoCount = 1;
            devCreateInfo.pQueueCreateInfos = &queueCreateInfo;
            devCreateInfo.pEnabledFeatures = &deviceFeatures;

            throwVk(vkCreateDevice(physicalDevice, &devCreateInfo, nullptr, &device), "Failed to create logical device");
            vkGetDeviceQueue(device, computeQueueFamily, 0, &computeQueue);
        }

        // A persistent one-time command pool for staging copies and submissions.
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = computeQueueFamily;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            throwVk(vkCreateCommandPool(device, &poolInfo, nullptr, &oneTimeCommandPool), "vkCreateCommandPool failed");
        }

        auto findMemoryType = [&](uint32_t typeBits, VkMemoryPropertyFlags props) -> uint32_t
        {
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            {
                if ((typeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & props) == props)
                {
                    return i;
                }
            }
            throw std::runtime_error("Failed to find suitable memory type");
        };

        auto createBuffer = [&](VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags props,
                                VkBuffer& buf,
                                VkDeviceMemory& mem)
        {
            VkBufferCreateInfo bufInfo{};
            bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufInfo.size = size;
            bufInfo.usage = usage;
            bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            throwVk(vkCreateBuffer(device, &bufInfo, nullptr, &buf), "vkCreateBuffer failed");

            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, buf, &req);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = req.size;
            allocInfo.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);

            throwVk(vkAllocateMemory(device, &allocInfo, nullptr, &mem), "vkAllocateMemory failed");
            throwVk(vkBindBufferMemory(device, buf, mem, 0), "vkBindBufferMemory failed");
        };

        auto submitOneTimeCommands = [&](auto&& recordFn)
        {
            VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;

            auto cleanupLocal = [&]()
            {
                if (fence != VK_NULL_HANDLE)
                {
                    vkDestroyFence(device, fence, nullptr);
                    fence = VK_NULL_HANDLE;
                }
                if (cmdBuf != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(device, oneTimeCommandPool, 1, &cmdBuf);
                    cmdBuf = VK_NULL_HANDLE;
                }
            };

            try
            {
                VkCommandBufferAllocateInfo cbAlloc{};
                cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cbAlloc.commandPool = oneTimeCommandPool;
                cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbAlloc.commandBufferCount = 1;
                throwVk(vkAllocateCommandBuffers(device, &cbAlloc, &cmdBuf), "vkAllocateCommandBuffers failed");

                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                throwVk(vkBeginCommandBuffer(cmdBuf, &beginInfo), "vkBeginCommandBuffer failed");

                recordFn(cmdBuf);

                throwVk(vkEndCommandBuffer(cmdBuf), "vkEndCommandBuffer failed");

                VkFenceCreateInfo fenceInfo{};
                fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                throwVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence failed");

                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmdBuf;

                throwVk(vkQueueSubmit(computeQueue, 1, &submitInfo, fence), "vkQueueSubmit failed");
                throwVk(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences failed");

                cleanupLocal();
            }
            catch (...)
            {
                cleanupLocal();
                throw;
            }
        };

        auto copyBuffer = [&](VkBuffer src, VkBuffer dst, VkDeviceSize size)
        {
            submitOneTimeCommands([&](VkCommandBuffer cmdBuf)
            {
                VkBufferCopy region{};
                region.size = size;
                vkCmdCopyBuffer(cmdBuf, src, dst, 1, &region);
            });
        };

        auto uploadMappedBuffer = [&](VkDeviceMemory memory, VkDeviceSize size, const void* src)
        {
            void* mapped = nullptr;
            throwVk(vkMapMemory(device, memory, 0, size, 0, &mapped), "vkMapMemory failed");
            std::memcpy(mapped, src, static_cast<size_t>(size));
            vkUnmapMemory(device, memory);
        };

        // 4. Create buffers
        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     inputBuffer, inputMemory);

        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     outputBuffer, outputMemory);

        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     authBuffer, authMemory);

        createBuffer(keyBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     keyBuffer, keyMemory);

        createBuffer(jobBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     jobBuffer, jobMemory);

        createBuffer(aadWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     aadBuffer, aadMemory);

        createBuffer(tagBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     tagBuffer, tagMemory);

        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingInputBuffer, stagingInputMemory);

        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingOutputBuffer, stagingOutputMemory);

        createBuffer(keyBytes,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingKeyBuffer, stagingKeyMemory);

        createBuffer(jobBytes,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingJobBuffer, stagingJobMemory);

        createBuffer(aadWordBytes,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingAadBuffer, stagingAadMemory);

        createBuffer(tagBytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingTagBuffer, stagingTagMemory);

        // Upload data
        uploadMappedBuffer(stagingInputMemory, dataWordBytes, inputWords.data());
        copyBuffer(stagingInputBuffer, inputBuffer, dataWordBytes);

        uploadMappedBuffer(stagingAadMemory, aadWordBytes, aadWords.data());
        copyBuffer(stagingAadBuffer, aadBuffer, aadWordBytes);

        uploadMappedBuffer(stagingKeyMemory, keyBytes, &key);
        copyBuffer(stagingKeyBuffer, keyBuffer, keyBytes);

        uploadMappedBuffer(stagingJobMemory, jobBytes, &job);
        copyBuffer(stagingJobBuffer, jobBuffer, jobBytes);

        // 5. Descriptor set layout
        {
            VkDescriptorSetLayoutBinding bindings[6]{};
            for (int i = 0; i < 6; ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }

            VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
            descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descLayoutInfo.bindingCount = 6;
            descLayoutInfo.pBindings = bindings;
            throwVk(vkCreateDescriptorSetLayout(device, &descLayoutInfo, nullptr, &descSetLayout),
                    "vkCreateDescriptorSetLayout failed");

            VkPipelineLayoutCreateInfo pipeLayoutInfo{};
            pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeLayoutInfo.setLayoutCount = 1;
            pipeLayoutInfo.pSetLayouts = &descSetLayout;
            throwVk(vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &pipelineLayout),
                    "vkCreatePipelineLayout failed");
        }

        // 6. Compile shader and create pipelines
        {
            const std::vector<uint32_t> chachaSpv = compileShaderFile("chacha20.comp");
            const std::vector<uint32_t> polySpv   = compileShaderFile("poly1305.comp");

            VkShaderModuleCreateInfo shaderInfo{};
            shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderInfo.codeSize = chachaSpv.size() * sizeof(uint32_t);
            shaderInfo.pCode = chachaSpv.data();
            throwVk(vkCreateShaderModule(device, &shaderInfo, nullptr, &chachaShaderModule),
                    "vkCreateShaderModule(chacha) failed");

            shaderInfo.codeSize = polySpv.size() * sizeof(uint32_t);
            shaderInfo.pCode = polySpv.data();
            throwVk(vkCreateShaderModule(device, &shaderInfo, nullptr, &polyShaderModule),
                    "vkCreateShaderModule(poly1305) failed");

            VkPipelineShaderStageCreateInfo stageInfo{};
            stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stageInfo.pName = "main";

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = pipelineLayout;

            stageInfo.module = chachaShaderModule;
            pipelineInfo.stage = stageInfo;
            throwVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &chachaPipeline),
                    "vkCreateComputePipelines(chacha) failed");

            stageInfo.module = polyShaderModule;
            pipelineInfo.stage = stageInfo;
            throwVk(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &polyPipeline),
                    "vkCreateComputePipelines(poly1305) failed");
        }

        // 7. Descriptor pool and 2 descriptor sets
        {
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            poolSize.descriptorCount = 12;

            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 2;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            throwVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool),
                    "vkCreateDescriptorPool failed");

            VkDescriptorSetLayout layouts[2] = {descSetLayout, descSetLayout};
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descPool;
            allocInfo.descriptorSetCount = 2;
            allocInfo.pSetLayouts = layouts;

            VkDescriptorSet sets[2]{};
            throwVk(vkAllocateDescriptorSets(device, &allocInfo, sets),
                    "vkAllocateDescriptorSets failed");

            cryptoDescSet = sets[0];
            authDescSet   = sets[1];

            VkDescriptorBufferInfo cryptoBufInfos[6]{};
            cryptoBufInfos[0].buffer = inputBuffer;
            cryptoBufInfos[0].offset = 0;
            cryptoBufInfos[0].range  = dataWordBytes;

            cryptoBufInfos[1].buffer = outputBuffer;
            cryptoBufInfos[1].offset = 0;
            cryptoBufInfos[1].range  = dataWordBytes;

            cryptoBufInfos[2].buffer = keyBuffer;
            cryptoBufInfos[2].offset = 0;
            cryptoBufInfos[2].range  = keyBytes;

            cryptoBufInfos[3].buffer = jobBuffer;
            cryptoBufInfos[3].offset = 0;
            cryptoBufInfos[3].range  = jobBytes;

            cryptoBufInfos[4].buffer = aadBuffer;
            cryptoBufInfos[4].offset = 0;
            cryptoBufInfos[4].range  = aadWordBytes;

            cryptoBufInfos[5].buffer = tagBuffer;
            cryptoBufInfos[5].offset = 0;
            cryptoBufInfos[5].range  = tagBytes;

            VkDescriptorBufferInfo authBufInfos[6]{};
            authBufInfos[0].buffer = inputBuffer;
            authBufInfos[0].offset = 0;
            authBufInfos[0].range  = dataWordBytes;

            authBufInfos[1].buffer = authBuffer;
            authBufInfos[1].offset = 0;
            authBufInfos[1].range  = dataWordBytes;

            authBufInfos[2].buffer = keyBuffer;
            authBufInfos[2].offset = 0;
            authBufInfos[2].range  = keyBytes;

            authBufInfos[3].buffer = jobBuffer;
            authBufInfos[3].offset = 0;
            authBufInfos[3].range  = jobBytes;

            authBufInfos[4].buffer = aadBuffer;
            authBufInfos[4].offset = 0;
            authBufInfos[4].range  = aadWordBytes;

            authBufInfos[5].buffer = tagBuffer;
            authBufInfos[5].offset = 0;
            authBufInfos[5].range  = tagBytes;

            VkWriteDescriptorSet writes[12]{};
            for (int i = 0; i < 6; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = cryptoDescSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &cryptoBufInfos[i];
            }
            for (int i = 0; i < 6; ++i)
            {
                writes[6 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[6 + i].dstSet = authDescSet;
                writes[6 + i].dstBinding = i;
                writes[6 + i].descriptorCount = 1;
                writes[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[6 + i].pBufferInfo = &authBufInfos[i];
            }
            vkUpdateDescriptorSets(device, 12, writes, 0, nullptr);
        }

        const uint32_t blockCount = static_cast<uint32_t>((inputData.size() + 63u) / 64u);
        const uint32_t groupCount = (blockCount + 63u) / 64u;

        if (encryptMode)
        {
            submitOneTimeCommands([&](VkCommandBuffer cmdBuf)
            {
                VkBufferCopy copyRegion{};
                copyRegion.size = dataWordBytes;

                VkBufferCopy tagCopy{};
                tagCopy.size = tagBytes;

                // ChaCha20: plaintext -> ciphertext
                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, chachaPipeline);
                vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipelineLayout, 0, 1, &cryptoDescSet, 0, nullptr);
                vkCmdDispatch(cmdBuf, groupCount, 1, 1);

                VkMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);
                vkCmdCopyBuffer(cmdBuf, outputBuffer, authBuffer, 1, &copyRegion);

                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);

                // Poly1305 over AAD + ciphertext -> tag.
                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, polyPipeline);
                vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipelineLayout, 0, 1, &authDescSet, 0, nullptr);
                vkCmdDispatch(cmdBuf, 1, 1, 1);

                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);

                vkCmdCopyBuffer(cmdBuf, outputBuffer, stagingOutputBuffer, 1, &copyRegion);
                vkCmdCopyBuffer(cmdBuf, tagBuffer, stagingTagBuffer, 1, &tagCopy);
            });

            std::vector<uint32_t> outputWords(inputWords.size(), 0);
            {
                void* mappedOut = nullptr;
                throwVk(vkMapMemory(device, stagingOutputMemory, 0, dataWordBytes, 0, &mappedOut),
                        "vkMapMemory(stagingOutput) failed");
                std::memcpy(outputWords.data(), mappedOut, static_cast<size_t>(dataWordBytes));
                vkUnmapMemory(device, stagingOutputMemory);
            }

            std::vector<uint8_t> outputData;
            unpackWordsToBytes(outputWords, outputData, inputData.size());

            std::vector<uint32_t> tagWords(4, 0);
            {
                void* mappedTag = nullptr;
                throwVk(vkMapMemory(device, stagingTagMemory, 0, tagBytes, 0, &mappedTag),
                        "vkMapMemory(stagingTag) failed");
                std::memcpy(tagWords.data(), mappedTag, static_cast<size_t>(tagBytes));
                vkUnmapMemory(device, stagingTagMemory);
            }

            std::vector<uint8_t> tagData;
            unpackWordsToBytes(tagWords, tagData, 16);

            std::cout << "Ciphertext: " << bytesToHex(outputData) << "\n";
            std::cout << "Tag:        " << bytesToHex(tagData) << "\n";
        }
        else
        {
            // First GPU pass: compute tag over ciphertext + AAD
            submitOneTimeCommands([&](VkCommandBuffer cmdBuf)
            {
                VkBufferCopy copyRegion{};
                copyRegion.size = dataWordBytes;
                VkBufferCopy tagCopy{};
                tagCopy.size = tagBytes;
                vkCmdCopyBuffer(cmdBuf, inputBuffer, authBuffer, 1, &copyRegion);

                VkMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);

                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, polyPipeline);
                vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipelineLayout, 0, 1, &authDescSet, 0, nullptr);
                vkCmdDispatch(cmdBuf, 1, 1, 1);

                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);

                vkCmdCopyBuffer(cmdBuf, tagBuffer, stagingTagBuffer, 1, &tagCopy);
            });

            std::vector<uint32_t> computedTagWords(4, 0);
            {
                void* mappedTag = nullptr;
                throwVk(vkMapMemory(device, stagingTagMemory, 0, tagBytes, 0, &mappedTag),
                        "vkMapMemory(stagingTag) failed");
                std::memcpy(computedTagWords.data(), mappedTag, static_cast<size_t>(tagBytes));
                vkUnmapMemory(device, stagingTagMemory);
            }

            std::vector<uint8_t> computedTag;
            unpackWordsToBytes(computedTagWords, computedTag, 16);

            bool tagMatches = (computedTag.size() == expectedTag.size());
            uint8_t diff = 0;
            for (size_t i = 0; i < computedTag.size() && i < expectedTag.size(); ++i)
                diff |= static_cast<uint8_t>(computedTag[i] ^ expectedTag[i]);
            tagMatches = tagMatches && (diff == 0);

            if (!tagMatches)
            {
                std::cerr << "Authentication failed: tag mismatch\n";
                shutdownAndDestroy();
                return 1;
            }

            // Second GPU pass: decrypt ciphertext -> plaintext
            submitOneTimeCommands([&](VkCommandBuffer cmdBuf)
            {
                VkBufferCopy copyRegion{};
                copyRegion.size = dataWordBytes;

                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, chachaPipeline);
                vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        pipelineLayout, 0, 1, &cryptoDescSet, 0, nullptr);
                vkCmdDispatch(cmdBuf, groupCount, 1, 1);

                VkMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmdBuf,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0,
                                     1, &barrier,
                                     0, nullptr,
                                     0, nullptr);

                vkCmdCopyBuffer(cmdBuf, outputBuffer, stagingOutputBuffer, 1, &copyRegion);
            });

            std::vector<uint32_t> outputWords(inputWords.size(), 0);
            {
                void* mappedOut = nullptr;
                throwVk(vkMapMemory(device, stagingOutputMemory, 0, dataWordBytes, 0, &mappedOut),
                        "vkMapMemory(stagingOutput) failed");
                std::memcpy(outputWords.data(), mappedOut, static_cast<size_t>(dataWordBytes));
                vkUnmapMemory(device, stagingOutputMemory);
            }

            std::vector<uint8_t> outputData;
            unpackWordsToBytes(outputWords, outputData, inputData.size());

            std::cout << "Plaintext:  " << bytesToHex(outputData) << "\n";
        }
        
        shutdownAndDestroy();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        shutdownAndDestroy();
        return 1;
    }
}