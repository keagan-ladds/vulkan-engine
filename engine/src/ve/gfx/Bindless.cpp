#include "ve/gfx/Bindless.hpp"

#include "ve/gfx/Device.hpp"

namespace ve {

void Bindless::init(Device& device)
{
    m_device = &device;
    VkDevice dev = device.handle();

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = kMaxStorageImages;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = kMaxTextures;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;

    // PARTIALLY_BOUND: slots we never wrote are fine as long as shaders don't read them.
    // UPDATE_AFTER_BIND (+UNUSED_WHILE_PENDING): we may write new slots while
    // command buffers that use this set are still executing on the GPU.
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags bindingFlags[2] = {flags, flags};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 2;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_layout));

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxStorageImages},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_pool));

    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = m_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &m_layout;
    VK_CHECK(vkAllocateDescriptorSets(dev, &alloc, &m_set));
}

void Bindless::shutdown()
{
    if (!m_device) return;
    vkDestroyDescriptorPool(m_device->handle(), m_pool, nullptr);
    vkDestroyDescriptorSetLayout(m_device->handle(), m_layout, nullptr);
    m_device = nullptr;
}

void Bindless::setFallback(VkImageView view, VkSampler sampler)
{
    m_fallbackView = view;
    m_fallbackSampler = sampler;
}

void Bindless::writeTexture(uint32_t index, VkImageView view, VkSampler sampler)
{
    if (!view) return;
    VkDescriptorImageInfo img{sampler, view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device->handle(), 1, &write, 0, nullptr);
}

void Bindless::writeStorage(uint32_t index, VkImageView view)
{
    VkDescriptorImageInfo img{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_set;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(m_device->handle(), 1, &write, 0, nullptr);
}

uint32_t Bindless::addTexture(VkImageView view, VkSampler sampler)
{
    uint32_t index;
    if (!m_freeTextures.empty()) {
        index = m_freeTextures.back();
        m_freeTextures.pop_back();
    } else {
        if (m_nextTexture >= kMaxTextures) throw std::runtime_error("Bindless texture table is full");
        index = m_nextTexture++;
    }
    writeTexture(index, view, sampler);
    return index;
}

void Bindless::updateTexture(uint32_t index, VkImageView view, VkSampler sampler)
{
    writeTexture(index, view, sampler);
}

void Bindless::removeTexture(uint32_t index)
{
    if (index == kInvalid || !m_device) return;
    // The GPU may still be reading this slot for in-flight frames: recycle later.
    m_device->deferDestroy([this, index] {
        if (m_fallbackView) writeTexture(index, m_fallbackView, m_fallbackSampler);
        m_freeTextures.push_back(index);
    });
}

uint32_t Bindless::addStorageImage(VkImageView view)
{
    uint32_t index;
    if (!m_freeStorage.empty()) {
        index = m_freeStorage.back();
        m_freeStorage.pop_back();
    } else {
        if (m_nextStorage >= kMaxStorageImages) throw std::runtime_error("Bindless storage table is full");
        index = m_nextStorage++;
    }
    writeStorage(index, view);
    return index;
}

void Bindless::updateStorageImage(uint32_t index, VkImageView view)
{
    writeStorage(index, view);
}

void Bindless::removeStorageImage(uint32_t index)
{
    if (index == kInvalid || !m_device) return;
    m_device->deferDestroy([this, index] { m_freeStorage.push_back(index); });
}

} // namespace ve
