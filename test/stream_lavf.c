#include "test_utils.h"
#include "stream/stream_lavf.c"

struct avio_info {
    const AVClass *av_class;
    const char *location;
    const char *mime_type;
};

static const AVClass avio_info_class = {
    .class_name = "test_avio_info",
    .version = LIBAVUTIL_VERSION_INT,
    .option = (const AVOption[]) {
        {"location", NULL, offsetof(struct avio_info, location),
         AV_OPT_TYPE_STRING},
        {"mime_type", NULL, offsetof(struct avio_info, mime_type),
         AV_OPT_TYPE_STRING},
        {0}
    },
};

static void *avio_info_child_next(void *obj, void *prev)
{
    AVIOContext *avio = obj;
    return prev ? NULL : avio->opaque;
}

static const AVClass avio_class = {
    .class_name = "test_avio",
    .version = LIBAVUTIL_VERSION_INT,
    .child_next = avio_info_child_next,
};

static void test_avio_info(const char *location, const char *expected)
{
    struct stream *s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, "http://localhost/proxy?channel=1");
    struct avio_info info = {
        .av_class = &avio_info_class,
        .location = location,
        .mime_type = "application/vnd.apple.mpegurl",
    };
    AVIOContext avio = {
        .av_class = &avio_class,
        .opaque = &info,
    };

    read_avio_info(s, &avio);
    assert_string_equal(s->url, expected);
    assert_string_equal(s->mime_type, info.mime_type);
    assert_true((const void *)s->url != location);
    assert_true(s->mime_type != info.mime_type);
    talloc_free(s);
}

static void test_avio_without_info(void)
{
    struct stream *s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, "file:///master.m3u8");
    AVIOContext avio = {0};

    read_avio_info(s, &avio);
    assert_string_equal(s->url, "file:///master.m3u8");
    assert_true(s->mime_type == NULL);

    avio.av_class = &avio_class;
    read_avio_info(s, &avio);
    assert_string_equal(s->url, "file:///master.m3u8");
    assert_true(s->mime_type == NULL);
    talloc_free(s);
}

int main(void)
{
    test_avio_info("https://cdn.example/live/master.m3u8?token=1",
                   "https://cdn.example/live/master.m3u8?token=1");
    test_avio_info("http://localhost/proxy?channel=1",
                   "http://localhost/proxy?channel=1");
    test_avio_info(NULL, "http://localhost/proxy?channel=1");
    test_avio_info("", "http://localhost/proxy?channel=1");
    test_avio_without_info();
    return 0;
}
