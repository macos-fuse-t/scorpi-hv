# D3D12 command transport design guidelines

Date: 2026-05-19

This document records the current design direction for Scorpi's future Windows
DirectX 12 guest driver and host renderer. It is intentionally written as
guidelines rather than a frozen protocol specification.

The accepted direction is:

- virtualize D3D12 semantics, not Vulkan;
- use virtio-gpu as the transport/device foundation;
- send normalized D3D12-style command buffers from the Windows guest;
- make the host responsible for translating those commands to Metal first and
  Vulkan second;
- treat shader translation as the highest-risk technical area.

Related research note:

- `docs/windows_dx12_virtio_gpu_research.md`

## Core architecture

The target architecture is:

```text
Windows application
  -> D3D12 runtime
  -> Scorpi D3D12 UMD
  -> Scorpi D3D12 semantic command stream
  -> Scorpi Windows KMD / virtio-gpu transport
  -> scorpi-gpu-renderer direct virtqueue backend
       -> host command decoder and validator
       -> host D3D12 semantic frontend
       -> Metal backend
       -> optional Vulkan backend
```

The guest should not stream raw COM calls, raw Windows handles, guest pointers,
or runtime-private structures. The guest UMD should encode a stable Scorpi
command stream derived from the D3D12 UMD DDI. That stream should preserve
D3D12 semantics closely enough that Metal and Vulkan can share a common host
frontend.

The host should own:

- direct consumption of renderer-owned virtio-gpu queues;
- command stream decoding;
- command validation;
- object lifetime tracking;
- resource state validation;
- descriptor/root-signature interpretation;
- shader translation and pipeline caching;
- backend lowering to Metal and Vulkan;
- error reporting and device-loss behavior.

## Transport model

Use virtqueues for control, submission, and notifications. Use shared
guest/host-visible buffers for large command streams and payloads.

`scorpi-gpu-renderer`, not `scorpi-hv`, should consume accelerated virtio-gpu
queues. `scorpi-hv` exposes the virtio PCI device, negotiates features, sets up
queue and memory metadata, reports kicks, and injects interrupts. It must not
parse, copy, or forward D3D12 submit payloads on the hot path.

This should be implemented through Scorpi's generic vhost virtio backend
framework. Virtio-gpu is the first consumer, but the setup/control layer should
also be reusable by future vhost virtio devices such as virtio-fs.

Do not put every D3D12 command directly in virtqueue descriptors. Virtqueues
should act as doorbells and lifecycle/control channels. Render command streams
should live in shared command-buffer resources.

Recommended logical channels:

```text
control queue
  query caps
  create/destroy context
  create/destroy resource
  map/unmap resource
  create/destroy fence
  create/destroy shader/pipeline/root signature
  report synchronous control errors

submit queue
  submit command buffer resource + offset + size
  identify context and queue type
  attach waits/signals
  reference fence ids

event/completion path
  fence completion
  async command errors
  device lost/reset
  display and presentation events
```

The implementation can initially multiplex logical channels over existing
virtio-gpu control infrastructure, but the queue ownership must stay clear:
`scorpi-hv` sets up transport, `scorpi-gpu-renderer` reads and completes the
virtqueue work.

## Control protocol vs render protocol

Keep control protocol and render command protocol separate.

Control protocol characteristics:

- request/response;
- small messages;
- object lifecycle;
- capability queries;
- memory mapping;
- explicit error status.

Render command protocol characteristics:

- append-only command buffers;
- no request/response inside the stream;
- validation at submit time;
- completion only through fences;
- backend execution can be asynchronous.

This separation keeps render submission fast and makes error handling easier to
reason about.

## Object identity

Use Scorpi object IDs everywhere.

Do not expose or trust:

- guest virtual addresses;
- guest kernel pointers;
- Windows handles;
- COM object addresses;
- backend-native Metal/Vulkan handles.

Every guest-visible object should have a compact typed ID:

```text
context_id
queue_id
resource_id
heap_id
descriptor_heap_id
root_signature_id
shader_id
pso_id
fence_id
query_heap_id
```

IDs should be scoped where useful. For example, command queue fence values are
usually queue-local or context-local, while resource IDs may be adapter-wide.
The protocol spec should make those scopes explicit.

## Packet format guidelines

All command and control packets should be:

- versioned;
- length-delimited;
- little-endian;
- fixed-width where practical;
- aligned to a documented boundary;
- forward-compatible through size/version fields;
- validated before execution.

Recommended packet header shape:

```c
struct scorpi_d3d12_packet_header {
	uint16_t opcode;
	uint16_t header_size;
	uint32_t total_size;
	uint32_t protocol_version;
	uint32_t flags;
};
```

The exact fields can change, but every packet needs enough metadata to skip,
reject, or safely parse unknown/future packet versions.

Avoid variable-length inline data except for small bounded arrays. Large data
should live in blob/shared resources and be referenced by object ID, offset,
size, and hash when appropriate.

## Command buffers

Render command buffers should be replayable and independently validatable.

A submit record should identify:

```text
context_id
queue_type
command_buffer_resource_id
offset
size
wait fence list
signal fence id/value
submission flags
```

The command buffer should not depend on transient guest memory outside the
resources it explicitly references.

Queue types should exist in the protocol from the beginning:

```text
GRAPHICS
COMPUTE
COPY
PRESENT
```

The first implementation may map these to one Metal queue internally. Keeping
the queue type in the protocol avoids redesigning the guest ABI later.

## D3D12 semantic command stream

The stream should be close to D3D12, but normalized.

Preserve:

- command queues and command lists;
- root signatures;
- descriptor heaps and descriptor tables;
- pipeline state objects;
- resource barriers;
- draw, dispatch, copy, clear, resolve operations;
- query operations;
- fences;
- presentation.

Normalize:

- guest handles into Scorpi object IDs;
- descriptors into validated descriptor records;
- root arguments into backend-neutral root argument packets;
- resource barriers into canonical transition/UAV/aliasing records;
- pipeline state into immutable PSO descriptors;
- shader bytecode into content-addressed shader objects;
- CPU-visible memory into explicit mapped resource ranges.

Do not encode guest-side implementation details that are not meaningful to
Metal or Vulkan.

## Resource and memory model

Prefer standard virtio-gpu blob-resource concepts for large resources, even for
the custom D3D12 command protocol.

Required concepts:

- resource creation/destruction;
- heap creation/destruction;
- committed and placed resources;
- upload/readback/default-style memory classes;
- host-visible memory window where needed;
- map/unmap ranges;
- resource UUID/export path if needed;
- displayable resources for presentation;
- resource format, dimensions, mip levels, array layers, sample count, usage;
- strict bounds checks for offset, size, stride, row pitch, and slice pitch.

Metal-first memory notes:

- Apple Silicon unified memory helps performance but does not remove WDDM
  residency and budgeting requirements.
- Displayable resources should likely be backed by IOSurface or a host object
  that can be presented by Scorpi Viewer.
- Upload/readback resources should be modeled separately from displayable or
  render-target resources, even if the host allocation strategy later merges
  them.

## Synchronization

Use explicit fence objects and monotonic fence values.

Recommended operations:

```text
CREATE_FENCE
DESTROY_FENCE
SUBMIT(wait_fences..., signal_fence)
WAIT_FENCE
QUERY_FENCE
SIGNAL_FENCE_FROM_HOST
```

Backend mapping:

- Metal: `MTLSharedEvent`, command-buffer completion handlers, or CPU-tracked
  completion for early prototypes.
- Vulkan: timeline semaphores where available, otherwise fences/semaphores with
  compatibility handling.

The host should signal completions asynchronously through the event/completion
path and interrupt the guest when required.

## Shader and pipeline handling

Shader translation is the riskiest part of the project and should be treated as
a first-class protocol concern.

Do not bury shader bytecode inside draw or pipeline commands. Use explicit
shader and pipeline objects.

Recommended control objects:

```text
CREATE_ROOT_SIGNATURE
CREATE_SHADER
CREATE_PIPELINE_STATE
DESTROY_ROOT_SIGNATURE
DESTROY_SHADER
DESTROY_PIPELINE_STATE
```

Recommended shader creation fields:

```text
shader_id
stage
dxil_blob_resource_id
dxil_offset
dxil_size
dxil_hash
entrypoint
shader_model
flags
metadata/reflection payload
```

Recommended PSO creation fields:

```text
pso_id
root_signature_id
shader_ids
render target formats
depth/stencil format
blend/raster/depth state
primitive topology
sample count
cached pipeline hash
```

Metal-first shader strategy:

- Use DXIL as the primary guest shader payload.
- Evaluate Apple Metal Shader Converter as the primary DXIL-to-Metal path.
- Cache translated shader and pipeline artifacts by content hash.
- Use reflection to map D3D12 root signatures and descriptors to Metal
  argument buffers/resources.
- Report only the D3D12 feature options that the translation path can support.

Vulkan backend shader strategy:

- Evaluate DXIL-to-SPIR-V paths separately.
- Use vkd3d-proton/Wine vkd3d as references for semantic lowering, not as a
  drop-in WDDM driver.
- Keep shader object IDs and pipeline object IDs shared with the common
  frontend.

## Descriptor and binding model

D3D12 descriptors and root signatures are central to correctness.

The protocol should represent:

- descriptor heaps;
- descriptor heap type;
- descriptor records;
- CPU descriptor writes;
- shader-visible descriptor tables;
- root constants;
- root CBV/SRV/UAV descriptors;
- static samplers;
- resource binding flags;
- descriptor ranges and register spaces.

The host frontend should validate descriptor records before use and translate
them to backend binding models.

For Metal, expect substantial custom work:

- map descriptor tables to argument buffers or explicit resource binding;
- handle register spaces;
- handle dynamic offsets and root constants;
- handle sampler state differences;
- handle bindless/resource-array limits honestly.

## Presentation

Presentation should be modeled explicitly rather than hidden in a flush.

Recommended operations:

```text
CREATE_SWAPCHAIN_OR_PRESENT_TARGET
PRESENT_RESOURCE
SET_SCANOUT_RESOURCE
ACQUIRE_PRESENT_RESOURCE
RELEASE_PRESENT_RESOURCE
```

Early implementation can be simpler, but the protocol should distinguish:

- renderable resources;
- displayable resources;
- host viewer scanout resources;
- synchronization required before present.

For Metal, IOSurface-backed resources are the likely long-term path for
displayable surfaces.

## Error handling and device loss

Every control command should return a status. Render command buffers should
report asynchronous failures through the event path.

Error classes to define early:

```text
OK
INVALID_OPCODE
INVALID_SIZE
INVALID_OBJECT
INVALID_STATE
INVALID_RESOURCE_RANGE
UNSUPPORTED_FORMAT
UNSUPPORTED_FEATURE
OUT_OF_MEMORY
SHADER_TRANSLATION_FAILED
PIPELINE_CREATION_FAILED
DEVICE_LOST
PROTOCOL_VIOLATION
```

The host should be able to reject a command buffer without crashing the VM or
host process.

Device loss must have a defined guest-visible behavior because WDDM TDR/reset
will rely on it.

## Security rules

Treat all guest input as hostile.

Required checks:

- packet size and alignment;
- object type and lifetime;
- resource bounds;
- integer overflow;
- descriptor bounds;
- command-buffer offset and size;
- shader blob size and hash;
- format/usage compatibility;
- queue/context ownership;
- fence ownership;
- maximum counts for descriptors, resources, packets, and submissions.

Do not pass guest-provided data directly into Metal/Vulkan without validation
and translation.

Consider process isolation for the renderer once basic functionality works.
QEMU's vhost-user graphics split is a useful architectural reference, even if
Scorpi does not adopt that exact model.

## Capability negotiation

The protocol should have explicit capability discovery.

Capabilities should include:

- protocol version;
- backend type: Metal or Vulkan;
- supported queue types;
- maximum command buffer size;
- maximum resource dimensions;
- supported formats/usages;
- shader model support;
- root signature version support;
- descriptor heap limits;
- bindless/resource-array limits;
- supported barrier types;
- supported query types;
- presentation capabilities;
- debugging/tracing flags.

The Windows driver must report D3D12 feature levels and options based on these
capabilities, not aspirational backend goals.

## Debugging and tracing

Build tracing into the protocol from the start.

Useful debug features:

- command stream dump tool;
- packet validator executable;
- host-side replay of captured command buffers;
- shader translation cache inspection;
- object lifetime logging;
- fence timeline logging;
- backend debug labels for Metal/Vulkan objects;
- environment flags to force synchronous execution;
- protocol version and capability dump.

The ability to replay a captured command buffer outside the VM will be
extremely valuable for shader and backend bugs.

## Initial milestones

Suggested implementation order:

1. Define protocol skeleton:
   - caps query;
   - object IDs;
   - packet header;
   - submit record;
   - fence object.
2. Implement host-only Metal renderer spike:
   - one shader;
   - one PSO;
   - one triangle;
   - one present path.
3. Implement shared command buffer transport:
   - guest/test producer writes packets;
   - renderer reads virtqueue work directly;
   - renderer validates and decodes;
   - fence completion works.
4. Add resource creation and upload path.
5. Add shader object and PSO object creation.
6. Add root signature and descriptor model.
7. Run a minimal Windows guest producer path.
8. Expand toward WDDM KMD submission.
9. Implement D3D12 UMD prototype.
10. Add Vulkan backend after the Metal path proves the frontend model.

## Open questions

- Should Scorpi define a vendor virtio-gpu capset for D3D12 semantics, or a
  separate virtio device layered next to virtio-gpu?
- How much command validation belongs in the KMD versus host?
- Does the first Windows prototype use a private test DLL, Vulkan-like ICD, or
  early D3D12 UMD?
- Can Apple Metal Shader Converter be redistributed and used at runtime in
  Scorpi's intended packaging model?
- What is the first supported D3D12 feature level?
- What root signature version is the first target?
- What descriptor heap limits can be honestly supported on Metal?
- Should the renderer run in-process initially and move out-of-process later,
  or start isolated from day one?
- What is the minimum macOS version and Apple GPU family target?
