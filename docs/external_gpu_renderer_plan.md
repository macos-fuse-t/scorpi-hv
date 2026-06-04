# External GPU Renderer Plan

Date: 2026-06-04

This plan separates GPU rendering from `scorpi-hv`. The hypervisor remains the
guest-visible virtio-gpu device and session broker, but Metal, Vulkan, shader
translation, command validation, pipeline caches, and presentation backends live
in a separate renderer process.

## Target Architecture

```text
Windows guest
  -> Scorpi Windows KMD/UMD
  -> virtio-gpu PCI device exposed by scorpi-hv
  -> scorpi-hv virtio PCI frontend and transport setup
  -> renderer-owned virtqueue backend
  -> scorpi-gpu-renderer process
       -> direct virtio-gpu queue reader
       -> D3D12 semantic frontend
       -> macOS Metal backend
       -> Linux Vulkan backend
  -> display/export requests over Scorpi CNC WebSocket
  -> events brokered by scorpi-hv
  -> ScorpiViewer
```

`scorpi-hv` must not link Metal, Vulkan, DXIL conversion, or backend renderer
code. It owns:

- PCI device configuration, feature negotiation, and virtqueue setup;
- guest memory export, reset/lifecycle, kick notification, and interrupt
  injection;
- CNC actions for renderer display publication;
- display-event brokering to ScorpiViewer;
- input and resize routing.

`scorpi-hv` must not parse, copy, or forward accelerated virtio-gpu command
payloads on the hot path. After transport setup, renderer-owned queues are
consumed by `scorpi-gpu-renderer` directly.

`scorpi-gpu-renderer` owns:

- virtqueue descriptor walking for renderer-owned virtio-gpu queues;
- accelerated virtio-gpu backend state;
- capsets, contexts, blob resources, command submission, and fences;
- D3D12 semantic packet decoding and validation;
- Metal and Vulkan backend implementations;
- renderer-owned displayable images;
- shader conversion and pipeline caches.

ScorpiViewer remains a display/input client. It should not know virtio-gpu,
D3D12, Metal, or Vulkan.

## Best-Practice References

The split follows the same broad separation used by mature virtual GPU stacks:

- QEMU `vhost-user`/`vhost-user-gpu`: a VMM-side stub communicates with an
  external device backend over a socket and shared memory.
- virglrenderer/Venus: virtio-gpu transport with explicit capsets, contexts,
  blob resources, host-visible memory, and command-stream validation.
- crosvm rutabaga/gfxstream: a renderer abstraction with selectable backends
  behind a virtio-gpu handling layer.

Scorpi should use these as architectural references, not as an immediate
requirement to implement the full standard `vhost-user-gpu` protocol.

Scorpi does not add a custom HV-renderer socket protocol. The renderer uses the
existing CNC WebSocket channel as another `scorpi-cnc` client. For MS1 this is
only used for display publication. In later milestones the same WebSocket
control surface is used for transport setup: renderer registration, queue
metadata, memory-region metadata, kick notifications, interrupt requests, and
reset/disconnect events.

The WebSocket control surface is not the GPU command path. DirectX/virtio-gpu
command payloads are read by `scorpi-gpu-renderer` from mapped guest memory and
virtqueue descriptors.

## Final External-Virtio Direction

The long-term architecture is a generic external virtio backend framework, not
a GPU-only one-off path.

```text
scorpi-hv
  -> virtio PCI frontend
  -> generic external virtio backend setup/control
       -> external virtio-gpu backend: scorpi-gpu-renderer
       -> external virtio-fs backend: future scorpi-fs-backend
       -> other external virtio backends later
```

The generic layer owns only shared virtio mechanics:

- backend registration;
- device instance identity;
- feature bits;
- guest memory region metadata;
- virtqueue metadata;
- queue kick notifications;
- interrupt requests;
- reset/disconnect;
- health/state.

Device-specific semantics stay out of that layer. GPU capsets, scanouts, blob
resources, `SUBMIT_3D`, D3D12, Metal, and Vulkan remain in
`scorpi-gpu-renderer`. Future virtio-fs semantics remain in a future filesystem
backend.

Generic CNC setup/control actions should use device-agnostic names:

- `virtio_backend_register`
- `virtio_device_describe`
- `virtio_queue_kick`
- `virtio_queue_interrupt`
- `virtio_device_reset`
- `virtio_backend_disconnect`

GPU display publication remains separate and GPU/display-specific:

- `renderer_set_scanout`
- `renderer_update_scanout`
- `renderer_unset_scanout`

## Milestones

### MS1: External Renderer Skeleton

Goal: prove process separation and ScorpiViewer display brokering.

Deliverable:

```text
scorpi-hv starts the existing CNC WebSocket server
  -> scorpi-gpu-renderer is started separately
  -> renderer connects to the existing CNC WebSocket
  -> renderer creates a POSIX shm test scanout
  -> renderer calls renderer_set_scanout and renderer_update_scanout actions
  -> scorpi-hv forwards those as current ScorpiViewer CNC notifications
  -> ScorpiViewer displays renderer-owned output
```

Non-goals:

- no DirectX;
- no Metal/Vulkan rendering;
- no renderer-owned virtqueue transport;
- no blob resources;
- no Windows driver changes.

### MS2: Host Renderer Test Frame

Replace the software test pattern with a backend-rendered frame:

- macOS: Metal clear/triangle into a renderer-owned target;
- Linux: Vulkan clear/triangle into a renderer-owned target;
- export to the same display path used in MS1.

### MS3A: Generic External Virtio Backend Framework

Create the reusable setup/control layer for external virtio backends:

- backend registration;
- device description;
- memory-region metadata;
- virtqueue metadata;
- kick notification;
- interrupt request;
- reset/disconnect handling.

The first implementation uses CNC WebSocket setup/control messages. It does not
move submit payloads through JSON.

### MS3B: Generic Virtio-Host Frontend

Add a separate `virtio-host` PCI device implementation in `scorpi-hv`.
Do not modify the validated `pci_virtio_gpu.c` path for external backend work.

The VM config selects one path:

- `virtio-gpu`: current validated internal `scorpi-hv` 2D/display device;
- `virtio-host,device=virtio-gpu,backend=gpu0`: generic external virtio
  frontend that presents virtio-gpu identity while `scorpi-gpu-renderer`
  implements the virtio-gpu backend.

Do not enable both for the same Windows VM unless intentionally testing two
display adapters.

### MS3C: Renderer-Owned Virtqueue Transport

Move accelerated virtio-gpu queue ownership out of `scorpi-hv`:

- WebSocket control messages for renderer registration and transport setup;
- guest memory region export/metadata;
- virtqueue address/size/feature metadata;
- renderer-side descriptor walking and used-ring updates;
- HV-to-renderer kick notification;
- renderer-to-HV interrupt request;
- device reset and backend-disconnect handling.

Hot-path rule:

```text
guest virtqueue memory
  -> scorpi-gpu-renderer reads descriptor/avail rings directly
  -> scorpi-gpu-renderer reads command/resource payloads directly
  -> scorpi-gpu-renderer writes responses and used ring directly
  -> scorpi-gpu-renderer asks scorpi-hv to inject a virtio interrupt
```

`scorpi-hv` must not call into a submit forwarding path for renderer-owned
queues. Its role is setup and signaling.

### MS4: Accelerated Virtio-GPU Objects

Implement renderer-side:

- capset query;
- context create/destroy;
- blob resource create/map/unmap;
- resource attach/detach;
- submit;
- fences;
- blob scanout.

### MS5: D3D12 Semantic Frontend

Add the Scorpi D3D12 packet decoder and validator:

- resource/heaps;
- root signatures;
- descriptors;
- shaders;
- PSOs;
- command lists;
- barriers;
- draws/copies/clears;
- present;
- fence signaling.

### MS6: Windows Guest Native DX12 Prototype

Extend the Windows KMD/UMD path until a native D3D12 triangle presents through
the external renderer.

### MS7: Hardening

Add:

- renderer crash recovery and GPU-lost behavior;
- command replay/dump tooling;
- shader cache inspection;
- security validation limits;
- performance tracing;
- HLK-oriented test coverage.

## MS1 Concrete Tasks

Status: in progress. Process separation and display brokering are the active
scope; the renderer is launched separately and `scorpi-hv` does not spawn it.

1. [done] Create `/Users/alexf/work/scorpi-gpu-renderer` as a buildable daemon.
2. [done] Reuse the existing CNC WebSocket protocol. Add renderer-originated CNC
   actions:
   - `renderer_set_scanout`
   - `renderer_update_scanout`
   - `renderer_unset_scanout`
3. [done] Add renderer CLI:

```sh
scorpi-gpu-renderer \
  --backend metal \
  --cnc-socket <path> \
  --display-mode hv-broker \
  --vm-uuid <uuid>
```

4. [done] Make `scorpi-hv` register renderer display broker actions on the existing
   CNC command surface. Do not make `scorpi-hv` spawn the renderer.
5. [pending] Start `scorpi-gpu-renderer` separately with the active CNC socket path.
6. [done] Make the renderer connect to the existing CNC WebSocket using
   libwebsockets as a `scorpi-cnc` client.
7. [done] Make the renderer allocate a POSIX shm test scanout.
8. [done] Make the renderer send `renderer_set_scanout`.
9. [done] Make `scorpi-hv` translate that to `console_set_scanout`.
10. [done] Make the renderer send `renderer_update_scanout`.
11. [done] Make `scorpi-hv` translate that to ScorpiViewer's `update_scanout`
    notification.
12. [done] Add clean shutdown using `renderer_unset_scanout`.
13. [done] Keep all MS1 behavior external-client driven so the validated display-only
    driver path is unchanged unless the renderer publishes a scanout.

Remaining MS1 validation:

- run `scorpi-hv` normally;
- launch `scorpi-gpu-renderer` separately with the active CNC socket path;
- verify ScorpiViewer receives the renderer-owned shared-memory scanout;
- verify Ctrl-C or SIGTERM from the renderer clears the scanout through
  `renderer_unset_scanout`.

## MS2 Concrete Tasks

Status: started.

1. [done] Add a renderer backend abstraction in `scorpi-gpu-renderer`.
2. [done] Keep the existing software test pattern as `stub`/`software`.
3. [done] Add macOS Metal backend compilation to Meson.
4. [done] Render an offscreen Metal BGRA frame into a renderer-owned target.
5. [done] Copy the Metal frame into the existing POSIX shm scanout.
6. [pending] Validate `--backend metal` against live `scorpi-hv` and
   ScorpiViewer.
7. [pending] Add a Linux Vulkan backend equivalent.
8. [pending] Replace the one-shot frame with a timed redraw/update loop if the
   viewer path needs repeated frame publication.

## MS3 Concrete Tasks

Status: started. This milestone is the first transport milestone for the
eventual DX12 path. It does not execute DirectX yet.

1. [done] Define the generic CNC WebSocket setup/control actions:
   - `virtio_backend_register`
   - `virtio_device_describe`
   - `virtio_queue_kick`
   - `virtio_queue_interrupt`
   - `virtio_device_reset`
   - `virtio_backend_disconnect`
2. [done] Add a renderer registration state in `scorpi-hv` without spawning the
   renderer.
3. [done] Make `scorpi-gpu-renderer` register and disconnect over the generic
   external virtio setup/control surface.
4. [partial] Add a transport description object in `scorpi-hv` containing:
   - negotiated virtio-gpu features;
   - queue index, size, descriptor address, avail address, and used address;
   - exported guest memory region metadata;
   - reset generation.
   Current status: the new `virtio-host` frontend publishes feature bits, reset
   generation, and queue metadata for its external backend. Guest memory region
   export metadata is not populated yet.
5. [partial] Add a renderer-side transport object in `scorpi-gpu-renderer` that stores the
   transport description received over CNC.
   Current status: the renderer requests `virtio_device_describe` and records
   whether the transport is ready.
6. [pending] Add renderer-side virtqueue helpers:
   - translate guest physical address to mapped host pointer;
   - read descriptor table;
   - read avail ring;
   - walk descriptor chains;
   - write response data;
   - publish used entries.
7. [partial] Add `virtio-host` queue handling so `scorpi-hv` only sends a kick
   notification for renderer-owned queues. It must not parse or copy command
   payloads for those queues.
   Current status: `virtio-host` sends generic `virtio_queue_kick`
   notifications.
8. [pending] Add a renderer completion path where `scorpi-gpu-renderer` asks `scorpi-hv`
   to inject the virtio interrupt after updating the used ring.
9. [pending] Validate with one minimal renderer-owned virtio command:
   - guest posts command;
   - HV sends kick only;
   - renderer reads the command directly from the queue;
   - renderer writes a valid virtio-gpu response;
   - renderer requests interrupt;
   - guest receives completion.
10. [pending] Only after this works, add renderer-side handling for contexts, blob
   resources, `SUBMIT_3D`, and fences.
