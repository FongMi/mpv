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

static void test_data_uri(const char *uri, const char *expected)
{
    struct stream *s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, uri);
    const char *filename = s->url;

    assert_true(normalize_data_uri_for_lavf(s, s, &filename));
    assert_string_equal(filename, expected);
    assert_string_equal(s->url, expected);
    assert_string_equal(s->mime_type, "text/plain");
    talloc_free(s);
}

static void test_invalid_data_uri(const char *uri)
{
    struct stream *s = talloc_zero(NULL, struct stream);
    s->url = talloc_strdup(s, uri);
    const char *filename = s->url;

    assert_false(normalize_data_uri_for_lavf(s, s, &filename));
    assert_string_equal(s->url, uri);
    assert_true(s->mime_type == NULL);
    talloc_free(s);
}

static void test_data_uri_url_limit(void)
{
    void *parent = talloc_new(NULL);
    bstr payload = bstr0("SGU=");
    bstr decoded = {0};

    assert_false(decode_data_uri_url(parent, payload, 3, &decoded));
    assert_true(decoded.start == NULL);
    assert_true(decode_data_uri_url(parent, payload, 4, &decoded));
    assert_true(decoded.len == 4);
    assert_string_equal((char *)decoded.start, "SGU=");
    assert_true(decode_data_uri_base64(parent, decoded, &decoded));
    assert_true(decoded.len == 2);
    assert_true(!memcmp(decoded.start, "He", 2));

    payload = bstr0("SGU%3D");
    assert_false(decode_data_uri_url(parent, payload, 5, &decoded));
    assert_true(decode_data_uri_url(parent, payload, 6, &decoded));
    assert_true(decoded.len == 4);
    assert_string_equal((char *)decoded.start, "SGU=");

    payload.len = (size_t)INT_MAX + 1;
    assert_false(decode_data_uri_url(parent, payload, INT_MAX, &decoded));
    talloc_free(parent);
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
    test_data_uri_url_limit();
    test_data_uri("data:;base64,SGVsbG8=", "data:text/plain;base64,SGVsbG8=");
    test_data_uri("data:;base64,SGVsbG8%3D", "data:text/plain;base64,SGVsbG8=");
    test_data_uri("data:;base64,%2Fw%3d%3d", "data:text/plain;base64,/w==");
    test_data_uri("data:;base64,SGVs%20bG8%3D", "data:text/plain;base64,SGVsbG8=");
    test_data_uri("data:,Hello%00", "data:text/plain;base64,SGVsbG8A");
    test_data_uri("data://,Hello", "data:text/plain;base64,SGVsbG8=");
    test_invalid_data_uri("data:;base64,SGVsbG8%00=");
    test_invalid_data_uri("data:;base64,SGVsbG8%3");
    test_invalid_data_uri("data:;base64,SGVsbG8%GG");
    test_invalid_data_uri("data:;base64,SGVs!bG8=");
    return 0;
}
