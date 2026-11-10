#include <libavutil/dict.h>
#include <libavutil/error.h>

#include "demux/demux.h"
#include "misc/thread_tools.h"
#include "osdep/timer.h"
#include "player/core.h"
#include "stream/stream.h"
#include "stream/stream_curl.h"

int main(int argc, char **argv)
{
    mp_require(argc == 3);
    const int range_size = 128 * 1024;
    bool bounded = strcmp(argv[2], "bounded") == 0;
    char *options[] = {"--no-config", "--idle=yes", "--load-scripts=no",
                       "--terminal=no", "--curl-enabled=yes",
                       "--curl-http-version=1.1",
                       bounded ? "--curl-max-request-size=256KiB" : NULL,
                       NULL};
    struct MPContext *mpctx = mp_create();
    mp_require(mpctx);
    mpctx->is_cli = true;
    mp_require(mp_initialize(mpctx, options) == 0);

    struct demuxer demuxer = {
        .global = mpctx->global,
        .log = mpctx->log,
        .cancel = mp_cancel_new(mpctx),
        .stream_origin = STREAM_ORIGIN_NET,
    };
    AVDictionary *opts = NULL;
    AVIOContext *pb = NULL;
    void *cookie = NULL;
    uint8_t *buf = talloc_size(mpctx, range_size);
    mp_require(av_dict_set_int(&opts, "initial_request_size", range_size, 0) == 0);
    mp_require(mp_curl_avio_open(&demuxer, &pb, &cookie, argv[1], AVIO_FLAG_READ,
                                 &opts, NULL, NULL) == 0);
    mp_require(!opts && avio_read(pb, buf, range_size) == range_size);
    mp_require(buf[0] == 0 && buf[range_size - 1] == 0);

    // Give the curl thread time to finish the response while the reader is idle.
    mp_sleep_ns(MP_TIME_MS_TO_NS(100));
    if (strcmp(argv[2], "cancel") == 0) {
        mp_cancel_trigger(demuxer.cancel);
        mp_require(avio_read(pb, buf, 1) == AVERROR_EOF);
    } else if (bounded) {
        for (int block = 1; block < 4; block++) {
            mp_require(avio_read(pb, buf, range_size) == range_size);
            mp_require(buf[0] == block && buf[range_size - 1] == block);
        }
        mp_require(avio_read(pb, buf, 1) == AVERROR_EOF);
    } else {
        bool seek = strcmp(argv[2], "seek") == 0;
        if (seek) {
            int64_t pos = 3 * range_size;
            mp_require(avio_seek(pb, pos, SEEK_SET) == pos);
        }
        mp_require(avio_read(pb, buf, 1) == 1);
        mp_require(buf[0] == (seek ? 3 : 1));
    }

    mp_curl_avio_close(pb, cookie);
    mp_destroy(mpctx);
    return 0;
}
