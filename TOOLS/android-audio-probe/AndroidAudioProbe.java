/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv. If not, see <http://www.gnu.org/licenses/>.
 */

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;
import org.json.JSONObject;

/** Read-only preflight for an experimental raw AAC/MP3 AudioTrack path. */
public final class AndroidAudioProbe {
    private AndroidAudioProbe() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 0) {
            System.err.println("Usage: AndroidAudioProbe (query only; no arguments)");
            System.exit(2);
        }
        System.out.println(new JSONObject()
                .put("sdk", Build.VERSION.SDK_INT)
                .put("model", Build.MODEL)
                .put("scope", "capability query only; no playback or output-track creation"));
        if (Build.VERSION.SDK_INT < 29) {
            System.out.println(new JSONObject().put("status", "public capability query unavailable"));
            return;
        }
        probe(AudioAttributes.CONTENT_TYPE_MOVIE);
        probe(AudioAttributes.CONTENT_TYPE_MUSIC);
    }

    private static void probe(int contentType) throws Exception {
        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(contentType)
                .build();
        int[] encodings = {AudioFormat.ENCODING_AAC_LC, AudioFormat.ENCODING_MP3};
        int[] rates = {44100, 48000};
        int[] masks = {AudioFormat.CHANNEL_OUT_MONO, AudioFormat.CHANNEL_OUT_STEREO};
        for (int encoding : encodings) {
            for (int rate : rates) {
                for (int mask : masks) {
                    AudioFormat format = new AudioFormat.Builder().setEncoding(encoding)
                            .setSampleRate(rate).setChannelMask(mask).build();
                    JSONObject result = new JSONObject()
                            .put("contentType", contentType == AudioAttributes.CONTENT_TYPE_MOVIE ? "movie" : "music")
                            .put("encoding", encoding == AudioFormat.ENCODING_AAC_LC ? "aac-lc" : "mp3")
                            .put("rate", rate).put("channels", format.getChannelCount());
                    try {
                        if (Build.VERSION.SDK_INT >= 33) {
                            int support = AudioManager.getDirectPlaybackSupport(format, attributes);
                            result.put("bitstream", (support & AudioManager.DIRECT_PLAYBACK_BITSTREAM_SUPPORTED) != 0)
                                    .put("offload", (support & AudioManager.DIRECT_PLAYBACK_OFFLOAD_SUPPORTED) != 0);
                        } else {
                            // The older query combines direct and offloaded support. A true result
                            // alone must not enable a non-offloaded compressed output track.
                            result.put("directOrOffload", AudioTrack.isDirectPlaybackSupported(format, attributes))
                                    .put("offload", AudioManager.isOffloadedPlaybackSupported(format, attributes))
                                    .put("bitstream", JSONObject.NULL);
                        }
                    } catch (RuntimeException e) {
                        result.put("error", e.toString());
                    }
                    System.out.println(result);
                }
            }
        }
    }
}
