// Included inside rex::ui::vulkan. All objects are connection-owned and drained
// before resize, display migration, HDR toggle, or shutdown.
void VulkanPresenter::PaintContext::DestroyHdr() {
  const auto& f = vulkan_device->functions();
  auto d = vulkan_device->device();
  if (hdr_pipeline) f.vkDestroyPipeline(d, hdr_pipeline, nullptr);
  if (hdr_layout) f.vkDestroyPipelineLayout(d, hdr_layout, nullptr);
  if (hdr_pool) f.vkDestroyDescriptorPool(d, hdr_pool, nullptr);
  if (hdr_set_layout) f.vkDestroyDescriptorSetLayout(d, hdr_set_layout, nullptr);
  if (hdr_game_fb) f.vkDestroyFramebuffer(d, hdr_game_fb, nullptr);
  if (hdr_ui_fb) f.vkDestroyFramebuffer(d, hdr_ui_fb, nullptr);
  if (hdr_layer_pass) f.vkDestroyRenderPass(d, hdr_layer_pass, nullptr);
  hdr_game.reset(); hdr_ui.reset();
  hdr_pipeline = VK_NULL_HANDLE; hdr_layout = VK_NULL_HANDLE;
  hdr_pool = VK_NULL_HANDLE; hdr_set_layout = VK_NULL_HANDLE; hdr_set = VK_NULL_HANDLE;
  hdr_game_fb = hdr_ui_fb = VK_NULL_HANDLE; hdr_layer_pass = VK_NULL_HANDLE;
}

bool VulkanPresenter::SetupHdr() {
  auto& p = paint_context_;
  const auto& f = vulkan_device_->functions();
  auto d = vulkan_device_->device();
  p.hdr_game = GuestOutputImage::Create(vulkan_device_, p.swapchain_extent.width,
      p.swapchain_extent.height, VK_FORMAT_B8G8R8A8_UNORM);
  p.hdr_ui = GuestOutputImage::Create(vulkan_device_, p.swapchain_extent.width,
      p.swapchain_extent.height, VK_FORMAT_B8G8R8A8_UNORM);
  if (!p.hdr_game || !p.hdr_ui) return false;
  VkAttachmentDescription attachment{};
  attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
  VkSubpassDependency deps[2]{};
  deps[0] = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
  deps[1] = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, 0};
  VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass.attachmentCount = 1; pass.pAttachments = &attachment;
  pass.subpassCount = 1; pass.pSubpasses = &subpass;
  pass.dependencyCount = 2; pass.pDependencies = deps;
  if (f.vkCreateRenderPass(d, &pass, nullptr, &p.hdr_layer_pass) != VK_SUCCESS) return false;
  for (unsigned i = 0; i < 2; ++i) {
    auto view = (i ? p.hdr_ui : p.hdr_game)->view();
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = p.hdr_layer_pass; fb.attachmentCount = 1; fb.pAttachments = &view;
    fb.width = p.swapchain_extent.width; fb.height = p.swapchain_extent.height; fb.layers = 1;
    if (f.vkCreateFramebuffer(d, &fb, nullptr, i ? &p.hdr_ui_fb : &p.hdr_game_fb) != VK_SUCCESS) return false;
  }
  VkDescriptorSetLayoutBinding bindings[2]{};
  for (unsigned i = 0; i < 2; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
      VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout.bindingCount = 2; set_layout.pBindings = bindings;
  if (f.vkCreateDescriptorSetLayout(d, &set_layout, nullptr, &p.hdr_set_layout) != VK_SUCCESS) return false;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
  if (f.vkCreateDescriptorPool(d, &pool, nullptr, &p.hdr_pool) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = p.hdr_pool; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &p.hdr_set_layout;
  if (f.vkAllocateDescriptorSets(d, &alloc, &p.hdr_set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo images[2]{};
  VkWriteDescriptorSet writes[2]{};
  for (unsigned i = 0; i < 2; ++i) {
    images[i].imageView = (i ? p.hdr_ui : p.hdr_game)->view();
    images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = p.hdr_set; writes[i].dstBinding = i;
    writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  f.vkUpdateDescriptorSets(d, 2, writes, 0, nullptr);
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(hdr::Constants)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1; layout.pSetLayouts = &p.hdr_set_layout;
  layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
  if (f.vkCreatePipelineLayout(d, &layout, nullptr, &p.hdr_layout) != VK_SUCCESS) return false;
  const uint32_t vs_code[] = {
#include "ui/shaders/bytecode/hdr_vulkan_vs.inc"
  };
  const uint32_t fs_code[] = {
#include "ui/shaders/bytecode/hdr_vulkan_ps.inc"
  };
  VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module.codeSize = sizeof(vs_code); module.pCode = vs_code;
  if (f.vkCreateShaderModule(d, &module, nullptr, &vs) != VK_SUCCESS) return false;
  module.codeSize = sizeof(fs_code); module.pCode = fs_code;
  if (f.vkCreateShaderModule(d, &module, nullptr, &fs) == VK_SUCCESS)
    p.hdr_pipeline = CreateGuestOutputPaintPipeline(GuestOutputPaintEffect::kBilinear,
        p.swapchain_render_pass, vs, fs);
  if (fs) f.vkDestroyShaderModule(d, fs, nullptr);
  f.vkDestroyShaderModule(d, vs, nullptr);
  return p.hdr_pipeline != VK_NULL_HANDLE;
}

void VulkanPresenter::DrawHdr(VkCommandBuffer cl, VkRenderPassBeginInfo real_pass) {
  auto& p = paint_context_;
  const auto& f = vulkan_device_->functions();
  VkClearValue black{};
  black.color.float32[3] = 1;
  real_pass.clearValueCount = 1; real_pass.pClearValues = &black;
  f.vkCmdBeginRenderPass(cl, &real_pass, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, 0, float(p.swapchain_extent.width), float(p.swapchain_extent.height), 0, 1};
  VkRect2D scissor{{0, 0}, p.swapchain_extent};
  f.vkCmdSetViewport(cl, 0, 1, &viewport); f.vkCmdSetScissor(cl, 0, 1, &scissor);
  f.vkCmdBindPipeline(cl, VK_PIPELINE_BIND_POINT_GRAPHICS, p.hdr_pipeline);
  f.vkCmdBindDescriptorSets(cl, VK_PIPELINE_BIND_POINT_GRAPHICS, p.hdr_layout, 0, 1, &p.hdr_set, 0, nullptr);
  const auto values = p.hdr.Values();
  f.vkCmdPushConstants(cl, p.hdr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(values), &values);
  f.vkCmdDraw(cl, 3, 1, 0, 0);
  f.vkCmdEndRenderPass(cl);
}
