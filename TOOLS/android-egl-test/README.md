# Android EGL resize regression tests

This standalone Android test application compiles the selected **actual mpv
Android OpenGL backend**. It does not depend on Media3 test classes or change the
TV application. The native adapter replaces VO window ownership, logging and RA
framebuffer metadata wrapping; EGL, native windows, GL objects and compositor
screenshots are real. It is not a full libmpv renderer/decoder integration test.

## Run on an explicitly selected device

Requirements: Windows PowerShell 7, JDK 17+, Gradle 9.1+, Android SDK platform 36,
NDK 29.0.14206865, CMake 3.22.1, and an Android API 28+ device with at least 640x360
usable landscape pixels. API 28 is a **test-only** requirement for reading the
native dataspace; production libmpv's minimum API is unchanged.

Use matching FFmpeg and libplacebo headers from the native dependency build or
exported renderer SDK. These are compile-time headers, not prebuilt libmpv code.
An existing Gradle wrapper may be used as the launcher; `-p` points it exclusively
at this standalone project. No other repository's Gradle project is built.

```powershell
$env:JAVA_HOME = 'C:/path/to/jdk-21'
./run.ps1 -Serial emulator-5554 -OutputDirectory D:/Temp/mpv-egl-current `
    -Gradle C:/path/to/gradle-9.1.0/bin/gradle.bat `
    -AndroidSdk C:/path/to/Android/Sdk `
    -FFmpegInclude D:/native/ffmpeg/include `
    -PlaceboInclude D:/native/renderer-sdk/arm64-v8a/include
```

The default ABI is x86_64; `-Abi arm64-v8a` or `-Abi armeabi-v7a` selects an ARM device.
`-BuildOnly` produces the APKs without installing or operating a device.
`-TestMethod` selects one of the six named cases below. Use a separate output
directory for each source revision. Build/cache/native intermediates, source/APK
hashes, instrumentation output and screenshots stay in that output directory.

The runner checks build success before installing, verifies the source has not
changed during the build, requires the expected JUnit pass count (adb exit status
alone is insufficient), and imposes a 90-second instrumentation deadline. It
installs/replaces only `org.mpv.egl.testapp` and its `.test` package, then removes
both even when a test fails. Their APKs and collected evidence remain on the host.
It never uninstalls or clears `com.fongmi.android.tv`, changes player preferences,
or selects a different adb device automatically. Running tests occupies the
selected display and can interrupt foreground playback.

## Regression coverage

- `detachedWindow_nextProducerFollowsConsumerSize`,
  `releasedWindow_nextProducerFollowsConsumerSize`, and
  `replacedWindow_nextProducerFollowsConsumerSize`: after the actual backend
  detaches, shuts down, or moves to another window, a new EGL producer reuses the
  original Surface. Landscape and portrait resizes must follow the consumer's
  dimensions without inheriting mpv's previous buffer size. RA cleanup and VO
  ownership remain adapters; native window and EGL teardown are real.
- `firstFrameAndPausedRedrawKeepDrawableAligned`: twelve alternating sizes;
  actual drawable/VO agreement; CHECK_EVENTS and REDRAW; no duplicate resize
  events; EGLSurface/context/texture identity; native Display P3 retention;
  first submitted frame fills the window with the correct red/blue split.
- `continuousResizePreservesSurfaceAndFrameSubmission`: eight 150 ms animations
  driven independently by the UI thread while rendered frame IDs keep changing;
  no surface recreation; one successful native swap per submitted frame. Reports
  frame count, resize count and maximum submission gap. The 150 ms stall limit is
  a coarse regression guard, **not** proof of smooth compositor presentation.
- `pendingColorResizeAndWindowReattachKeepContext`: color negotiation during a
  pending resize cannot change the drawable behind VO metadata; real detach and
  reattach retain context/texture and reapply the non-default swap interval.
  The output explicitly reports EGL colorspace support. Rejection/recovery on a
  device lacking that extension is not proof of successful HDR output.

First-frame screenshots check an encoded frame ID before examining the split and
edge grid. They wait at most one second for compositor presentation without
drawing or swapping another frame. This prevents an old frame from satisfying the
geometry assertion. This waiting is in the test observer, not production playback.

## Negative control

Export `video/out/opengl/context_android.c` from the known bad recreation commit
`b84bfba5f87c8d573aaae7bfd974be8cff534740` using `git archive` into a separate
directory. Do not switch or overwrite the production checkout. Repeat the command
with `-ContextSource PATH/TO/EXPORTED/context_android.c`, a different output
directory, and `-TestMethod firstFrameAndPausedRedrawKeepDrawableAligned`.
It must fail specifically at `Ordinary resize recreated EGLSurface`; a compilation
failure, timeout or unrelated assertion does not establish the negative control.
The runner deliberately exits with failure for this source; it never marks a
negative control as a passing release test.

## End-to-end acceptance still required

Use the same hash-verified TV APK and clip/ratio on LDPlayer. Check Live and VOD,
gpu/OpenGL and gpu-next/OpenGL, playing and paused, repeated small/fullscreen
transitions, and return to playback. Confirm no position jump, exposed black
region or frozen transition. Keep Vulkan/Exo as comparison paths. Record the
actual backend, scenario and visible result; synthetic fixture success cannot
stand in for these checks.

References: [Android native buffer geometry](https://developer.android.com/ndk/reference/group/a-native-window#anativewindow_setbuffersgeometry),
[EGL swap contract](https://github.com/KhronosGroup/EGL-Registry/blob/main/sdk/docs/man/eglSwapBuffers.xml),
[Android native build integration](https://developer.android.com/studio/projects/gradle-external-native-builds).
