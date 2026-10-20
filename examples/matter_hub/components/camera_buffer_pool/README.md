# Camera buffer pool

The Matter hub's Mosaico configuration reserves the large image buffers at the
start of `app_main`, before other application services fragment PSRAM. The
reservation stays allocated when the camera is closed; opening it borrows the
same storage again.

Default reservation (DMA-capable PSRAM, aligned to at least 64 bytes):

| Buffer | Bytes |
| --- | ---: |
| Capture arena: one 1280×720 YUV422 frame, or two 640×480 frames | 1,843,200 |
| Dedicated DVP discard frame | 1,843,200 |
| Two 480×480 RGB565 preview frames | 921,600 |
| Total | 4,608,000 |

`CONFIG_CAMERA_BUFFER_POOL_ENABLE` is enabled in the hub's Mosaico board
defaults. Capture/discard and preview capacities are configurable through
`CONFIG_CAMERA_BUFFER_POOL_CAPTURE_BYTES` and
`CONFIG_CAMERA_BUFFER_POOL_PREVIEW_BYTES`. Reservation failure aborts startup
and logs the requested size, remaining free memory, and largest free block.
Other configurations retain dynamic allocation with the pool disabled.

`cmake/integrate.cmake` compiles adapted copies of the pinned camera and video
sources under the build directory. It leaves managed components and submodules
untouched and rejects unexpected source patterns when dependencies change.
The camera uses V4L2 USERPTR capture buffers. The DVP driver disables its
internally allocated backup frame and uses the reserved discard frame when no
capture frame is queued. Discard completions are never delivered as capture
frames, so DMA cannot overwrite a frame still borrowed by a consumer.

The camera-service adapter retains the borrowed camera handle across hotplug,
marks it unavailable during removal, and records physical presence changes.
The next open closes the stale stream, recreates the DVP video/sensor device,
and reapplies the sensor's register configuration. Removal callbacks do not
delete a handle used by a capture task. A missing sensor's SCCB response does
not prevent DMA/controller teardown; controller teardown failures still retain
the buffer lease. The Matter and Camera app descriptors give their GSP image
caches a 1 MiB budget so their 460,800-byte preview images fit after reservation.

Owners return capture leases only after stopping DMA and returning borrowed
frames. Preview leases return after the capture task and canvas have stopped.
The component reserves image payloads; small driver objects, DMA descriptors,
QR processing, and JPEG output can still allocate dynamically. Applications
using this component must call `camera_buffer_pool_init()` before camera users
start, and include the CMake adapter when enabling reserved DVP capture.
