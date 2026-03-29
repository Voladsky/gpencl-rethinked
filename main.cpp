#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Helper to read a file into a string
static std::string readFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open file: " + filename);

    std::streamsize size = file.tellg();
    file.seekg(std::ios::beg);

    std::string buffer(size, '\0');
    if (!file.read(&buffer[0], size))
        throw std::runtime_error("Failed to read file: " + filename);

    return buffer;
}

// Compile GLSL file to SPIR-V using shaderc
static std::vector<uint32_t> compileShaderFile(const std::string& filename)
{
    std::string source = readFile(filename);

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    // Set the source language to GLSL and target Vulkan environment
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(source, shaderc_compute_shader, filename.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        std::cerr << "Shader compilation failed: " << result.GetErrorMessage() << std::endl;
        throw std::runtime_error("Shader compilation failed");
    }

    return {result.cbegin(), result.cend()};
}

static std::vector<uint32_t> packBytesToWords(const std::vector<uint8_t>& bytes)
{
    const size_t wordCount = (bytes.size() + 3) / 4;
    std::vector<uint32_t> words(wordCount, 0);

    for (size_t i = 0; i < bytes.size(); ++i)
    {
        size_t w = i / 4;
        size_t shift = (i % 4) * 8;
        words[w] |= (uint32_t(bytes[i]) << shift);
    }

    return words;
}

static void unpackWordsToBytes(const std::vector<uint32_t>& words, std::vector<uint8_t>& bytesOut, size_t byteCount)
{
    bytesOut.resize(byteCount);
    for (size_t i = 0; i < byteCount; ++i)
    {
        size_t w = i / 4;
        size_t shift = (i % 4) * 8;
        bytesOut[i] = uint8_t((words[w] >> shift) & 0xFFu);
    }
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
};

struct alignas(16) GpuKey256
{
    uint32_t k[8];
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cout << "Usage: " << argv[0] << " <hex_bytes> <shader_file.glsl>\n";
        std::cout << "  hex_bytes        : input bytes in hex (e.g. 0a1b2c)\n";
        std::cout << "  shader_file.glsl : path to GLSL compute shader\n";
        return 1;
    }

    std::string hex = argv[1];
    std::string shaderPath = argv[2];

    std::vector<uint8_t> inputData;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        unsigned int byte = 0;
        std::sscanf(hex.substr(i, 2).c_str(), "%02x", &byte);
        inputData.push_back(static_cast<uint8_t>(byte));
    }

    const size_t dataSize = inputData.size();
    if (dataSize == 0)
    {
        std::cerr << "No input data provided." << std::endl;
        return 1;
    }

    // Pack bytes to 32-bit words for shader consumption
    std::vector<uint32_t> inputWords = packBytesToWords(inputData);
    const uint32_t wordCount = static_cast<uint32_t>(inputWords.size());

    GpuKey256 key{};
    key.k[0] = 0x03020100;
    key.k[1] = 0x07060504;
    key.k[2] = 0x0b0a0908;
    key.k[3] = 0x0f0e0d0c;
    key.k[4] = 0x13121110;
    key.k[5] = 0x17161514;
    key.k[6] = 0x1b1a1918;
    key.k[7] = 0x1f1e1d1c;

    GpuJob job{};
    job.inputWordOffset  = 0;
    job.outputWordOffset = 0;
    job.byteSize         = static_cast<uint32_t>(dataSize);
    job.wordCount        = wordCount;
    job.keyIndex         = 0;
    job.nonce0 = 0x09000000;
    job.nonce1 = 0x4a000000;
    job.nonce2 = 0x00000000;
    job.counter0         = 1u;

    // 1. Create Vulkan instance
    VkInstance instance = VK_NULL_HANDLE;
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "ChaCha20ComputeExample";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "NoEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
        {
            std::cerr << "Failed to create Vulkan instance." << std::endl;
            return -1;
        }
    }

    // 2. Pick a physical device with a compute queue
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t computeQueueFamily = UINT32_MAX;
    {
        uint32_t devCount = 0;
        vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
        if (devCount == 0)
        {
            std::cerr << "No Vulkan devices found." << std::endl;
            return -1;
        }

        std::vector<VkPhysicalDevice> devices(devCount);
        vkEnumeratePhysicalDevices(instance, &devCount, devices.data());

        for (auto dev : devices)
        {
            // Check for a compute-capable queue
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
        {
            std::cerr << "No device with compute queue found." << std::endl;
            return -1;
        }
    }

    // 3. Create logical device and get compute queue
    VkDevice device = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    {
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = computeQueueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;

        VkPhysicalDeviceFeatures deviceFeatures{}; // no extra features needed
        VkDeviceCreateInfo devCreateInfo{};
        devCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        devCreateInfo.queueCreateInfoCount = 1;
        devCreateInfo.pQueueCreateInfos = &queueCreateInfo;
        devCreateInfo.pEnabledFeatures = &deviceFeatures;

        if (vkCreateDevice(physicalDevice, &devCreateInfo, nullptr, &device) != VK_SUCCESS)
        {
            std::cerr << "Failed to create logical device." << std::endl;
            return -1;
        }
        vkGetDeviceQueue(device, computeQueueFamily, 0, &computeQueue);
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

    // Helper: creates a buffer with specified usage and memory properties
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
        if (vkCreateBuffer(device, &bufInfo, nullptr, &buf) != VK_SUCCESS)
            throw std::runtime_error("vkCreateBuffer failed");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buf, &req);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateMemory failed");
        if (vkBindBufferMemory(device, buf, mem, 0) != VK_SUCCESS)
            throw std::runtime_error("vkBindBufferMemory failed");
    };

    // Helper: copies data between buffers synchronously using a one-time command buffer
    auto copyBuffer = [&](VkBuffer src, VkBuffer dst, VkDeviceSize size)
    {
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = computeQueueFamily;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS)
            throw std::runtime_error("vkCreateCommandPool failed");

        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = cmdPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cbAlloc, &cmdBuf) != VK_SUCCESS)
            throw std::runtime_error("vkAllocateCommandBuffers failed");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS)
            throw std::runtime_error("vkBeginCommandBuffer failed");

        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(cmdBuf, src, dst, 1, &region);
        if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS)
            throw std::runtime_error("vkEndCommandBuffer failed");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
            throw std::runtime_error("vkCreateFence failed");

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        if (vkQueueSubmit(computeQueue, 1, &submitInfo, fence) != VK_SUCCESS)
            throw std::runtime_error("vkQueueSubmit failed");

        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        vkDestroyCommandPool(device, cmdPool, nullptr);
    };

    // 4. Create buffers
    VkBuffer inputBuffer = VK_NULL_HANDLE;
    VkBuffer outputBuffer = VK_NULL_HANDLE;
    VkBuffer stagingInputBuffer = VK_NULL_HANDLE;
    VkBuffer stagingOutputBuffer = VK_NULL_HANDLE;
    VkBuffer keyBuffer = VK_NULL_HANDLE;
    VkBuffer stagingKeyBuffer = VK_NULL_HANDLE;
    VkBuffer jobBuffer = VK_NULL_HANDLE;
    VkBuffer stagingJobBuffer = VK_NULL_HANDLE;

    VkDeviceMemory inputMemory = VK_NULL_HANDLE;
    VkDeviceMemory outputMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingInputMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingOutputMemory = VK_NULL_HANDLE;
    VkDeviceMemory keyMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingKeyMemory = VK_NULL_HANDLE;
    VkDeviceMemory jobMemory = VK_NULL_HANDLE;
    VkDeviceMemory stagingJobMemory = VK_NULL_HANDLE;

    const VkDeviceSize dataWordBytes = VkDeviceSize(inputWords.size() * sizeof(uint32_t));
    const VkDeviceSize keyBytes = VkDeviceSize(sizeof(GpuKey256));
    const VkDeviceSize jobBytes = VkDeviceSize(sizeof(GpuJob));
    try
    {
        // Device-local buffers
        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     inputBuffer, inputMemory);

        createBuffer(dataWordBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     outputBuffer, outputMemory);

        createBuffer(keyBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     keyBuffer, keyMemory);

        createBuffer(jobBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     jobBuffer, jobMemory);

        // Host-visible staging buffers
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

        // Upload input words
        {
            void* mapped = nullptr;
            vkMapMemory(device, stagingInputMemory, 0, dataWordBytes, 0, &mapped);
            std::memcpy(mapped, inputWords.data(), dataWordBytes);
            vkUnmapMemory(device, stagingInputMemory);
            copyBuffer(stagingInputBuffer, inputBuffer, dataWordBytes);
        }
        // Upload key
        {
            void* mapped = nullptr;
            vkMapMemory(device, stagingKeyMemory, 0, keyBytes, 0, &mapped);
            std::memcpy(mapped, &key, keyBytes);
            vkUnmapMemory(device, stagingKeyMemory);
            copyBuffer(stagingKeyBuffer, keyBuffer, keyBytes);
        }
        // Upload job
        {
            void* mapped = nullptr;
            vkMapMemory(device, stagingJobMemory, 0, jobBytes, 0, &mapped);
            std::memcpy(mapped, &job, jobBytes);
            vkUnmapMemory(device, stagingJobMemory);
            copyBuffer(stagingJobBuffer, jobBuffer, jobBytes);
        }

        // 5. Descriptor set layout with 4 bindings
        VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        VkPipeline computePipeline = VK_NULL_HANDLE;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        {
            VkDescriptorSetLayoutBinding bindings[4]{};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[3].binding = 3;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
            descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descLayoutInfo.bindingCount = 4;
            descLayoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(device, &descLayoutInfo, nullptr, &descSetLayout) != VK_SUCCESS)
                throw std::runtime_error("vkCreateDescriptorSetLayout failed");

            VkPipelineLayoutCreateInfo pipeLayoutInfo{};
            pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeLayoutInfo.setLayoutCount = 1;
            pipeLayoutInfo.pSetLayouts = &descSetLayout;
            if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
                throw std::runtime_error("vkCreatePipelineLayout failed");
        }

        // 6. Compile shader and create pipeline
        {
            std::vector<uint32_t> spv = compileShaderFile(shaderPath);
            std::cout << "Compiled shader file '" << shaderPath << "' to SPIR-V (" << spv.size() << " words)" << std::endl;

            VkShaderModuleCreateInfo shaderInfo{};
            shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderInfo.codeSize = spv.size() * sizeof(uint32_t);
            shaderInfo.pCode = spv.data();
            if (vkCreateShaderModule(device, &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS)
                throw std::runtime_error("vkCreateShaderModule failed");

            VkPipelineShaderStageCreateInfo stageInfo{};
            stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stageInfo.module = shaderModule;
            stageInfo.pName = "main";

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.stage = stageInfo;
            pipelineInfo.layout = pipelineLayout;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline) != VK_SUCCESS)
                throw std::runtime_error("vkCreateComputePipelines failed");
        }

        // 7. Descriptor pool and descriptor set
        {
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            poolSize.descriptorCount = 4;

            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool) != VK_SUCCESS)
                throw std::runtime_error("vkCreateDescriptorPool failed");

            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &descSetLayout;
            if (vkAllocateDescriptorSets(device, &allocInfo, &descSet) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateDescriptorSets failed");

            VkDescriptorBufferInfo bufInfos[4]{};
            bufInfos[0].buffer = inputBuffer;
            bufInfos[0].offset = 0;
            bufInfos[0].range = dataWordBytes;

            bufInfos[1].buffer = outputBuffer;
            bufInfos[1].offset = 0;
            bufInfos[1].range = dataWordBytes;

            bufInfos[2].buffer = keyBuffer;
            bufInfos[2].offset = 0;
            bufInfos[2].range = keyBytes;

            bufInfos[3].buffer = jobBuffer;
            bufInfos[3].offset = 0;
            bufInfos[3].range = jobBytes;

            VkWriteDescriptorSet writes[4]{};
            for (int i = 0; i < 4; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = descSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &bufInfos[i];
            }
            vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        }

        // 8. Dispatch compute
        {
            VkCommandPool cmdPool = VK_NULL_HANDLE;
            VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
            VkFence computeFence = VK_NULL_HANDLE;

            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = computeQueueFamily;
            if (vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS)
                throw std::runtime_error("vkCreateCommandPool failed");

            VkCommandBufferAllocateInfo cmdAlloc{};
            cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cmdAlloc.commandPool = cmdPool;
            cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdAlloc.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device, &cmdAlloc, &cmdBuf) != VK_SUCCESS)
                throw std::runtime_error("vkAllocateCommandBuffers failed");

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS)
                throw std::runtime_error("vkBeginCommandBuffer failed");

            // Bind pipeline and descriptor set
            vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
            vkCmdBindDescriptorSets(cmdBuf,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipelineLayout,
                                    0, 1, &descSet,
                                    0, nullptr);

            // One invocation handles one 64-byte block
            uint32_t blockCount = static_cast<uint32_t>((dataSize + 63) / 64);
            uint32_t groupCount = (blockCount + 63) / 64; // local_size_x = 64
            vkCmdDispatch(cmdBuf, groupCount, 1, 1);

            // Memory barrier: ensure shader writes (VK_ACCESS_SHADER_WRITE) are visible before transfer read
            VkMemoryBarrier memBarrier{};
            memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmdBuf,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,
                                 1, &memBarrier,
                                 0, nullptr,
                                 0, nullptr);

            // Copy from outputBuffer to stagingBuffer for host read
            VkBufferCopy copyRegion{};
            copyRegion.size = dataWordBytes;
            vkCmdCopyBuffer(cmdBuf, outputBuffer, stagingOutputBuffer, 1, &copyRegion);
            if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS)
                throw std::runtime_error("vkEndCommandBuffer failed");

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(device, &fenceInfo, nullptr, &computeFence) != VK_SUCCESS)
                throw std::runtime_error("vkCreateFence failed");

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &cmdBuf;
            if (vkQueueSubmit(computeQueue, 1, &submitInfo, computeFence) != VK_SUCCESS)
                throw std::runtime_error("vkQueueSubmit failed");

            vkWaitForFences(device, 1, &computeFence, VK_TRUE, UINT64_MAX);

            vkDestroyFence(device, computeFence, nullptr);
            vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
            vkDestroyCommandPool(device, cmdPool, nullptr);
        }

        // 9. Read output
        std::vector<uint32_t> outputWords(inputWords.size(), 0);
        {
            void* mappedOut = nullptr;
            vkMapMemory(device, stagingOutputMemory, 0, dataWordBytes, 0, &mappedOut);
            std::memcpy(outputWords.data(), mappedOut, dataWordBytes);
            vkUnmapMemory(device, stagingOutputMemory);
        }

        std::vector<uint8_t> outputData;
        unpackWordsToBytes(outputWords, outputData, dataSize);

        std::cout << "Output bytes: ";
        for (uint8_t b : outputData)
            std::printf("%02x", b);
        std::cout << std::endl;

        // 10. Cleanup Vulkan objects created in try block
        vkDestroyShaderModule(device, shaderModule, nullptr);
        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyDescriptorPool(device, descPool, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        if (device != VK_NULL_HANDLE)
        {
            if (jobBuffer) vkDestroyBuffer(device, jobBuffer, nullptr);
            if (jobMemory) vkFreeMemory(device, jobMemory, nullptr);
            if (stagingJobBuffer) vkDestroyBuffer(device, stagingJobBuffer, nullptr);
            if (stagingJobMemory) vkFreeMemory(device, stagingJobMemory, nullptr);

            if (keyBuffer) vkDestroyBuffer(device, keyBuffer, nullptr);
            if (keyMemory) vkFreeMemory(device, keyMemory, nullptr);
            if (stagingKeyBuffer) vkDestroyBuffer(device, stagingKeyBuffer, nullptr);
            if (stagingKeyMemory) vkFreeMemory(device, stagingKeyMemory, nullptr);

            if (inputBuffer) vkDestroyBuffer(device, inputBuffer, nullptr);
            if (inputMemory) vkFreeMemory(device, inputMemory, nullptr);
            if (outputBuffer) vkDestroyBuffer(device, outputBuffer, nullptr);
            if (outputMemory) vkFreeMemory(device, outputMemory, nullptr);
            if (stagingInputBuffer) vkDestroyBuffer(device, stagingInputBuffer, nullptr);
            if (stagingInputMemory) vkFreeMemory(device, stagingInputMemory, nullptr);
            if (stagingOutputBuffer) vkDestroyBuffer(device, stagingOutputBuffer, nullptr);
            if (stagingOutputMemory) vkFreeMemory(device, stagingOutputMemory, nullptr);

            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);

        return 1;
    }

    // Cleanup buffers and device
    vkDestroyBuffer(device, jobBuffer, nullptr);
    vkFreeMemory(device, jobMemory, nullptr);
    vkDestroyBuffer(device, stagingJobBuffer, nullptr);
    vkFreeMemory(device, stagingJobMemory, nullptr);

    vkDestroyBuffer(device, keyBuffer, nullptr);
    vkFreeMemory(device, keyMemory, nullptr);
    vkDestroyBuffer(device, stagingKeyBuffer, nullptr);
    vkFreeMemory(device, stagingKeyMemory, nullptr);

    vkDestroyBuffer(device, inputBuffer, nullptr);
    vkFreeMemory(device, inputMemory, nullptr);
    vkDestroyBuffer(device, outputBuffer, nullptr);
    vkFreeMemory(device, outputMemory, nullptr);
    vkDestroyBuffer(device, stagingInputBuffer, nullptr);
    vkFreeMemory(device, stagingInputMemory, nullptr);
    vkDestroyBuffer(device, stagingOutputBuffer, nullptr);
    vkFreeMemory(device, stagingOutputMemory, nullptr);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    return 0;
}
