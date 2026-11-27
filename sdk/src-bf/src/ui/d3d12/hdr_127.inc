// Included inside rex::ui::d3d12. The SDR game and UI layers retain the existing
// pipeline formats; all transfer conversion happens once, in this last pass.
bool D3D12Presenter::SetupHdr() {
  auto& p = paint_context_;
  auto* device = provider_.GetDevice();
  if (!p.hdr_root) {
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, sizeof(hdr::Constants) / 4};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = parameters;
    p.hdr_root.Attach(util::CreateRootSignature(provider_, root));
    if (!p.hdr_root) return false;
  }
  if (!p.hdr_pipeline) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = p.hdr_root.Get();
    desc.VS = {shaders::hdr_d3d12_vs, sizeof(shaders::hdr_d3d12_vs)};
    desc.PS = {shaders::hdr_d3d12_ps, sizeof(shaders::hdr_d3d12_ps)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&p.hdr_pipeline)))) return false;
  }
  if (p.hdr_game && p.hdr_game->GetDesc().Width == p.swap_chain_width &&
      p.hdr_game->GetDesc().Height == p.swap_chain_height) return true;
  p.hdr_game.Reset(); p.hdr_ui.Reset();
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = p.swap_chain_width;
  desc.Height = p.swap_chain_height;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.Format = kSwapChainFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  for (unsigned i = 0; i < 2; ++i) {
    auto& resource = i ? p.hdr_ui : p.hdr_game;
    if (FAILED(device->CreateCommittedResource(&util::kHeapPropertiesDefault,
        provider_.GetHeapFlagCreateNotZeroed(), &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&resource)))) return false;
    device->CreateRenderTargetView(resource.Get(), nullptr, provider_.OffsetRTVDescriptor(
        p.rtv_heap->GetCPUDescriptorHandleForHeapStart(), PaintContext::kRTVIndexHdrGame + i));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource.Get(), &srv, provider_.OffsetViewDescriptor(
        p.view_heap->GetCPUDescriptorHandleForHeapStart(), PaintContext::kViewIndexHdrGame + i));
  }
  return true;
}

void D3D12Presenter::DrawHdr(ID3D12GraphicsCommandList* cl, ID3D12Resource* output,
                            D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
  auto& p = paint_context_;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
  cl->ResourceBarrier(1, &barrier);
  cl->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, float(p.swap_chain_width), float(p.swap_chain_height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(p.swap_chain_width), LONG(p.swap_chain_height)};
  cl->RSSetViewports(1, &viewport); cl->RSSetScissorRects(1, &scissor);
  auto* heap = p.view_heap.Get();
  cl->SetDescriptorHeaps(1, &heap);
  cl->SetGraphicsRootSignature(p.hdr_root.Get());
  cl->SetPipelineState(p.hdr_pipeline.Get());
  cl->SetGraphicsRootDescriptorTable(0, provider_.OffsetViewDescriptor(
      heap->GetGPUDescriptorHandleForHeapStart(), PaintContext::kViewIndexHdrGame));
  const auto values = p.hdr.Values();
  cl->SetGraphicsRoot32BitConstants(1, sizeof(values) / 4, &values, 0);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cl->DrawInstanced(3, 1, 0, 0);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  cl->ResourceBarrier(1, &barrier);

  const char* capture = std::getenv("REX_DEV_HDR_CAPTURE");
  if (capture && *capture && !p.hdr_capture_done && ++p.hdr_capture_frames >= 1800) {
    p.hdr_capture_done = true;
    auto* device = provider_.GetDevice();
    auto desc = output->GetDesc();
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &p.hdr_capture_footprint, nullptr, nullptr, &size);
    D3D12_RESOURCE_DESC buffer;
    util::FillBufferResourceDesc(buffer, size, D3D12_RESOURCE_FLAG_NONE);
    if (SUCCEEDED(device->CreateCommittedResource(&util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
        &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.hdr_capture)))) {
      p.hdr_capture_path = capture;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1, &barrier);
      D3D12_TEXTURE_COPY_LOCATION source{};
      source.pResource = output; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION dest{};
      dest.pResource = p.hdr_capture.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dest.PlacedFootprint = p.hdr_capture_footprint;
      cl->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      cl->ResourceBarrier(1, &barrier);
    } else REXLOG_WARN("HDR: developer FP16 capture allocation failed");
  }
}

void D3D12Presenter::FlushHdrCapture() {
  auto& p = paint_context_;
  if (!p.hdr_capture) return;
  p.paint_submission_tracker.AwaitAllSubmissionsCompletion();
  const auto& fp = p.hdr_capture_footprint;
  D3D12_RANGE read{size_t(fp.Offset), size_t(fp.Offset) + size_t(fp.Footprint.RowPitch) * fp.Footprint.Height};
  void* data = nullptr;
  if (SUCCEEDED(p.hdr_capture->Map(0, &read, &data))) {
    std::ofstream file(p.hdr_capture_path, std::ios::binary);
    uint32_t width = fp.Footprint.Width, height = fp.Footprint.Height;
    file.write(reinterpret_cast<const char*>(&width), sizeof(width));
    file.write(reinterpret_cast<const char*>(&height), sizeof(height));
    for (uint32_t y = 0; y < height; ++y)
      file.write(static_cast<const char*>(data) + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(width) * 8);
    D3D12_RANGE written{0, 0};
    p.hdr_capture->Unmap(0, &written);
    REXLOG_INFO("HDR: captured actual scRGB FP16 swapchain {}x{}, success {}, {}",
        width, height, bool(file), p.hdr_capture_path);
  }
  p.hdr_capture.Reset();
  p.hdr_capture_path.clear();
}
