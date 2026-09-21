// SPDX-License-Identifier: LGPL-2.1-or-later

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.function.ToIntFunction;

/** Pure Java checks for the standalone experiment, not libmpv audio-frame handling. */
final class EncodedAudioChecks {
    private static final int[] AAC_RATES = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };

    private EncodedAudioChecks() {}

    static void requireSimpleAacLc(ByteBuffer config, int rate, int channels) {
        // Deliberately accept only the two-byte AudioSpecificConfig produced by
        // the documented LC fixture: no PCE, SBR, PS, 960-sample frames or extensions.
        if (config == null || config.remaining() != 2) {
            throw new IllegalArgumentException("Use the documented simple AAC-LC fixture");
        }
        int position = config.position();
        int bits = ((config.get(position) & 0xff) << 8) | (config.get(position + 1) & 0xff);
        int rateIndex = (bits >> 7) & 0xf;
        int channelConfig = (bits >> 3) & 0xf;
        if ((bits >> 11) != 2 || rateIndex >= AAC_RATES.length
                || AAC_RATES[rateIndex] != rate || (bits & 7) != 0
                || channelConfig != channels || (channels != 1 && channels != 2)) {
            throw new IllegalArgumentException("Unsupported or inconsistent AAC-LC configuration");
        }
    }

    static int writeFully(ByteBuffer packet, ToIntFunction<ByteBuffer> writer,
                          BooleanSupplier valid, long deadlineNs)
            throws IOException, InterruptedException {
        int bytes = packet.remaining();
        while (packet.hasRemaining()) {
            checkActive(valid, deadlineNs);
            int position = packet.position();
            int remaining = packet.remaining();
            int written = writer.applyAsInt(packet);
            if (written < 0) {
                throw new IOException("AudioTrack write failed: " + written);
            }
            if (written > remaining || packet.position() - position != written
                    || packet.remaining() != remaining - written) {
                throw new IOException("AudioTrack returned an inconsistent byte count");
            }
            if (written == 0) Thread.sleep(10);
        }
        return bytes;
    }

    static void awaitPresentationEnd(CountDownLatch ended, BooleanSupplier valid, long deadlineNs)
            throws IOException, InterruptedException {
        // Teardown also wakes the waiter, but is not a successful presentation.
        do {
            checkActive(valid, deadlineNs);
        } while (!ended.await(Math.min(deadlineNs - System.nanoTime(),
                TimeUnit.MILLISECONDS.toNanos(100)), TimeUnit.NANOSECONDS));
        checkActive(valid, deadlineNs);
    }

    static void checkActive(BooleanSupplier valid, long deadlineNs)
            throws IOException, InterruptedException {
        if (Thread.interrupted()) throw new InterruptedException();
        if (!valid.getAsBoolean()) throw new IOException("Offloaded output was torn down");
        if (System.nanoTime() - deadlineNs >= 0) throw new IOException("Audio experiment timed out");
    }
}
