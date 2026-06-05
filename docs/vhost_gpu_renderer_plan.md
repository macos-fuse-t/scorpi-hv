# Vhost GPU Renderer Plan

Date: 2026-06-05

This plan separates GPU rendering from `scorpi-hv` while preserving virtio as
the guest-visible command transport. The production transport between
`scorpi-hv` and renderer backends uses vhost-style semantics over Unix domain
sockets: memory table export, vring setup, kick/call fds, and backend-owned
virtqueue processing.

The Scorpi wire format is not QEMU-compatible. We use the vhost architecture
and operating model, but keep a Scorpi-owned protocol so payloads fit our VM
memory model, macOS notification fallback, and future multi-device requirements.

## Target Architecture

```text
Windows guest
  -> virtio-gpu PCI device exposed by scorpi-hv
  -> generic Scorpi vhost frontend in scorpi-hv
       -> guest memory export
       -> vring address setup
       -> kick/call fd wiring
       -> interrupt injection
  -> Scorpi vhost Unix domain socket
  -> scorpi-gpu-renderer process
       -> Scorpi vhost backend server
       -> direct virtqueue descriptor reader
       -> virtio-gpu and future D3D12 command frontend
       -> macOS Metal backend
       -> Linux Vulkan backend
  -> ScorpiViewer display/control path
```

`scorpi-hv` owns:

- virtio PCI/MMIO frontend behavior;
- feature negotiation and device config exposure;
- guest memory export to backend processes;
- vring setup, queue kick forwarding, and call interrupt handling;
- VM reset, backend disconnect, and process-independent lifecycle handling.

`scorpi-hv` must not parse, copy, or forward virtio-gpu or D3D12 command
payloads on renderer-owned queues.

`scorpi-gpu-renderer` owns:

- Scorpi vhost backend server socket;
- guest memory mapping from Scorpi vhost memory table fds;
- virtqueue descriptor walking and used-ring updates;
- virtio-gpu command handling;
- EDID, display info, scanout, cursor, and resource state;
- host display probing and renderer scanout sizing;
- future D3D12 semantic packet decoding and Metal/Vulkan execution.

ScorpiViewer remains a display/input client. It should not know virtio-gpu,
D3D12, Metal, Vulkan, or Scorpi vhost.
Viewer window resizing must not drive vhost GPU display configuration. The
renderer publishes scanout metadata and dirty rectangles to `scorpi-hv`, and
`scorpi-hv` only exposes that published scanout to ScorpiViewer.

## Transport Decision

The WebSocket/CNC queue transport was a bring-up path and is no longer used for
renderer virtqueues. WebSocket may remain for viewer control and debug surfaces.
Virtio queue ownership lives in the Scorpi vhost transport.

Production transport properties:

- AF_UNIX stream socket for Scorpi vhost control messages;
- `SCM_RIGHTS` fd passing for guest memory, kick fds, call fds, and future
  device-specific fds;
- fd-based notifications by default;
- optional in-band notifications only as fallback/debug;
- no JSON on the virtio datapath.

## Run And Configure

`scorpi-gpu-renderer` is launched separately and must own the backend socket.
`scorpi-hv` connects to that socket when the VM starts; it does not spawn the
renderer and it does not send host display dimensions. The renderer probes host
display limits, publishes EDID/display modes, creates scanout shared memory, and
notifies `scorpi-hv` about scanout and dirty-rectangle updates.

Start the renderer first:

```sh
cd /Users/alexf/work/scorpi-gpu-renderer
./build/scorpi-gpu-renderer \
  --backend metal \
  --listen /tmp/scorpi-vm1-gpu.sock
```

Then start the VM:

```sh
cd /Users/alexf/work/scorpi-hv
./build-macos-arm64/scorpi-hv -f ./win2.yaml
```

The VM config uses a vhost GPU device entry like:

```yaml
- device: virtio-gpu-vhost
  backend: gpu0
  backend_device: virtio-gpu
  socket: /tmp/scorpi-vm1-gpu.sock
  hdpi: true
```

Runtime ownership:

- renderer owns the socket listener, host display probing, EDID, display modes,
  scanout allocation, virtio-gpu command processing, and future D3D12-to-Metal
  execution;
- `scorpi-hv` owns the guest-visible virtio PCI device, feature negotiation,
  guest memory export, vring setup, kick/call fd wiring, and interrupt
  injection;
- ScorpiViewer reads the scanout exposed by `scorpi-hv` and remains independent
  of vhost, virtio-gpu, Metal, Vulkan, and D3D12 internals.

## Reference Assessment

The OpenEBS `vhost-user` repository is useful as a compact reference for:

- fixed-size message headers with typed payloads;
- `sendmsg`/`recvmsg` plus `SCM_RIGHTS` fd passing;
- setup ordering: features, memory table, queue size/base, queue fds, queue
  addresses;
- fd-driven notification instead of JSON or copied command payloads.

Do not import it wholesale. Its vring code allocates backend-owned shared
vrings, uses Linux `eventfd`, and is tied to DPDK/SPDK storage assumptions.
Scorpi must instead export VM guest RAM from `scorpi-hv`, let the renderer map
that RAM, and have the renderer walk guest-owned virtio rings directly.

## Generic Device Model

The HV side should be a reusable Scorpi vhost frontend:

```text
scorpi-hv
  pci_virtio_vhost.c
    generic Scorpi vhost mechanics
    memory table export
    vring setup
    kick/call fd wiring
    reset/disconnect

  pci_virtio_gpu_vhost.c
    virtio-gpu identity
    config space
    feature mask
    queue count
    scanout/viewer compatibility

  pci_virtio_fs_vhost.c
    future virtio-fs adapter
```

Backends should be separate processes:

```text
scorpi-gpu-renderer
scorpi-fs-backend       future
other Scorpi vhost backends
```

## Queue Notification Flow

Guest-to-backend kick:

```text
guest writes descriptors
guest updates avail ring
guest notifies virtio queue
scorpi-hv writes queue kick fd
renderer drains kick fd
renderer walks descriptors from mapped guest RAM
```

Backend-to-guest completion:

```text
renderer writes response buffers
renderer updates used ring
renderer writes queue call fd
scorpi-hv drains call fd
scorpi-hv injects virtio interrupt/MSI-X
guest driver observes completion
```

On macOS, notification fds are implemented with `pipe` or `socketpair` instead
of Linux `eventfd`. The abstraction must hide that difference from the vhost
frontend/backend code.

## Milestones

### MS1: Shared Scorpi vhost foundation

Deliverables:

- public Scorpi vhost protocol header in `include/scorpi/protocol`;
- UDS connect/listen helpers;
- `SCM_RIGHTS` send/receive helpers;
- portable notify-fd abstraction for macOS/Linux;
- installed protocol header via `meson install`.

### MS2: Renderer Scorpi vhost skeleton

Deliverables:

- renderer listens on `--listen <socket>`;
- accepts a Scorpi vhost connection;
- completes feature negotiation;
- accepts memory table and vring setup messages;
- logs queue and memory setup;
- no virtio-gpu behavior change yet.

### MS3: HV generic Scorpi vhost frontend

Deliverables:

- `pci_virtio_vhost.c` sends Scorpi vhost setup messages;
- exports VM memory once per backend connection;
- creates kick/call notify fds per queue;
- registers call fds in the HV event loop;
- forwards queue notify as fd kicks;
- injects queue interrupts when call fd fires.

### MS4: Move virtio-gpu-vhost queues to Scorpi vhost

Deliverables:

- renderer processes existing virtio-gpu queue code from Scorpi vhost vrings;
- WebSocket/CNC virtqueue metadata/kick/interrupt path removed from GPU vhost;
- parity for display info, EDID, resource create/attach, transfer, flush, and
  cursor commands.

### MS5: Cleanup and multi-device readiness

Deliverables:

- Scorpi vhost config documented in VM YAML;
- memory export shared across multiple vhost devices where possible;
- robust reset/disconnect/shutdown behavior;
- WebSocket retained only for viewer/control/debug paths;
- future `virtio-fs` backend can reuse the generic frontend.

### MS6: D3D12/Metal acceleration path

Deliverables:

- virtio-gpu capsets/contexts/blob resources;
- D3D12 command packet frontend;
- Metal backend execution;
- fences and presentation path;
- Linux Vulkan backend later.

## First Implementation Tasks

1. Add `include/scorpi/protocol/vhost_user.h`.
2. Add local Scorpi vhost socket/fd helper API.
3. Build and install the new public header with `scorpi_kit`.
4. Add renderer-side Scorpi vhost server skeleton.
5. Add HV-side feature negotiation against the renderer.
6. Add memory table export via Scorpi vhost.
7. Add vring setup messages.
8. Add kick/call fd notification loop.
9. Port `virtio-gpu-vhost` command processing to Scorpi vhost queues.
10. Remove WebSocket queue transport for GPU vhost.
