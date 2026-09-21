// SPDX-License-Identifier: LGPL-2.1-or-later

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Explicit, short, audio-only offload experiment. No fallback or mpv integration. */
public final class AndroidAudioOffloadExperiment {
    private static final int MAX_PACKET_BYTES = 64 * 1024;
    private static final long MAX_DURATION_US = 5_000_000;

    private AndroidAudioOffloadExperiment() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 2 || !args[0].equals("--play")) {
            System.err.println("Usage: AndroidAudioOffloadExperiment --play LOCAL_TEST_FILE");
            System.exit(2);
        }
        try {
            if (Build.VERSION.SDK_INT < 29) {
                throw new UnsupportedOperationException("Offload experiment requires API 29+");
            }
            play(new File(args[1]));
        } catch (Exception e) {
            System.err.println(new JSONObject().put("status", "failed").put("error", e.toString()));
            System.exit(1);
        }
    }

    private static void play(File file) throws Exception {
        if (!file.isFile()) throw new IllegalArgumentException("A local test file is required");
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(file.getAbsolutePath());
            if (extractor.getTrackCount() != 1) {
                throw new IllegalArgumentException("Use a single-track audio-only test file");
            }
            MediaFormat source = extractor.getTrackFormat(0);
            long durationUs = source.containsKey(MediaFormat.KEY_DURATION)
                    ? source.getLong(MediaFormat.KEY_DURATION) : 0;
            if (durationUs <= 0 || durationUs > MAX_DURATION_US) {
                throw new IllegalArgumentException("Use a known-duration test file of at most 5 seconds");
            }
            int rate = source.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            int channels = source.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            if (channels != 1 && channels != 2) {
                throw new IllegalArgumentException("This experiment only supports mono or stereo");
            }
            String mime = source.getString(MediaFormat.KEY_MIME);
            int encoding;
            if ("audio/mp4a-latm".equals(mime)) {
                EncodedAudioChecks.requireSimpleAacLc(source.getByteBuffer("csd-0"), rate, channels);
                encoding = AudioFormat.ENCODING_AAC_LC;
            } else if ("audio/mpeg".equals(mime)) {
                encoding = AudioFormat.ENCODING_MP3;
            } else {
                throw new IllegalArgumentException("Only AAC-LC and MP3 are supported");
            }
            AudioFormat format = new AudioFormat.Builder().setEncoding(encoding)
                    .setSampleRate(rate).setChannelMask(channels == 1
                            ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO).build();
            AudioAttributes attributes = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
            boolean supported = Build.VERSION.SDK_INT >= 33
                    ? (AudioManager.getDirectPlaybackSupport(format, attributes)
                            & AudioManager.DIRECT_PLAYBACK_OFFLOAD_SUPPORTED) != 0
                    : AudioManager.isOffloadedPlaybackSupported(format, attributes);
            if (!supported) throw new UnsupportedOperationException("Exact offload format is unsupported");
            extractor.selectTrack(0);
            run(extractor, source, format, attributes, durationUs);
        } finally {
            extractor.release();
        }
    }

    private static void run(MediaExtractor extractor, MediaFormat source, AudioFormat format,
                            AudioAttributes attributes, long durationUs) throws Exception {
        AudioTrack track = new AudioTrack.Builder().setAudioFormat(format)
                .setAudioAttributes(attributes).setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes(MAX_PACKET_BYTES).setOffloadedPlayback(true).build();
        HandlerThread events = new HandlerThread("audio-offload-experiment");
        CountDownLatch ended = new CountDownLatch(1);
        AtomicBoolean tornDown = new AtomicBoolean();
        AudioTrack.StreamEventCallback callback = new AudioTrack.StreamEventCallback() {
            @Override
            public void onPresentationEnded(AudioTrack ignored) {
                ended.countDown();
            }

            @Override
            public void onTearDown(AudioTrack ignored) {
                tornDown.set(true);
                ended.countDown();
            }
        };
        boolean registered = false;
        try {
            if (track.getState() != AudioTrack.STATE_INITIALIZED || !track.isOffloadedPlayback()) {
                throw new IOException("AudioTrack was not initialized for offload");
            }
            events.start();
            Handler handler = new Handler(events.getLooper());
            track.registerStreamEventCallback(command -> handler.post(command), callback);
            registered = true;
            int delay = source.containsKey(MediaFormat.KEY_ENCODER_DELAY)
                    ? source.getInteger(MediaFormat.KEY_ENCODER_DELAY) : 0;
            int padding = source.containsKey(MediaFormat.KEY_ENCODER_PADDING)
                    ? source.getInteger(MediaFormat.KEY_ENCODER_PADDING) : 0;
            track.setOffloadDelayPadding(delay, padding);
            long deadlineNs = System.nanoTime() + TimeUnit.MICROSECONDS.toNanos(durationUs)
                    + TimeUnit.SECONDS.toNanos(5);
            track.play();
            ByteBuffer packet = ByteBuffer.allocateDirect(MAX_PACKET_BYTES);
            long bytesWritten = 0;
            int packetsWritten = 0;
            while (extractor.getSampleTrackIndex() >= 0) {
                EncodedAudioChecks.checkActive(() -> !tornDown.get(), deadlineNs);
                long size = extractor.getSampleSize();
                int flags = extractor.getSampleFlags();
                if (size <= 0 || size > MAX_PACKET_BYTES
                        || (flags & (MediaExtractor.SAMPLE_FLAG_ENCRYPTED
                                | MediaExtractor.SAMPLE_FLAG_PARTIAL_FRAME)) != 0) {
                    throw new IOException("Invalid, encrypted or partial compressed access unit");
                }
                packet.clear();
                int read = extractor.readSampleData(packet, 0);
                if (read != size) throw new IOException("Compressed access-unit size changed");
                packet.position(0);
                packet.limit(read);
                bytesWritten += EncodedAudioChecks.writeFully(packet,
                        data -> track.write(data, data.remaining(), AudioTrack.WRITE_NON_BLOCKING),
                        () -> !tornDown.get(), deadlineNs);
                packetsWritten++;
                extractor.advance();
            }
            if (packetsWritten == 0) throw new IOException("No compressed access units found");
            // stop() drains a streaming track; only the offload completion callback
            // confirms all queued data was presented. A successful write is not enough.
            track.stop();
            EncodedAudioChecks.awaitPresentationEnd(ended, () -> !tornDown.get(), deadlineNs);
            System.out.println(new JSONObject().put("status", "presentation-ended")
                    .put("format", format.toString()).put("packetsWritten", packetsWritten)
                    .put("bytesWritten", bytesWritten).put("audibleOutput", "requires human confirmation"));
        } finally {
            try {
                if (registered) track.unregisterStreamEventCallback(callback);
            } finally {
                try {
                    track.release();
                } finally {
                    events.quitSafely();
                    events.join(1000);
                }
            }
        }
    }
}
