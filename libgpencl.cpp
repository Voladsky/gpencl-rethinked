#include "libgpencl.h"
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>
#include <algorithm>
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
#include <random>
#include <memory>

// Global Vulkan state
static VkInstance g_instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
static uint32_t g_computeQueueFamily = UINT32_MAX;
static VkDevice g_device = VK_NULL_HANDLE;
static VkQueue g_computeQueue = VK_NULL_HANDLE;
static VkCommandPool g_commandPool = VK_NULL_HANDLE;

static VkDescriptorSetLayout g_descSetLayout = VK_NULL_HANDLE;
static VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
static VkShaderModule g_chachaShaderModule = VK_NULL_HANDLE;
static VkShaderModule g_polyShaderModule = VK_NULL_HANDLE;
static VkPipeline g_chachaPipeline = VK_NULL_HANDLE;
static VkPipeline g_polyPipeline = VK_NULL_HANDLE;
static VkDescriptorPool g_descPool = VK_NULL_HANDLE;

static std::unique_ptr<shaderc::Compiler> g_compiler;

struct alignas(16) GpuJob {
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

struct alignas(16) GpuKey256 {
    uint32_t k[8];
};

// Helper functions
static void throwVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        std::ostringstream oss;
        oss << what << " (VkResult=" << result << ")";
        throw std::runtime_error(oss.str());
    }
}

static std::string readFile(const std::string& filename) {
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

static std::vector<uint32_t> compileShaderFile(const std::string& filename) {
    const std::string source = readFile(filename);

    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    shaderc::SpvCompilationResult result =
        g_compiler->CompileGlslToSpv(source, shaderc_compute_shader, filename.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::cerr << "Shader compilation failed (" << filename << "): "
                  << result.GetErrorMessage() << std::endl;
        throw std::runtime_error("Shader compilation failed");
    }

    return {result.cbegin(), result.cend()};
}

static std::vector<uint32_t> packBytesToWords(const std::vector<uint8_t>& bytes) {
    if (bytes.empty())
        return std::vector<uint32_t>(1, 0u);

    const size_t wordCount = (bytes.size() + 3) / 4;
    std::vector<uint32_t> words(wordCount, 0);

    for (size_t i = 0; i < bytes.size(); ++i) {
        const size_t w = i / 4;
        const size_t shift = (i % 4) * 8;
        words[w] |= (uint32_t(bytes[i]) << shift);
    }

    return words;
}

static void unpackWordsToBytes(const std::vector<uint32_t>& words, std::vector<uint8_t>& bytesOut, size_t byteCount) {
    bytesOut.resize(byteCount);
    for (size_t i = 0; i < byteCount; ++i) {
        const size_t w = i / 4;
        const size_t shift = (i % 4) * 8;
        bytesOut[i] = uint8_t((words[w] >> shift) & 0xFFu);
    }
}

static uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
            if (memProps.memoryHeaps[memProps.memoryTypes[i].heapIndex].size > 0)
                return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

static void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                         VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    throwVk(vkCreateBuffer(g_device, &bufInfo, nullptr, &buf), "vkCreateBuffer failed");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, buf, &req);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);

    throwVk(vkAllocateMemory(g_device, &allocInfo, nullptr, &mem), "vkAllocateMemory failed");
    throwVk(vkBindBufferMemory(g_device, buf, mem, 0), "vkBindBufferMemory failed");
}

static void destroyBuffer(VkBuffer& buf, VkDeviceMemory& mem) {
    if (buf != VK_NULL_HANDLE) {
        vkDestroyBuffer(g_device, buf, nullptr);
        buf = VK_NULL_HANDLE;
    }
    if (mem != VK_NULL_HANDLE) {
        vkFreeMemory(g_device, mem, nullptr);
        mem = VK_NULL_HANDLE;
    }
}

static void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    throwVk(vkAllocateCommandBuffers(g_device, &allocInfo, &cmdBuf), "vkAllocateCommandBuffers failed");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = size;
    vkCmdCopyBuffer(cmdBuf, src, dst, 1, &region);

    vkEndCommandBuffer(cmdBuf);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(g_device, &fenceInfo, nullptr, &fence);

    vkQueueSubmit(g_computeQueue, 1, &submitInfo, fence);
    vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(g_device, fence, nullptr);
    vkFreeCommandBuffers(g_device, g_commandPool, 1, &cmdBuf);
}

static void uploadMappedBuffer(VkDeviceMemory memory, VkDeviceSize size, const void* src) {
    void* mapped = nullptr;
    throwVk(vkMapMemory(g_device, memory, 0, size, 0, &mapped), "vkMapMemory failed");
    std::memcpy(mapped, src, static_cast<size_t>(size));
    vkUnmapMemory(g_device, memory);
}

// Simple PBKDF2-like key derivation from password
static std::vector<uint8_t> deriveKey(const std::string& password, const std::vector<uint8_t>& salt) {
    std::vector<uint8_t> key(32, 0);
    const size_t iterations = 10000;
    
    // Simple PBKDF2-like construction
    std::vector<uint8_t> input;
    input.insert(input.end(), password.begin(), password.end());
    input.insert(input.end(), salt.begin(), salt.end());
    
    // Hash iterations
    for (size_t iter = 0; iter < iterations; ++iter) {
        for (size_t i = 0; i < input.size() && i < key.size(); ++i) {
            key[i] ^= input[i];
        }
        // Simple mixing
        for (size_t i = 0; i < key.size(); ++i) {
            key[i] = (key[i] * 13 + 7) & 0xFF;
        }
    }
    
    return key;
}

// Public API implementation
namespace GpEncl {

bool init() {
    try {
        // Initialize shader compiler first (needed for shader compilation later)
        if (!g_compiler) {
            g_compiler = std::make_unique<shaderc::Compiler>();
        }

        // Create Vulkan instance
        {
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "GPENCL";
            appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.pEngineName = "NoEngine";
            appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.apiVersion = VK_API_VERSION_1_0;

            VkInstanceCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            createInfo.pApplicationInfo = &appInfo;

            throwVk(vkCreateInstance(&createInfo, nullptr, &g_instance), "Failed to create Vulkan instance");
        }

        // Select physical device with compute queue
        {
            uint32_t devCount = 0;
            throwVk(vkEnumeratePhysicalDevices(g_instance, &devCount, nullptr), "vkEnumeratePhysicalDevices failed");
            if (devCount == 0)
                throw std::runtime_error("No Vulkan devices found");

            std::vector<VkPhysicalDevice> devices(devCount);
            throwVk(vkEnumeratePhysicalDevices(g_instance, &devCount, devices.data()), "vkEnumeratePhysicalDevices failed");

            for (auto dev : devices) {
                uint32_t qCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
                std::vector<VkQueueFamilyProperties> qprops(qCount);
                vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, qprops.data());

                for (uint32_t i = 0; i < qCount; ++i) {
                    if (qprops[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                        g_physicalDevice = dev;
                        g_computeQueueFamily = i;
                        break;
                    }
                }

                if (g_physicalDevice != VK_NULL_HANDLE)
                    break;
            }

            if (g_physicalDevice == VK_NULL_HANDLE)
                throw std::runtime_error("No device with compute queue found");
        }

        // Create logical device
        {
            float queuePriority = 1.0f;

            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = g_computeQueueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;

            VkPhysicalDeviceFeatures deviceFeatures{};

            VkDeviceCreateInfo devCreateInfo{};
            devCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            devCreateInfo.queueCreateInfoCount = 1;
            devCreateInfo.pQueueCreateInfos = &queueCreateInfo;
            devCreateInfo.pEnabledFeatures = &deviceFeatures;

            throwVk(vkCreateDevice(g_physicalDevice, &devCreateInfo, nullptr, &g_device), "Failed to create logical device");
            vkGetDeviceQueue(g_device, g_computeQueueFamily, 0, &g_computeQueue);
        }

        // Create command pool
        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = g_computeQueueFamily;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            throwVk(vkCreateCommandPool(g_device, &poolInfo, nullptr, &g_commandPool), "vkCreateCommandPool failed");
        }

        // Create descriptor set layout
        {
            VkDescriptorSetLayoutBinding bindings[6]{};
            for (int i = 0; i < 6; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }

            VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
            descLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descLayoutInfo.bindingCount = 6;
            descLayoutInfo.pBindings = bindings;
            throwVk(vkCreateDescriptorSetLayout(g_device, &descLayoutInfo, nullptr, &g_descSetLayout),
                    "vkCreateDescriptorSetLayout failed");

            VkPipelineLayoutCreateInfo pipeLayoutInfo{};
            pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipeLayoutInfo.setLayoutCount = 1;
            pipeLayoutInfo.pSetLayouts = &g_descSetLayout;
            throwVk(vkCreatePipelineLayout(g_device, &pipeLayoutInfo, nullptr, &g_pipelineLayout),
                    "vkCreatePipelineLayout failed");
        }

        // Compile shaders and create pipelines
        {
            const std::vector<uint32_t> chachaSpv = compileShaderFile("shaders/chacha20.comp");
            const std::vector<uint32_t> polySpv   = compileShaderFile("shaders/poly1305.comp");

            VkShaderModuleCreateInfo shaderInfo{};
            shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            shaderInfo.codeSize = chachaSpv.size() * sizeof(uint32_t);
            shaderInfo.pCode = chachaSpv.data();
            throwVk(vkCreateShaderModule(g_device, &shaderInfo, nullptr, &g_chachaShaderModule),
                    "vkCreateShaderModule(chacha) failed");

            shaderInfo.codeSize = polySpv.size() * sizeof(uint32_t);
            shaderInfo.pCode = polySpv.data();
            throwVk(vkCreateShaderModule(g_device, &shaderInfo, nullptr, &g_polyShaderModule),
                    "vkCreateShaderModule(poly1305) failed");

            VkPipelineShaderStageCreateInfo stageInfo{};
            stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stageInfo.pName = "main";

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = g_pipelineLayout;

            stageInfo.module = g_chachaShaderModule;
            pipelineInfo.stage = stageInfo;
            throwVk(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_chachaPipeline),
                    "vkCreateComputePipelines(chacha) failed");

            stageInfo.module = g_polyShaderModule;
            pipelineInfo.stage = stageInfo;
            throwVk(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_polyPipeline),
                    "vkCreateComputePipelines(poly1305) failed");
        }

        // Create descriptor pool
        {
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            poolSize.descriptorCount = 12;

            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = 2;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;
            throwVk(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_descPool),
                    "vkCreateDescriptorPool failed");
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Init failed: " << e.what() << std::endl;
        return false;
    }
}

void shutdown() {
    if (g_device == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(g_device);

    if (g_descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(g_device, g_descPool, nullptr);
    if (g_chachaShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, g_chachaShaderModule, nullptr);
    if (g_polyShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(g_device, g_polyShaderModule, nullptr);
    if (g_chachaPipeline != VK_NULL_HANDLE) vkDestroyPipeline(g_device, g_chachaPipeline, nullptr);
    if (g_polyPipeline != VK_NULL_HANDLE) vkDestroyPipeline(g_device, g_polyPipeline, nullptr);
    if (g_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    if (g_descSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(g_device, g_descSetLayout, nullptr);
    if (g_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(g_device, g_commandPool, nullptr);

    g_compiler.reset();

    if (g_device != VK_NULL_HANDLE) {
        vkDestroyDevice(g_device, nullptr);
        g_device = VK_NULL_HANDLE;
    }
    if (g_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }

    g_physicalDevice = VK_NULL_HANDLE;
    g_computeQueue = VK_NULL_HANDLE;
}

bool encrypt_file(const std::string& password, const std::string& input_file, const std::string& output_file) {
    try {
        std::ifstream in(input_file, std::ios::binary);
        if (!in) return false;
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto encrypted = encrypt_buffer(password, buffer);
        std::ofstream out(output_file, std::ios::binary);
        if (!out) return false;
        out.write((char*)encrypted.data(), encrypted.size());
        return true;
    } catch (...) {
        return false;
    }
}

bool decrypt_file(const std::string& password, const std::string& input_file, const std::string& output_file) {
    try {
        std::ifstream in(input_file, std::ios::binary);
        if (!in) return false;
        std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto decrypted = decrypt_buffer(password, buffer);
        if (decrypted.empty()) return false;
        std::ofstream out(output_file, std::ios::binary);
        if (!out) return false;
        out.write((char*)decrypted.data(), decrypted.size());
        return true;
    } catch (...) {
        return false;
    }
}

bool encrypt_files(const std::vector<std::string>& passwords, const std::vector<std::string>& input_files,
                   const std::vector<std::string>& output_files) {
    if (passwords.empty() || input_files.size() != output_files.size())
        return false;
    for (size_t i = 0; i < input_files.size(); ++i) {
        const std::string& password = passwords[i % passwords.size()];
        if (!encrypt_file(password, input_files[i], output_files[i]))
            return false;
    }
    return true;
}

bool decrypt_files(const std::vector<std::string>& passwords, const std::vector<std::string>& input_files,
                   const std::vector<std::string>& output_files) {
    if (passwords.empty() || input_files.size() != output_files.size())
        return false;
    for (size_t i = 0; i < input_files.size(); ++i) {
        const std::string& password = passwords[i % passwords.size()];
        if (!decrypt_file(password, input_files[i], output_files[i]))
            return false;
    }
    return true;
}

std::vector<uint8_t> encrypt_buffer(const std::string& password, const std::vector<uint8_t>& buffer) {
    try {
        if (buffer.empty())
            return std::vector<uint8_t>();
        
        // Store original length
        uint64_t originalLength = buffer.size();
        
        // Generate random nonce and fixed salt for key derivation
        std::random_device rd;
        std::vector<uint8_t> nonce(12);
        for (auto& b : nonce) b = rd() % 256;
        
        std::vector<uint8_t> salt = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
        std::vector<uint8_t> key = deriveKey(password, salt);
        
        // Prepare data for GPU: input buffer, output buffer, job descriptor
        const size_t inputSize = buffer.size();
        const size_t outputSize = inputSize + 16;  // ciphertext + auth tag
        
        // Pack input to words
        std::vector<uint32_t> inputWords = packBytesToWords(buffer);
        std::vector<uint32_t> keyWords = packBytesToWords(key);
        std::vector<uint32_t> nonceWords = packBytesToWords(nonce);
        
        // Create GPU buffers
        VkBuffer inputBuf = VK_NULL_HANDLE, outputBuf = VK_NULL_HANDLE;
        VkDeviceMemory inputMem = VK_NULL_HANDLE, outputMem = VK_NULL_HANDLE;
        VkBuffer keyBuf = VK_NULL_HANDLE, jobBuf = VK_NULL_HANDLE;
        VkDeviceMemory keyMem = VK_NULL_HANDLE, jobMem = VK_NULL_HANDLE;
        
        createBuffer(inputWords.size() * sizeof(uint32_t), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     inputBuf, inputMem);
        
        createBuffer(outputSize + 16, 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     outputBuf, outputMem);
        
        createBuffer(keyWords.size() * sizeof(uint32_t), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     keyBuf, keyMem);
        
        GpuJob job{};
        job.inputWordOffset = 0;
        job.outputWordOffset = 0;
        job.byteSize = inputSize;
        job.wordCount = inputWords.size();
        job.keyIndex = 0;
        job.nonce0 = nonceWords.size() > 0 ? nonceWords[0] : 0;
        job.nonce1 = nonceWords.size() > 1 ? nonceWords[1] : 0;
        job.nonce2 = nonceWords.size() > 2 ? nonceWords[2] : 0;
        job.counter0 = 0;
        job.aadWordOffset = 0;
        job.aadByteSize = 0;
        
        createBuffer(sizeof(GpuJob), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     jobBuf, jobMem);
        
        // Upload data
        uploadMappedBuffer(inputMem, inputWords.size() * sizeof(uint32_t), inputWords.data());
        uploadMappedBuffer(keyMem, keyWords.size() * sizeof(uint32_t), keyWords.data());
        uploadMappedBuffer(jobMem, sizeof(GpuJob), &job);
        
        // Create descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = g_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &g_descSetLayout;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        throwVk(vkAllocateDescriptorSets(g_device, &allocInfo, &descSet), "vkAllocateDescriptorSets failed");
        
        // Update descriptor set
        VkDescriptorBufferInfo bufferInfos[6]{};
        bufferInfos[0].buffer = inputBuf;
        bufferInfos[0].offset = 0;
        bufferInfos[0].range = inputWords.size() * sizeof(uint32_t);
        
        bufferInfos[1].buffer = outputBuf;
        bufferInfos[1].offset = 0;
        bufferInfos[1].range = outputSize + 16;
        
        bufferInfos[2].buffer = keyBuf;
        bufferInfos[2].offset = 0;
        bufferInfos[2].range = keyWords.size() * sizeof(uint32_t);
        
        bufferInfos[3].buffer = jobBuf;
        bufferInfos[3].offset = 0;
        bufferInfos[3].range = sizeof(GpuJob);
        
        bufferInfos[4].buffer = jobBuf;
        bufferInfos[4].offset = 0;
        bufferInfos[4].range = sizeof(GpuJob);
        
        bufferInfos[5].buffer = outputBuf;
        bufferInfos[5].offset = 0;
        bufferInfos[5].range = outputSize + 16;
        
        VkWriteDescriptorSet writes[6]{};
        for (int i = 0; i < 6; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(g_device, 6, writes, 0, nullptr);
        
        // Execute ChaCha20 shader
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = g_commandPool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        throwVk(vkAllocateCommandBuffers(g_device, &cmdAllocInfo, &cmdBuf), "vkAllocateCommandBuffers failed");
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);
        
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_chachaPipeline);
        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipelineLayout, 0, 1, &descSet, 0, nullptr);
        
        uint32_t groupCountX = (inputSize + 255) / 256;
        vkCmdDispatch(cmdBuf, groupCountX, 1, 1);
        
        vkEndCommandBuffer(cmdBuf);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(g_device, &fenceInfo, nullptr, &fence);
        
        vkQueueSubmit(g_computeQueue, 1, &submitInfo, fence);
        vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
        
        // Read output
        void* outputMapped = nullptr;
        vkMapMemory(g_device, outputMem, 0, outputSize + 16, 0, &outputMapped);
        std::vector<uint8_t> ciphertext(outputSize + 16);
        std::memcpy(ciphertext.data(), outputMapped, outputSize + 16);
        vkUnmapMemory(g_device, outputMem);
        
        // Cleanup
        vkDestroyFence(g_device, fence, nullptr);
        vkFreeCommandBuffers(g_device, g_commandPool, 1, &cmdBuf);
        vkFreeDescriptorSets(g_device, g_descPool, 1, &descSet);
        
        destroyBuffer(inputBuf, inputMem);
        destroyBuffer(outputBuf, outputMem);
        destroyBuffer(keyBuf, keyMem);
        destroyBuffer(jobBuf, jobMem);
        
        // Return length (8 bytes) + nonce + ciphertext
        std::vector<uint8_t> result;
        for (int i = 0; i < 8; ++i) {
            result.push_back((originalLength >> (i * 8)) & 0xFF);
        }
        result.insert(result.end(), nonce.begin(), nonce.end());
        result.insert(result.end(), ciphertext.begin(), ciphertext.end());
        return result;
    } catch (const std::exception& e) {
        std::cerr << "Encryption failed: " << e.what() << std::endl;
        return std::vector<uint8_t>();
    }
}

std::vector<uint8_t> decrypt_buffer(const std::string& password, const std::vector<uint8_t>& encrypted_buffer) {
    try {
        // Encrypted format: length (8 bytes) + nonce (12 bytes) + ciphertext + tag (16 bytes)
        if (encrypted_buffer.size() < 36)  // 8 (length) + 12 (nonce) + 16 (tag minimum)
            return std::vector<uint8_t>();
        
        // Extract original length
        uint64_t originalLength = 0;
        for (int i = 0; i < 8; ++i) {
            originalLength |= (uint64_t(encrypted_buffer[i]) << (i * 8));
        }
        
        // Extract nonce
        std::vector<uint8_t> nonce(encrypted_buffer.begin() + 8, encrypted_buffer.begin() + 20);
        
        // Extract data (ciphertext + tag)
        const size_t dataSize = encrypted_buffer.size() - 20;  // Skip 8-byte length + 12-byte nonce
        std::vector<uint8_t> data(encrypted_buffer.begin() + 20, encrypted_buffer.end());
        
        // Derive key
        std::vector<uint8_t> salt = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
        std::vector<uint8_t> key = deriveKey(password, salt);
        
        // Pack for GPU
        std::vector<uint32_t> inputWords = packBytesToWords(data);
        std::vector<uint32_t> keyWords = packBytesToWords(key);
        std::vector<uint32_t> nonceWords = packBytesToWords(nonce);
        
        // Create GPU buffers
        VkBuffer inputBuf = VK_NULL_HANDLE, outputBuf = VK_NULL_HANDLE;
        VkDeviceMemory inputMem = VK_NULL_HANDLE, outputMem = VK_NULL_HANDLE;
        VkBuffer keyBuf = VK_NULL_HANDLE, jobBuf = VK_NULL_HANDLE;
        VkDeviceMemory keyMem = VK_NULL_HANDLE, jobMem = VK_NULL_HANDLE;
        
        createBuffer(inputWords.size() * sizeof(uint32_t), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     inputBuf, inputMem);
        
        createBuffer(dataSize, 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     outputBuf, outputMem);
        
        createBuffer(keyWords.size() * sizeof(uint32_t), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     keyBuf, keyMem);
        
        GpuJob job{};
        job.inputWordOffset = 0;
        job.outputWordOffset = 0;
        job.byteSize = dataSize - 16;  // Exclude auth tag from plaintext size
        job.wordCount = inputWords.size();
        job.keyIndex = 0;
        job.nonce0 = nonceWords.size() > 0 ? nonceWords[0] : 0;
        job.nonce1 = nonceWords.size() > 1 ? nonceWords[1] : 0;
        job.nonce2 = nonceWords.size() > 2 ? nonceWords[2] : 0;
        job.counter0 = 0;
        job.aadWordOffset = 0;
        job.aadByteSize = 0;
        
        createBuffer(sizeof(GpuJob), 
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     jobBuf, jobMem);
        
        // Upload data
        uploadMappedBuffer(inputMem, inputWords.size() * sizeof(uint32_t), inputWords.data());
        uploadMappedBuffer(keyMem, keyWords.size() * sizeof(uint32_t), keyWords.data());
        uploadMappedBuffer(jobMem, sizeof(GpuJob), &job);
        
        // Create descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = g_descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &g_descSetLayout;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
        throwVk(vkAllocateDescriptorSets(g_device, &allocInfo, &descSet), "vkAllocateDescriptorSets failed");
        
        // Update descriptor set
        VkDescriptorBufferInfo bufferInfos[6]{};
        bufferInfos[0].buffer = inputBuf;
        bufferInfos[0].offset = 0;
        bufferInfos[0].range = inputWords.size() * sizeof(uint32_t);
        
        bufferInfos[1].buffer = outputBuf;
        bufferInfos[1].offset = 0;
        bufferInfos[1].range = dataSize;
        
        bufferInfos[2].buffer = keyBuf;
        bufferInfos[2].offset = 0;
        bufferInfos[2].range = keyWords.size() * sizeof(uint32_t);
        
        bufferInfos[3].buffer = jobBuf;
        bufferInfos[3].offset = 0;
        bufferInfos[3].range = sizeof(GpuJob);
        
        bufferInfos[4].buffer = jobBuf;
        bufferInfos[4].offset = 0;
        bufferInfos[4].range = sizeof(GpuJob);
        
        bufferInfos[5].buffer = outputBuf;
        bufferInfos[5].offset = 0;
        bufferInfos[5].range = dataSize;
        
        VkWriteDescriptorSet writes[6]{};
        for (int i = 0; i < 6; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(g_device, 6, writes, 0, nullptr);
        
        // Execute ChaCha20 shader (for decryption, ChaCha20 stream is XOR'd with ciphertext)
        VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmdAllocInfo{};
        cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdAllocInfo.commandPool = g_commandPool;
        cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdAllocInfo.commandBufferCount = 1;
        throwVk(vkAllocateCommandBuffers(g_device, &cmdAllocInfo, &cmdBuf), "vkAllocateCommandBuffers failed");
        
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmdBuf, &beginInfo);
        
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_chachaPipeline);
        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipelineLayout, 0, 1, &descSet, 0, nullptr);
        
        uint32_t groupCountX = (dataSize + 255) / 256;
        vkCmdDispatch(cmdBuf, groupCountX, 1, 1);
        
        vkEndCommandBuffer(cmdBuf);
        
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuf;
        
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(g_device, &fenceInfo, nullptr, &fence);
        
        vkQueueSubmit(g_computeQueue, 1, &submitInfo, fence);
        vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
        
        // Read output
        void* outputMapped = nullptr;
        vkMapMemory(g_device, outputMem, 0, dataSize, 0, &outputMapped);
        
        // Trim to original length (skip padding)
        uint64_t trimSize = originalLength < (uint64_t)(dataSize - 16) ? originalLength : (uint64_t)(dataSize - 16);
        std::vector<uint8_t> plaintext(trimSize);
        std::memcpy(plaintext.data(), outputMapped, trimSize);
        vkUnmapMemory(g_device, outputMem);
        
        // Cleanup
        vkDestroyFence(g_device, fence, nullptr);
        vkFreeCommandBuffers(g_device, g_commandPool, 1, &cmdBuf);
        vkFreeDescriptorSets(g_device, g_descPool, 1, &descSet);
        
        destroyBuffer(inputBuf, inputMem);
        destroyBuffer(outputBuf, outputMem);
        destroyBuffer(keyBuf, keyMem);
        destroyBuffer(jobBuf, jobMem);
        
        return plaintext;
    } catch (const std::exception& e) {
        std::cerr << "Decryption failed: " << e.what() << std::endl;
        return std::vector<uint8_t>();
    }
}

std::vector<std::string> probe_devices() {
    std::vector<std::string> devices;
    try {
        VkInstance tempInstance = VK_NULL_HANDLE;
        {
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = "GPENCL Probe";
            appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            appInfo.apiVersion = VK_API_VERSION_1_0;

            VkInstanceCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            createInfo.pApplicationInfo = &appInfo;

            if (vkCreateInstance(&createInfo, nullptr, &tempInstance) != VK_SUCCESS)
                return devices;
        }

        uint32_t devCount = 0;
        vkEnumeratePhysicalDevices(tempInstance, &devCount, nullptr);
        std::vector<VkPhysicalDevice> physDevices(devCount);
        vkEnumeratePhysicalDevices(tempInstance, &devCount, physDevices.data());

        for (auto dev : physDevices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(dev, &props);
            devices.push_back(props.deviceName);
        }

        vkDestroyInstance(tempInstance, nullptr);
    } catch (...) {
    }
    return devices;
}

} // namespace GpEncl
