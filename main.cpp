#include <vulkan/vulkan.h>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <sstream>
#include <shaderc/shaderc.hpp>

// Helper to read a file into a string
static std::string readFile(const std::string &filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string buffer(size, '\0');
    if (file.read(&buffer[0], size))
    {
        return buffer;
    }
    throw std::runtime_error("Failed to read file: " + filename);
}

// Compile GLSL file to SPIR-V using shaderc
static std::vector<uint32_t> compileShaderFile(const std::string &filename)
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

int main(int argc, char **argv)
{

    // Parse command-line arguments
    if (argc < 3)
    {
        std::cout << "Usage: " << argv[0] << " <hex_bytes> <shader_file.glsl>" << std::endl;
        std::cout << "  hex_bytes : input bytes in hex (e.g. 0a1b2c)" << std::endl;
        std::cout << "  shader_file.glsl : path to GLSL compute shader" << std::endl;
        return 1;
    }

    std::string hex = argv[1];
    std::string shaderPath = argv[2];

    std::vector<uint8_t> inputData;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        unsigned int byte;
        std::sscanf(hex.substr(i, 2).c_str(), "%02x", &byte);
        inputData.push_back(static_cast<uint8_t>(byte));
    }
    size_t dataSize = inputData.size();
    if (dataSize == 0)
    {
        std::cerr << "No input data provided." << std::endl;
        return 1;
    }

    // 1. Create Vulkan instance
    VkInstance instance;
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "ComputeXorExample";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "NoEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        // No special extensions or layers for compute
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
            for (uint32_t i = 0; i < qCount; i++)
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
    VkDevice device;
    VkQueue computeQueue;
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

    // 4. Create buffers: input (device-local), output (device-local), and staging (host-visible)
    VkBuffer inputBuffer, outputBuffer, stagingBuffer;
    VkDeviceMemory inputMemory, outputMemory, stagingMemory;
    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags props,
                            VkBuffer &buf, VkDeviceMemory &mem)
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &buf);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buf, &req);
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
        {
            if ((req.memoryTypeBits & (1 << i)) &&
                (memProps.memoryTypes[i].propertyFlags & props) == props)
            {
                idx = i;
                break;
            }
        }
        if (idx == UINT32_MAX)
        {
            throw std::runtime_error("Failed to find suitable memory type");
        }
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = idx;
        vkAllocateMemory(device, &allocInfo, nullptr, &mem);
        vkBindBufferMemory(device, buf, mem, 0);
    };

    VkDeviceSize alignedSize = (dataSize + 3) & ~3; // align size to 4 bytes
    // Device-local buffer for input data (will be updated via transfer)
    createBuffer(alignedSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 inputBuffer, inputMemory);
    // Device-local buffer for output data (to be copied back)
    createBuffer(alignedSize,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 outputBuffer, outputMemory);
    // Host-visible staging buffer for transfers
    createBuffer(alignedSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    // 5. Copy input bytes into the staging buffer (host->GPU)
    {
        void *mapped;
        vkMapMemory(device, stagingMemory, 0, alignedSize, 0, &mapped);
        std::memcpy(mapped, inputData.data(), dataSize);
        vkUnmapMemory(device, stagingMemory);

        // Record command buffer to copy staging -> inputBuffer
        VkCommandPool cmdPool;
        VkCommandBuffer cmdBuf;
        VkFence fence;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = computeQueueFamily;
        vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);

        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool = cmdPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cbAlloc, &cmdBuf);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);
        VkBufferCopy copyRegion{};
        copyRegion.size = alignedSize;
        vkCmdCopyBuffer(cmdBuf, stagingBuffer, inputBuffer, 1, &copyRegion);
        vkEndCommandBuffer(cmdBuf);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(device, &fenceInfo, nullptr, &fence);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(computeQueue, 1, &submitInfo, fence);
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        // Cleanup copy resources
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        vkDestroyCommandPool(device, cmdPool, nullptr);
    }

    // 6. Create descriptor set layout and pipeline layout
    VkDescriptorSetLayout descSetLayout;
    VkPipelineLayout pipelineLayout;
    {
        VkDescriptorSetLayoutBinding bindings[2];
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[0].pImmutableSamplers = nullptr;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
        descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descLayoutInfo.bindingCount = 2;
        descLayoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &descLayoutInfo, nullptr, &descSetLayout);

        VkPipelineLayoutCreateInfo pipeLayoutInfo{};
        pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &descSetLayout;
        vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &pipelineLayout);
    }

    // 7. Create compute shader module (embedded SPIR-V for XOR)
    VkShaderModule shaderModule;
    {
        std::vector<uint32_t> spv;
        try
        {
            spv = compileShaderFile(shaderPath);
            std::cout << "Compiled shader file '" << shaderPath << "' to SPIR-V ("
                      << spv.size() << " words)" << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error compiling shader: " << e.what() << std::endl;
            return 1;
        }

        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderInfo.codeSize = spv.size() * sizeof(uint32_t);
        shaderInfo.pCode = spv.data();
        if (vkCreateShaderModule(device, &shaderInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            std::cerr << "Failed to create shader module." << std::endl;
            return -1;
        }
    }

    // 8. Create compute pipeline
    VkPipeline computePipeline;
    {
        VkPipelineShaderStageCreateInfo stageInfo{};
        stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stageInfo.module = shaderModule;
        stageInfo.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stageInfo;
        pipelineInfo.layout = pipelineLayout;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline);
    }

    // 9. Allocate descriptor set and write buffer bindings
    VkDescriptorPool descPool;
    VkDescriptorSet descSet;
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 2;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descSetLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &descSet);

        VkDescriptorBufferInfo bufInfos[2];
        bufInfos[0].buffer = inputBuffer;
        bufInfos[0].offset = 0;
        bufInfos[0].range = alignedSize;
        bufInfos[1].buffer = outputBuffer;
        bufInfos[1].offset = 0;
        bufInfos[1].range = alignedSize;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &bufInfos[0];
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &bufInfos[1];

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // 10. Record command buffer: dispatch compute and copy output back to host
    std::vector<uint8_t> outputData(dataSize);
    {
        VkCommandPool cmdPool;
        VkCommandBuffer cmdBuf;
        VkFence computeFence;
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = computeQueueFamily;
        vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);

        VkCommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAlloc.commandPool = cmdPool;
        cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAlloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cmdAlloc, &cmdBuf);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);

        // Bind pipeline and descriptor set
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipelineLayout, 0, 1, &descSet, 0, nullptr);
        // Dispatch: (dataSize+255)/256 workgroups
        uint32_t groupCount = uint32_t((dataSize + 255) / 256);
        vkCmdDispatch(cmdBuf, groupCount, 1, 1);

        // Memory barrier: ensure shader writes (VK_ACCESS_SHADER_WRITE) are visible before transfer read
        VkMemoryBarrier memBarrier{};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmdBuf,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &memBarrier, 0, nullptr, 0, nullptr);

        // Copy from outputBuffer to stagingBuffer for host read
        VkBufferCopy copyRegion{};
        copyRegion.size = alignedSize;
        vkCmdCopyBuffer(cmdBuf, outputBuffer, stagingBuffer, 1, &copyRegion);

        vkEndCommandBuffer(cmdBuf);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(device, &fenceInfo, nullptr, &computeFence);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        vkQueueSubmit(computeQueue, 1, &submitInfo, computeFence);
        vkWaitForFences(device, 1, &computeFence, VK_TRUE, UINT64_MAX);

        // Map stagingBuffer to get output bytes
        void *mappedOut;
        vkMapMemory(device, stagingMemory, 0, alignedSize, 0, &mappedOut);
        std::memcpy(outputData.data(), mappedOut, dataSize);
        vkUnmapMemory(device, stagingMemory);

        vkDestroyFence(device, computeFence, nullptr);
        vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        vkDestroyCommandPool(device, cmdPool, nullptr);
    }

    // 11. Print encrypted output (hex)
    std::cout << "Encrypted bytes: ";
    for (uint8_t b : outputData)
    {
        printf("%02x", b);
    }
    std::cout << std::endl;

    // 12. Cleanup Vulkan resources
    vkDestroyShaderModule(device, shaderModule, nullptr);
    vkDestroyPipeline(device, computePipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkFreeMemory(device, inputMemory, nullptr);
    vkFreeMemory(device, outputMemory, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, inputBuffer, nullptr);
    vkDestroyBuffer(device, outputBuffer, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
