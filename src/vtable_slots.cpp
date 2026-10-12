// Vtable indices of the device methods hooked for pipeline creation and descriptor copies (C interface layout).
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>

size_t graphics_pipeline_slot() { return offsetof(ID3D12DeviceVtbl, CreateGraphicsPipelineState) / sizeof(void*); }
size_t pipeline_stream_slot() { return offsetof(ID3D12Device2Vtbl, CreatePipelineState) / sizeof(void*); }
size_t copy_descriptors_slot() { return offsetof(ID3D12DeviceVtbl, CopyDescriptors) / sizeof(void*); }
size_t copy_descriptors_simple_slot() { return offsetof(ID3D12DeviceVtbl, CopyDescriptorsSimple) / sizeof(void*); }
size_t create_descriptor_heap_slot() { return offsetof(ID3D12DeviceVtbl, CreateDescriptorHeap) / sizeof(void*); }
