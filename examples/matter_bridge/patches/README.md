# Camera dependency patches

These patches are applied with Git during CMake configuration by
[`dependency_compat.cmake`](../cmake/dependency_compat.cmake).

- `sc101iot-pclk-delay.patch`, based on `espressif/esp_cam_sensor` 2.4.0:
  define zero PCLK delay and apply it when setting the sensor format and
  before starting capture, including after wake. A failed timing write
  prevents format setup or stream start.
- `claw-video-stop-cleanup.patch`, based on `tda-2030/claw_video` 2.4.1~1:
  release the host controller even when a disconnected sensor cannot acknowledge
  stream stop. Stop callbacks before releasing interface resources. Host cleanup
  failures retain the controller handle for retry; a failed controller deletion
  also retains teardown progress so retry does not repeat completed cleanup.
  Successful host cleanup returns success so the video layer resets its stream
  and buffer state.

When updating dependencies, check whether these fixes are included upstream.
Remove an obsolete patch and its CMake call together. Configuration fails if
a dependency matches neither the original nor the patched source.
