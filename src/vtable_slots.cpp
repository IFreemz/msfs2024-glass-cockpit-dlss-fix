// Vtable indices of the two device methods hooked for pipeline creation (C interface layout).
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>

size_t graphics_pipeline_slot() { return offsetof(ID3D12DeviceVtbl, CreateGraphicsPipelineState) / sizeof(void*); }
size_t pipeline_stream_slot() { return offsetof(ID3D12Device2Vtbl, CreatePipelineState) / sizeof(void*); }
