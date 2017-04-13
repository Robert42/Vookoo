
#include <vku/vku_framework.hpp>
#include <vku/vku.hpp>
#include <glm/glm.hpp>
#include <glm/gtx/io.hpp>

#include <gilgamesh/mesh.hpp>
#include <gilgamesh/scene.hpp>
#include <gilgamesh/shapes/teapot.hpp>
#include <gilgamesh/decoders/fbx_decoder.hpp>
#include <gilgamesh/encoders/fbx_encoder.hpp>

int main() {
  glfwInit();

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  const char *title = "teapot";
  auto glfwwindow = glfwCreateWindow(800, 600, title, nullptr, nullptr);

  vku::Framework fw{title};
  if (!fw.ok()) {
    std::cout << "Framework creation failed" << std::endl;
    exit(1);
  }

  vk::Device device = fw.device();

  vku::Window window{fw.instance(), fw.device(), fw.physicalDevice(), fw.graphicsQueueFamilyIndex(), glfwwindow};
  if (!window.ok()) {
    std::cout << "Window creation failed" << std::endl;
    exit(1);
  }

  ////////////////////////////////////////
  //
  // Build the descriptor sets

  vku::DescriptorSetLayoutMaker dslm{};
  dslm.buffer(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment, 1);
  dslm.image(1, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment, 1);
  dslm.image(2, vk::DescriptorType::eCombinedImageSampler, vk::ShaderStageFlagBits::eFragment, 1);
  auto layout = dslm.createUnique(device);

  vku::DescriptorSetMaker dsm{};
  dsm.layout(*layout);
  auto descriptorSets = dsm.create(device, fw.descriptorPool());

  vku::PipelineLayoutMaker plm{};
  plm.descriptorSetLayout(*layout);
  auto pipelineLayout = plm.createUnique(device);

  gilgamesh::simple_mesh mesh;
  gilgamesh::teapot shape;
  shape.build(mesh);
  mesh.reindex(true);

  struct Vertex { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv; };
  std::vector<Vertex> vertices;

  auto meshpos = mesh.pos();
  auto meshnormal = mesh.normal();
  auto meshuv = mesh.uv(0);
  for (size_t i = 0; i != meshpos.size(); ++i) {
    glm::vec3 pos = meshpos[i];
    glm::vec3 normal = meshnormal[i];
    glm::vec2 uv = meshuv[i];
    vertices.emplace_back(Vertex{pos, normal, uv});
  }
  std::vector<uint32_t> indices = mesh.indices32();

  vku::VertexBuffer vbo(fw.device(), fw.memprops(), vertices);
  vku::IndexBuffer ibo(fw.device(), fw.memprops(), indices);
  uint32_t indexCount = (uint32_t)indices.size();

  struct Uniform {
    glm::mat4 modelToPerspective;
    glm::mat4 modelToWorld;
    glm::mat4 normalToWorld;
    glm::mat4 modelToLight;
    glm::vec4 cameraPos;
  };

  // World matrices of model, camera and light
  glm::mat4 modelToWorld = glm::rotate(glm::mat4{}, glm::radians(-90.0f), glm::vec3(1, 0, 0));
  glm::mat4 cameraToWorld = glm::translate(glm::mat4{}, glm::vec3(0, 2, 8));
  glm::mat4 lightToWorld = glm::translate(glm::mat4{}, glm::vec3(8, 6, 0));
  lightToWorld = glm::rotate(lightToWorld, glm::radians(90.0f), glm::vec3(0, 1, 0));
  lightToWorld = glm::rotate(lightToWorld, glm::radians(-30.0f), glm::vec3(1, 0, 0));
  //cameraToWorld = lightToWorld;

  glm::mat4 worldToCamera = glm::inverse(cameraToWorld);
  glm::mat4 worldToLight = glm::inverse(lightToWorld);

  glm::mat4 leftHandCorrection(
    1.0f,  0.0f, 0.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 0.0f,
    0.0f,  0.0f, 0.5f, 0.0f,
    0.0f,  0.0f, 0.5f, 1.0f);
  glm::mat4 cameraToPerspective = leftHandCorrection * glm::perspective(glm::radians(45.0f), (float)window.width()/window.height(), 1.0f, 100.0f);
  glm::mat4 lightToPerspective = leftHandCorrection * glm::perspective(glm::radians(45.0f), (float)256/256, 1.0f, 100.0f);

  // correct for "Y is down"
  //cameraToPerspective[1] = -cameraToPerspective[1];
  //lightToPerspective[1] = -lightToPerspective[1];
  
  // Create, but do not upload the uniform buffer as a device local buffer.
  vku::UniformBuffer ubo(fw.device(), fw.memprops(), sizeof(Uniform));

  ////////////////////////////////////////
  //
  // Build the final pipeline including enabling the depth test

  vku::ShaderModule final_vert{device, BINARY_DIR "teapot.vert.spv"};
  vku::ShaderModule final_frag{device, BINARY_DIR "teapot.frag.spv"};

  vku::PipelineMaker pm{window.width(), window.height()};
  pm.shader(vk::ShaderStageFlagBits::eVertex, final_vert);
  pm.shader(vk::ShaderStageFlagBits::eFragment, final_frag);
  pm.vertexBinding(0, (uint32_t)sizeof(Vertex));
  pm.vertexAttribute(0, 0, vk::Format::eR32G32B32Sfloat, (uint32_t)offsetof(Vertex, pos));
  pm.vertexAttribute(1, 0, vk::Format::eR32G32B32Sfloat, (uint32_t)offsetof(Vertex, normal));
  pm.vertexAttribute(2, 0, vk::Format::eR32G32Sfloat, (uint32_t)offsetof(Vertex, uv));
  pm.depthTestEnable(VK_TRUE);
  pm.cullMode(vk::CullModeFlagBits::eBack);
  pm.frontFace(vk::FrontFace::eCounterClockwise);

  auto renderPass = window.renderPass();
  auto &cache = fw.pipelineCache();
  auto finalPipeline = pm.createUnique(device, cache, *pipelineLayout, renderPass);

  ////////////////////////////////////////
  //
  // Build a pipeline for shadows
  uint32_t shadow_size = 256;

  vku::ShaderModule shadow_vert{device, BINARY_DIR "teapot.shadow.vert.spv"};
  vku::ShaderModule shadow_frag{device, BINARY_DIR "teapot.shadow.frag.spv"};

  vku::PipelineMaker spm{shadow_size, shadow_size};
  spm.shader(vk::ShaderStageFlagBits::eVertex, shadow_vert);
  spm.shader(vk::ShaderStageFlagBits::eFragment, shadow_frag);
  spm.vertexBinding(0, (uint32_t)sizeof(Vertex));
  spm.vertexAttribute(0, 0, vk::Format::eR32G32B32Sfloat, (uint32_t)offsetof(Vertex, pos));
  spm.vertexAttribute(1, 0, vk::Format::eR32G32B32Sfloat, (uint32_t)offsetof(Vertex, normal));
  spm.vertexAttribute(2, 0, vk::Format::eR32G32Sfloat, (uint32_t)offsetof(Vertex, uv));
  spm.depthTestEnable(VK_TRUE);
  pm.cullMode(vk::CullModeFlagBits::eBack);
  pm.frontFace(vk::FrontFace::eCounterClockwise);

  vku::DepthStencilImage shadowImage(device, fw.memprops(), shadow_size, shadow_size);

  // Build the renderpass using only depth/stencil.
  vku::RenderpassMaker rpm;

  // The depth/stencil attachment.
  rpm.attachmentBegin(shadowImage.format());
  rpm.attachmentLoadOp(vk::AttachmentLoadOp::eClear);
  rpm.attachmentStoreOp(vk::AttachmentStoreOp::eStore);
  rpm.attachmentFinalLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

  // A subpass to render using the above attachment.
  rpm.subpassBegin(vk::PipelineBindPoint::eGraphics);
  rpm.subpassDepthStencilAttachment(vk::ImageLayout::eDepthStencilAttachmentOptimal, 0);

  // A dependency to reset the layout of both attachments.
  rpm.dependencyBegin(VK_SUBPASS_EXTERNAL, 0);
  rpm.dependencySrcStageMask(vk::PipelineStageFlagBits::eEarlyFragmentTests);
  rpm.dependencyDstStageMask(vk::PipelineStageFlagBits::eLateFragmentTests);

  // Use the maker object to construct the vulkan object
  vk::UniqueRenderPass shadowRenderPass = rpm.createUnique(device);

  vk::ImageView attachments[1] = {shadowImage.imageView()};
  vk::FramebufferCreateInfo fbci{{}, *shadowRenderPass, 1, attachments, shadow_size, shadow_size, 1 };
  vk::UniqueFramebuffer shadowFrameBuffer = device.createFramebufferUnique(fbci);

  auto shadowPipeline = spm.createUnique(device, cache, *pipelineLayout, *shadowRenderPass, false);

  vk::ClearDepthStencilValue clearDepthValue{ 1.0f, 0 };
  std::array<vk::ClearValue, 1> clearColours{clearDepthValue};

  vk::RenderPassBeginInfo shadowRpbi{};
  shadowRpbi.renderPass = *shadowRenderPass;
  shadowRpbi.framebuffer = *shadowFrameBuffer;
  shadowRpbi.renderArea = vk::Rect2D{{0, 0}, {256, 256}};
  shadowRpbi.clearValueCount = (uint32_t)clearColours.size();
  shadowRpbi.pClearValues = clearColours.data();

  ////////////////////////////////////////
  //
  // Create a cubemap

  auto cubeBytes = vku::loadFile(SOURCE_DIR "okretnica.ktx");
  vku::KTXFileLayout ktx(cubeBytes.data(), cubeBytes.data()+cubeBytes.size());
  if (!ktx.ok()) {
    std::cout << "Could not load KTX file" << std::endl;
    exit(1);
  }

  vku::TextureImageCube cubeMap{device, fw.memprops(), ktx.width(0), ktx.height(0), ktx.mipLevels(), vk::Format::eR8G8B8A8Unorm};

  vku::GenericBuffer stagingBuffer(device, fw.memprops(), vk::BufferUsageFlagBits::eTransferSrc, cubeBytes.size());
  stagingBuffer.update(device, (const void*)cubeBytes.data(), cubeBytes.size());
  cubeBytes = std::vector<uint8_t>{};

  // Copy the staging buffer to the GPU texture and set the layout.
  vku::executeImmediately(device, window.commandPool(), fw.graphicsQueue(), [&](vk::CommandBuffer cb) {
    vk::Buffer buf = stagingBuffer.buffer();
    //for (uint32_t mipLevel = 0; mipLevel != 1; ++mipLevel) {
    for (uint32_t mipLevel = 0; mipLevel != ktx.mipLevels(); ++mipLevel) {
      auto width = ktx.width(mipLevel); 
      auto height = ktx.height(mipLevel); 
      auto depth = ktx.depth(mipLevel); 
      for (uint32_t face = 0; face != ktx.faces(); ++face) {
        cubeMap.copy(cb, buf, mipLevel, face, width, height, depth, ktx.offset(mipLevel, 0, face));
      }
    }
    cubeMap.setLayout(cb, vk::ImageLayout::eShaderReadOnlyOptimal);
  });

  // Free the staging buffer.
  stagingBuffer = vku::GenericBuffer{};

  ////////////////////////////////////////
  //
  // Update the descriptor sets for the shader uniforms.

  vku::SamplerMaker sm{};
  sm.magFilter(vk::Filter::eLinear);
  sm.minFilter(vk::Filter::eLinear);
  sm.mipmapMode(vk::SamplerMipmapMode::eNearest);
  vk::UniqueSampler sampler = sm.createUnique(device);

  vku::DescriptorSetUpdater update;
  update.beginDescriptorSet(descriptorSets[0]);

  // Set initial uniform buffer value
  update.beginBuffers(0, 0, vk::DescriptorType::eUniformBuffer);
  update.buffer(ubo.buffer(), 0, sizeof(Uniform));

  // Set initial sampler value
  update.beginImages(1, 0, vk::DescriptorType::eCombinedImageSampler);
  update.image(*sampler, cubeMap.imageView(), vk::ImageLayout::eShaderReadOnlyOptimal);

  update.beginImages(2, 0, vk::DescriptorType::eCombinedImageSampler);
  update.image(*sampler, shadowImage.imageView(), vk::ImageLayout::eShaderReadOnlyOptimal);

  update.update(device);

  // Set the static render commands for the main renderpass.
  window.setStaticCommands(
    [&](vk::CommandBuffer cb, int imageIndex, vk::RenderPassBeginInfo &rpbi) {
      vk::CommandBufferBeginInfo bi{};
      cb.begin(bi);

      cb.beginRenderPass(shadowRpbi, vk::SubpassContents::eInline);
      cb.bindPipeline(vk::PipelineBindPoint::eGraphics, *shadowPipeline);
      cb.bindVertexBuffers(0, vbo.buffer(), vk::DeviceSize(0));
      cb.bindIndexBuffer(ibo.buffer(), vk::DeviceSize(0), vk::IndexType::eUint32);
      cb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, descriptorSets, nullptr);
      cb.drawIndexed(indexCount, 1, 0, 0, 0);
      cb.endRenderPass();

      cb.beginRenderPass(rpbi, vk::SubpassContents::eInline);
      cb.bindPipeline(vk::PipelineBindPoint::eGraphics, *finalPipeline);
      cb.bindVertexBuffers(0, vbo.buffer(), vk::DeviceSize(0));
      cb.bindIndexBuffer(ibo.buffer(), vk::DeviceSize(0), vk::IndexType::eUint32);
      cb.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, descriptorSets, nullptr);
      cb.drawIndexed(indexCount, 1, 0, 0, 0);
      cb.endRenderPass();
      cb.end();
    }
  );

  if (!window.ok()) {
    std::cout << "Window creation failed" << std::endl;
    exit(1);
  }

  while (!glfwWindowShouldClose(glfwwindow)) {
    glfwPollEvents();

    window.draw(fw.device(), fw.graphicsQueue(),
      [&](vk::CommandBuffer pscb, int imageIndex) {
        // Generate the uniform buffer inline in the command buffer.
        // This is good for small buffers only!
        Uniform uniform;
        modelToWorld = glm::rotate(modelToWorld, glm::radians(1.0f), glm::vec3(0, 0, 1));
        uniform.modelToPerspective = cameraToPerspective * worldToCamera * modelToWorld;
        uniform.normalToWorld = modelToWorld;
        uniform.modelToLight = lightToPerspective * worldToLight * modelToWorld;

        // Record the dynamic buffer.
        vk::CommandBufferBeginInfo bi{};
        pscb.begin(bi);
        pscb.updateBuffer(ubo.buffer(), 0, sizeof(Uniform), (void*)&uniform);
        pscb.end();
      }
    );

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  device.waitIdle();
  glfwDestroyWindow(glfwwindow);
  glfwTerminate();

  return 0;
}