// SPDX-License-Identifier: LGPL-2.1-or-later

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Host tests: java -cp classes EncodedAudioChecksTest */
public final class EncodedAudioChecksTest {
    private static int checks;

    public static void main(String[] args) throws Exception {
        testAacLcConfiguration();
        testPartialWrites();
        testPresentationEnd();
        System.out.println(checks + " host checks passed");
    }

    private static void testAacLcConfiguration() throws Exception {
        ByteBuffer config = ByteBuffer.wrap(new byte[]{0, 0x11, (byte) 0x90});
        config.position(1);
        EncodedAudioChecks.requireSimpleAacLc(config, 48000, 2);
        require(config.position() == 1, "AAC validation must not consume caller data");
        expect(IllegalArgumentException.class,
                () -> EncodedAudioChecks.requireSimpleAacLc(config, 44100, 2));
        expect(IllegalArgumentException.class,
                () -> EncodedAudioChecks.requireSimpleAacLc(config, 48000, 1));
        expect(IllegalArgumentException.class,
                () -> EncodedAudioChecks.requireSimpleAacLc(null, 48000, 2));
        for (int bits : new int[]{0x2990, 0x1790, 0x1194, 0x1192, 0x1191}) {
            expect(IllegalArgumentException.class, () -> EncodedAudioChecks.requireSimpleAacLc(
                    ByteBuffer.wrap(new byte[]{(byte) (bits >> 8), (byte) bits}), 48000, 2));
        }
        expect(IllegalArgumentException.class, () -> EncodedAudioChecks.requireSimpleAacLc(
                ByteBuffer.wrap(new byte[]{0x11, (byte) 0x90, 0}), 48000, 2));
    }

    private static void testPartialWrites() throws Exception {
        ByteBuffer packet = ByteBuffer.allocateDirect(11);
        packet.position(2);
        AtomicInteger writes = new AtomicInteger();
        int bytes = EncodedAudioChecks.writeFully(packet, data -> {
            if (writes.getAndIncrement() == 0) return 0;
            int count = Math.min(2, data.remaining());
            data.position(data.position() + count);
            return count;
        }, () -> true, deadline());
        require(bytes == 9 && packet.position() == 11 && writes.get() == 6,
                "Partial writes must preserve all encoded bytes");
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> -6, () -> true, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> 2, () -> true, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> 5, () -> true, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> { data.limit(0); return 0; }, () -> true, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> { throw new AssertionError("must not write"); },
                () -> false, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> { throw new AssertionError("must not write"); },
                () -> true, System.nanoTime() - 1));
        AtomicBoolean valid = new AtomicBoolean(true);
        ByteBuffer cancelled = ByteBuffer.allocate(4);
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(cancelled, data -> {
            data.position(data.position() + 1);
            valid.set(false);
            return 1;
        }, valid::get, deadline()));
        require(cancelled.position() == 1, "Teardown must prevent the next partial write");
        require(EncodedAudioChecks.writeFully(ByteBuffer.allocate(0),
                data -> { throw new AssertionError("must not write"); }, () -> true, deadline()) == 0,
                "Empty packets must not write");
        expect(IOException.class, () -> EncodedAudioChecks.writeFully(
                ByteBuffer.allocate(4), data -> 0, () -> true,
                System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(30)));
        Thread.currentThread().interrupt();
        try {
            expect(InterruptedException.class, () -> EncodedAudioChecks.writeFully(
                    ByteBuffer.allocate(4), data -> 0, () -> true, deadline()));
        } finally {
            Thread.interrupted();
        }
    }

    private static void testPresentationEnd() throws Exception {
        EncodedAudioChecks.awaitPresentationEnd(new CountDownLatch(0), () -> true, deadline());
        checks++;
        expect(IOException.class, () -> EncodedAudioChecks.awaitPresentationEnd(
                new CountDownLatch(0), () -> false, deadline()));
        expect(IOException.class, () -> EncodedAudioChecks.awaitPresentationEnd(
                new CountDownLatch(0), () -> true, System.nanoTime() - 1));
        expect(IOException.class, () -> EncodedAudioChecks.awaitPresentationEnd(
                new CountDownLatch(1), () -> true,
                System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(30)));
        CountDownLatch ended = new CountDownLatch(1);
        AtomicInteger checksBeforeEnd = new AtomicInteger();
        EncodedAudioChecks.awaitPresentationEnd(ended, () -> {
            if (checksBeforeEnd.incrementAndGet() == 2) ended.countDown();
            return true;
        }, deadline());
        require(checksBeforeEnd.get() >= 3, "Validate output before and after drain completion");
        Thread.currentThread().interrupt();
        try {
            expect(InterruptedException.class, () -> EncodedAudioChecks.awaitPresentationEnd(
                    new CountDownLatch(1), () -> true, deadline()));
        } finally {
            Thread.interrupted();
        }
    }

    private static long deadline() {
        return System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        checks++;
    }

    private static void expect(Class<? extends Exception> type, CheckedRunnable action) throws Exception {
        try {
            action.run();
        } catch (Exception e) {
            if (!type.isInstance(e)) throw e;
            checks++;
            return;
        }
        throw new AssertionError("Expected " + type.getSimpleName());
    }

    private interface CheckedRunnable {
        void run() throws Exception;
    }
}
