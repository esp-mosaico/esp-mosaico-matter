# mosaico_matter_camera

Board-native Matter camera endpoint for the ESP-Mosaico `matter_bridge`
example. The hot-plug bridge calls this component when a camera subboard is
inserted or removed.

The endpoint exposes Camera AV Stream Management with Video, Audio, and
Snapshot features (`FeatureMap = 0x07`), plus WebRTC Transport Provider/Requestor
clusters. Video uses the camera; audio and snapshots are synthetic test media.
It captures 1280x720 UYVY with one capture buffer when memory permits, then
falls back to 640x480 UYVY. HD
frames are center-cropped to 960x720; either mode is downsampled to 320x240
YUYV at 10 fps. It encodes H.264 in software and sends the encoded frames
through `esp_peer` 1.5.3. Matter carries SDP and ICE signaling; local host
candidates and controller-provided STUN/TURN servers are supported.
The bridge maps a controller's named SDP media IDs to the numeric IDs used by
`esp_peer`, then returns an Answer with the controller's original media order
and IDs. Sessions can select video, audio, or both. Unselected media and data
sections are explicitly rejected with port zero.
For a trickle-ICE controller, the bridge combines the answer and subsequent
candidate command before passing them to `esp_peer`. When the controller
originates the offer, the bridge also re-applies that offer with the received
candidates after sending its initial answer.

The board ID, serial number and dynamic endpoint ID are stored in NVS. Unplug
disables rather than destroys the endpoint, allowing the same module to resume
the same identity after replug or reboot. Replacing the module or factory
resetting the bridge erases that identity.

## Mock audio and snapshots

One audio stream can be allocated: LiveView, Opus, mono, 48,000 Hz,
6,000–128,000 bit/s, and 16-, 24-, or 32-bit depth. The peer task sends one
pre-encoded 20 ms silence packet every 20 ms while a session selects that
stream. Packet padding follows the allocated bitrate (rounded down to a
whole number of bytes per packet). This uses no microphone or
audio encoder. Mute and volume attributes retain their Matter state; the
output remains silent. Speaker and two-way talk are not advertised.
Repeated allocation of the same format reuses its stream ID. Deallocate the
existing audio stream before requesting a different bitrate or bit depth;
otherwise allocation returns `ResourceExhausted`.

One JPEG snapshot stream can be allocated at 320×240 and a maximum frame rate
of 1 fps. `CaptureSnapshot` returns a fixed 527-byte grayscale quadrant image,
independent of the sensor and requested quality. Capture requires an allocated
snapshot stream and a requested resolution of 320×240. Other codecs and
resolutions are rejected. The static image consumes no video encoder capacity.

Allocation lists and microphone settings use the shared cluster's persistent
storage. Video configuration is restored when persistent attributes load;
mock audio and snapshots need no additional runtime state. Unplug stops the
peer, including silence transmission. The same module retains its allocations
on replug. Audio/video deallocation stops a session that uses that stream.

## Stream commands

After building and flashing the bridge, commission it and plug in the camera.
Replace `NODE_ID` and `ENDPOINT_ID` with the commissioned node and camera
endpoint. These commands allocate mock streams and capture the test image:

```sh
chip-tool descriptor read device-type-list NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement read feature-map NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement read microphone-capabilities NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement read snapshot-capabilities NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement audio-stream-allocate 3 0 1 48000 20000 24 NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement snapshot-stream-allocate 0 1 '{"width":320,"height":240}' '{"width":320,"height":240}' 75 NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement capture-snapshot 1 '{"width":320,"height":240}' NODE_ID ENDPOINT_ID
```

Expect Camera `0x0142` in the device-type list, feature map `7`, and stream ID
`1` in each allocation response. Snapshot and audio IDs have separate scopes.
Read `AllocatedAudioStreams` and `AllocatedSnapshotStreams` after allocation,
reboot, and same-module replug; capture again after restoration. Test unknown
stream IDs, unsupported codecs, audio bitrates outside 6,000–128,000, and a
capture resolution other than 320×240; each should be rejected.

Use a WebRTC controller that requests audio stream `1`, video, or both. Audio
must negotiate Opus; the serial log should show `First mock Opus silence frame
sent` and the controller's inbound audio packet count should increase. Verify
video-only sessions still work and audio-only sessions do not start the H.264
task. End a session or unplug the module and verify packets stop. Deallocate
the mock streams when finished:

```sh
chip-tool cameraavstreammanagement audio-stream-deallocate 1 NODE_ID ENDPOINT_ID
chip-tool cameraavstreammanagement snapshot-stream-deallocate 1 NODE_ID ENDPOINT_ID
```

### Audio allocation diagnostics

`0x0551`, command `0x00`, status `0xcf` means `AudioStreamAllocate` returned
`DynamicConstraintError`. The bridge logs every request's usage, codec,
channels, sample rate, bitrate, and bit depth, followed by the rejection reason
or allocated/reused stream ID. Use the `AudioStreamAllocate ep=...` line when
debugging a controller request. The earlier fixed 20 kbps / 24-bit profile
could reject other valid mono Opus requests; it is now a range of supported
bitrates and bit depths. Stereo, AAC, and sample rates other than 48 kHz remain
unsupported.
