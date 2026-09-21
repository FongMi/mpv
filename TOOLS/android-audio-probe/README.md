# Android compressed-audio experiments

These standalone tools are not part of libmpv and do not enable a new mpv
playback mode. `AndroidAudioProbe` queries raw AAC-LC and MP3 support without creating an
AudioTrack, changing settings, or playing sound. It checks movie and music
attributes, matching mpv's video and audio-only cases. Re-run it after changing the
audio route. Do not cache its results as permanent device capabilities.

With JDK 17+, Android SDK platform 33+, and SDK build-tools installed:

```sh
mkdir -p classes dex
javac --release 8 -cp "$ANDROID_SDK_ROOT/platforms/android-33/android.jar" \
    -d classes AndroidAudioProbe.java AndroidAudioOffloadExperiment.java EncodedAudioChecks.java
"$ANDROID_SDK_ROOT/build-tools/33.0.2/d8" --min-api 21 \
    --lib "$ANDROID_SDK_ROOT/platforms/android-33/android.jar" \
    --output dex classes/*.class
adb -s DEVICE_SERIAL push dex/classes.dex /data/local/tmp/mpv-audio-probe.dex
adb -s DEVICE_SERIAL shell CLASSPATH=/data/local/tmp/mpv-audio-probe.dex \
    app_process /system/bin AndroidAudioProbe
```

Use the installed SDK and build-tools versions instead of the example paths.
The shell query is a preflight, not proof that the playback application's UID,
attributes, routing, or resource allocation will produce the same result.

## Explicit offload playback experiment

`AndroidAudioOffloadExperiment --play FILE` **plays sound**. It requires API 29+,
the exact offload format to be supported, and a local single-track mono/stereo
test file of at most five seconds. It never tries another output mode or changes
settings. AAC is deliberately limited to a simple two-byte LC AudioSpecificConfig,
without SBR, PS, PCE, short frames or extensions. Use the controlled fixtures below
instead of arbitrary media. This is a sink-only experiment, not an A/V player or
a libmpv integration, seek, gapless or power-consumption test.

Create quiet, two-second fixtures on the host (FFmpeg required):

```sh
ffmpeg -n -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' \
    -af volume=0.1 -ac 2 -c:a aac -profile:a aac_low -b:a 96000 -f adts tone-lc.aac
ffmpeg -n -i tone-lc.aac -c:a copy -bsf:a aac_adtstoasc tone-lc.m4a
ffmpeg -n -f lavfi -i 'sine=frequency=660:sample_rate=44100:duration=2' \
    -af volume=0.1 -ac 2 -c:a libmp3lame -b:a 128000 tone.mp3
adb -s DEVICE_SERIAL push tone-lc.m4a /data/local/tmp/mpv-offload-tone.m4a
adb -s DEVICE_SERIAL shell CLASSPATH=/data/local/tmp/mpv-audio-probe.dex \
    app_process /system/bin AndroidAudioOffloadExperiment --play /data/local/tmp/mpv-offload-tone.m4a
```

Repeat with the MP3 fixture at a separate path. Start with a low device/receiver
volume and stop other playback first; this shell experiment does not manage audio
focus. Successful completion reports bytes and packets written and waits for
`onPresentationEnded` after `stop()`; it still requires human confirmation of sound.
Partial writes retain their byte offset. Write errors, teardown, interruption,
invalid packets and backpressure timeouts abort the experiment instead of retrying
forever. The deadline cannot recover a vendor API call that hangs inside the driver;
run this separate process under an external timeout during device testing.

The writer intentionally does not turn encoded byte counts into sample counts or
infer an audio clock. This must still be designed and tested before porting to mpv.
The callback-based drain is specific to offload; non-offloaded compressed playback
is not implemented by this tool.

Host validation, without Android or a receiver:

```sh
mkdir -p host-classes
javac --release 8 -d host-classes EncodedAudioChecks.java EncodedAudioChecksTest.java
java -cp host-classes EncodedAudioChecksTest
```

These checks cover configuration rejection, byte-buffer ownership, partial writes,
backpressure, errors, cancellation, interruption and bounded drain completion,
not device playback.

## Interpretation

- Below API 29, the public support query is unavailable. This does not mean the
  device can or cannot decode AAC/MP3 in hardware.
- API 29-32 combines direct and offloaded support in its direct-support boolean.
  It cannot positively distinguish a non-offloaded bitstream route.
- API 33+ reports bitstream and offload support separately. Neither guarantees
  successful AudioTrack creation, audible output, or lower power consumption.
- Raw AAC/MP3 is not the existing `audio-spdif` IEC 61937 path for surround audio.
  No change to TrueHD, DTS, E-AC-3, normalization, or user options is made here.

## Gate before implementing a libmpv playback experiment

A supported target and a short audible sample are required. The experiment must
keep encoded byte counts separate from decoded sample counts throughout frame
storage, partial writes, queues, seek, drain, and audio-clock calculation. Do not
assume every AAC frame has 1024 samples or every MP3 frame has 1152 samples.
Offload additionally requires its own stream-event and end-of-stream handling.
Filters, speed changes, initialization failures, and route changes need bounded
PCM recovery before any default policy can change.

References: [AudioTrack](https://developer.android.com/reference/android/media/AudioTrack),
[AudioManager](https://developer.android.com/reference/android/media/AudioManager).
