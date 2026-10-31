// Vtable indices of the D3D12 and DXGI methods the fix hooks (C interface layout).
#define CINTERFACE
#include <d3d12.h>
#include <dxgi1_2.h>
#include <cstddef>

// Device: pipeline creation, descriptors, views, root signatures and command lists.
size_t graphics_pipeline_slot() { return offsetof(ID3D12DeviceVtbl, CreateGraphicsPipelineState) / sizeof(void*); }
size_t pipeline_stream_slot() { return offsetof(ID3D12Device2Vtbl, CreatePipelineState) / sizeof(void*); }
size_t copy_descriptors_slot() { return offsetof(ID3D12DeviceVtbl, CopyDescriptors) / sizeof(void*); }
size_t copy_descriptors_simple_slot() { return offsetof(ID3D12DeviceVtbl, CopyDescriptorsSimple) / sizeof(void*); }
size_t create_descriptor_heap_slot() { return offsetof(ID3D12DeviceVtbl, CreateDescriptorHeap) / sizeof(void*); }
size_t create_root_signature_slot() { return offsetof(ID3D12DeviceVtbl, CreateRootSignature) / sizeof(void*); }
size_t create_srv_slot() { return offsetof(ID3D12DeviceVtbl, CreateShaderResourceView) / sizeof(void*); }
size_t create_uav_slot() { return offsetof(ID3D12DeviceVtbl, CreateUnorderedAccessView) / sizeof(void*); }
size_t create_rtv_slot() { return offsetof(ID3D12DeviceVtbl, CreateRenderTargetView) / sizeof(void*); }
size_t create_command_list_slot() { return offsetof(ID3D12DeviceVtbl, CreateCommandList) / sizeof(void*); }
size_t create_command_list1_slot() { return offsetof(ID3D12Device4Vtbl, CreateCommandList1) / sizeof(void*); }
size_t create_command_signature_slot() { return offsetof(ID3D12DeviceVtbl, CreateCommandSignature) / sizeof(void*); }

// Command list: what is bound, and the draws and copies.
size_t list_reset_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, Reset) / sizeof(void*); }
size_t list_clear_state_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, ClearState) / sizeof(void*); }
size_t list_draw_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, DrawInstanced) / sizeof(void*); }
size_t list_draw_indexed_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced) / sizeof(void*); }
size_t list_copy_texture_region_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, CopyTextureRegion) / sizeof(void*); }
size_t list_set_pipeline_state_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, SetPipelineState) / sizeof(void*); }
size_t list_set_graphics_root_signature_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature) / sizeof(void*); }
size_t list_set_graphics_root_table_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable) / sizeof(void*); }
size_t list_set_render_targets_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) / sizeof(void*); }
size_t list_begin_render_pass_slot() { return offsetof(ID3D12GraphicsCommandList4Vtbl, BeginRenderPass) / sizeof(void*); }
size_t list_end_render_pass_slot() { return offsetof(ID3D12GraphicsCommandList4Vtbl, EndRenderPass) / sizeof(void*); }
size_t list_set_scissor_rects_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, RSSetScissorRects) / sizeof(void*); }
size_t list_execute_indirect_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, ExecuteIndirect) / sizeof(void*); }
size_t list_resource_barrier_slot() { return offsetof(ID3D12GraphicsCommandListVtbl, ResourceBarrier) / sizeof(void*); }

// Command queue: the lists the sim submits.
size_t queue_execute_slot() { return offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists) / sizeof(void*); }

// DXGI: swapchain creation and presents.
size_t factory_create_swapchain_slot() { return offsetof(IDXGIFactory2Vtbl, CreateSwapChain) / sizeof(void*); }
size_t factory_create_swapchain_hwnd_slot() { return offsetof(IDXGIFactory2Vtbl, CreateSwapChainForHwnd) / sizeof(void*); }
size_t factory_create_swapchain_core_window_slot() { return offsetof(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow) / sizeof(void*); }
size_t factory_create_swapchain_composition_slot() { return offsetof(IDXGIFactory2Vtbl, CreateSwapChainForComposition) / sizeof(void*); }
size_t swapchain_present_slot() { return offsetof(IDXGISwapChain1Vtbl, Present) / sizeof(void*); }
size_t swapchain_present1_slot() { return offsetof(IDXGISwapChain1Vtbl, Present1) / sizeof(void*); }
