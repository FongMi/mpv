/*
 * JNI utility functions
 *
 * Copyright (c) 2015-2016 Matthieu Bouron <matthieu.bouron stupeflix.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libavcodec/jni.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "jni.h"
#include "mpv_talloc.h"
#include "osdep/threads.h"

enum android_video_profile {
    ANDROID_HEVC_MAIN10 = 0x02,
    ANDROID_DVHE_DTB = 0x80,
    ANDROID_DVHE_ST = 0x100,
};

static bool jni_dovi_mime_matches(const char *codec_name, const char *mime,
                                  bool has_canonical_type)
{
    if (!strcasecmp(mime, "video/dolby-vision"))
        return true;
    if (has_canonical_type)
        return false;
    if (!strcmp(codec_name, "OMX.MS.HEVCDV.Decoder"))
        return !strcasecmp(mime, "video/hevcdv");
    if (!strcmp(codec_name, "OMX.RTK.video.decoder") ||
        !strcmp(codec_name, "OMX.realtek.video.decoder.tunneled"))
        return !strcasecmp(mime, "video/dv_hevc");
    return false;
}

static JavaVM *java_vm;
static pthread_key_t current_env;
static mp_once once = MP_STATIC_ONCE_INITIALIZER;
static mp_static_mutex lock = MP_STATIC_MUTEX_INITIALIZER;

static void jni_detach_env(void *data)
{
    if (java_vm) {
        (*java_vm)->DetachCurrentThread(java_vm);
    }
}

static void jni_create_pthread_key(void)
{
    pthread_key_create(&current_env, jni_detach_env);
}

JNIEnv *mp_jni_get_env(struct mp_log *log)
{
    JNIEnv *env = NULL;

    mp_mutex_lock(&lock);
    if (!java_vm)
        java_vm = av_jni_get_java_vm(NULL);

    if (!java_vm) {
        mp_err(log, "No Java virtual machine has been registered\n");
        goto done;
    }

    mp_exec_once(&once, jni_create_pthread_key);

    if ((env = pthread_getspecific(current_env)) != NULL)
        goto done;

    int ret = (*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6);
    switch(ret) {
    case JNI_EDETACHED:
        if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != 0) {
            mp_err(log, "Failed to attach the JNI environment to the current thread\n");
            env = NULL;
        } else {
            pthread_setspecific(current_env, env);
        }
        break;
    case JNI_OK:
        break;
    case JNI_EVERSION:
        mp_err(log, "The specified JNI version is not supported\n");
        break;
    default:
        mp_err(log, "Failed to get the JNI environment attached to this thread\n");
        break;
    }

done:
    mp_mutex_unlock(&lock);
    return env;
}

static bool jni_video_format_supported(JNIEnv *env, jclass format_class,
                                       jmethodID create, jmethodID set_int,
                                       jmethodID set_float, jmethodID supports,
                                       jobject caps, jstring mime, int profile,
                                       int width, int height, double fps,
                                       int sdk, struct mp_log *log)
{
    jobject format = (*env)->CallStaticObjectMethod(env, format_class, create,
                                                    mime, width, height);
    if (mp_jni_exception_check(env, 1, log) < 0 || !format)
        return false;

    jstring profile_key = (*env)->NewStringUTF(env, "profile");
    if (mp_jni_exception_check(env, 1, log) < 0 || !profile_key) {
        (*env)->DeleteLocalRef(env, format);
        return false;
    }
    jstring rate_key = (*env)->NewStringUTF(env, "frame-rate");
    bool supported = false;
    if (mp_jni_exception_check(env, 1, log) == 0 && rate_key) {
        (*env)->CallVoidMethod(env, format, set_int, profile_key, profile);
        if (!(*env)->ExceptionCheck(env) && sdk >= 22 &&
            isfinite(fps) && fps >= 1)
            (*env)->CallVoidMethod(env, format, set_float, rate_key, (jfloat)fps);
    }
    if (rate_key && mp_jni_exception_check(env, 1, log) == 0) {
        supported = (*env)->CallBooleanMethod(env, caps, supports, format);
        if (mp_jni_exception_check(env, 1, log) < 0)
            supported = false;
    }
    (*env)->DeleteLocalRef(env, profile_key);
    if (rate_key)
        (*env)->DeleteLocalRef(env, rate_key);
    (*env)->DeleteLocalRef(env, format);
    return supported;
}

int mp_jni_dovi_decoder_support(struct mp_log *log, int width, int height,
                                double fps)
{
    if (width <= 0 || height <= 0)
        return 0;

    JNIEnv *env = mp_jni_get_env(log);
    if (!env)
        return 0;
    if ((*env)->PushLocalFrame(env, 32) < 0) {
        mp_jni_exception_check(env, 1, log);
        return 0;
    }

    int supported = 0;
    bool query_failed = false;
    int error;
    jclass list_class = (*env)->FindClass(env, "android/media/MediaCodecList");
    jclass info_class = (*env)->FindClass(env, "android/media/MediaCodecInfo");
    jclass caps_class = (*env)->FindClass(env, "android/media/MediaCodecInfo$CodecCapabilities");
    jclass level_class = (*env)->FindClass(env, "android/media/MediaCodecInfo$CodecProfileLevel");
    jclass format_class = (*env)->FindClass(env, "android/media/MediaFormat");
    jclass version_class = (*env)->FindClass(env, "android/os/Build$VERSION");
    if (!list_class || !info_class || !caps_class || !level_class || !format_class ||
        !version_class || (*env)->ExceptionCheck(env))
        goto done;

    jfieldID sdk_field = (*env)->GetStaticFieldID(env, version_class,
                                                 "SDK_INT", "I");
    jint sdk = sdk_field ? (*env)->GetStaticIntField(env, version_class,
                                                     sdk_field) : 0;
    jmethodID count = (*env)->GetStaticMethodID(env, list_class,
                                               "getCodecCount", "()I");
    jmethodID info_at = (*env)->GetStaticMethodID(env, list_class,
        "getCodecInfoAt", "(I)Landroid/media/MediaCodecInfo;");
    jmethodID is_encoder = (*env)->GetMethodID(env, info_class, "isEncoder", "()Z");
    jmethodID hardware = sdk >= 29
        ? (*env)->GetMethodID(env, info_class, "isHardwareAccelerated", "()Z")
        : NULL;
    jmethodID software_only = sdk >= 29
        ? (*env)->GetMethodID(env, info_class, "isSoftwareOnly", "()Z")
        : NULL;
    jmethodID name = (*env)->GetMethodID(env, info_class, "getName", "()Ljava/lang/String;");
    jmethodID types = (*env)->GetMethodID(env, info_class,
                                         "getSupportedTypes", "()[Ljava/lang/String;");
    jmethodID caps_for_type = (*env)->GetMethodID(env, info_class,
        "getCapabilitiesForType",
        "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
    jmethodID create = (*env)->GetStaticMethodID(env, format_class,
        "createVideoFormat", "(Ljava/lang/String;II)Landroid/media/MediaFormat;");
    jmethodID set_int = (*env)->GetMethodID(env, format_class,
                                           "setInteger", "(Ljava/lang/String;I)V");
    jmethodID set_float = (*env)->GetMethodID(env, format_class,
                                             "setFloat", "(Ljava/lang/String;F)V");
    jmethodID supports = (*env)->GetMethodID(env, caps_class,
        "isFormatSupported", "(Landroid/media/MediaFormat;)Z");
    jfieldID levels_field = (*env)->GetFieldID(env, caps_class,
        "profileLevels", "[Landroid/media/MediaCodecInfo$CodecProfileLevel;");
    jfieldID profile_field = (*env)->GetFieldID(env, level_class, "profile", "I");
    if (!sdk_field || !count || !info_at || !is_encoder ||
        (sdk >= 29 && (!hardware || !software_only)) ||
        !name || !types || !caps_for_type ||
        !create || !set_int || !set_float || !supports || !levels_field ||
        !profile_field ||
        (*env)->ExceptionCheck(env))
        goto done;

    jint codec_count = (*env)->CallStaticIntMethod(env, list_class, count);
    const int all_formats = MP_ANDROID_DOVI_P7 | MP_ANDROID_DOVI_P81 |
                            MP_ANDROID_HEVC_MAIN10;
    // Match FFmpeg's ff_AMediaCodecList_getCodecNameByType selection order.
    // It selects the first decoder advertising each MIME/profile pair.
    // Later decoders cannot rescue a format the selected decoder cannot handle.
    int selected = 0;
    for (jint i = 0; i < codec_count && selected != all_formats; i++) {
        if ((*env)->PushLocalFrame(env, 16) < 0) {
            mp_jni_exception_check(env, 1, log);
            query_failed = true;
            break;
        }
        jstring codec_name = NULL;
        const char *value = NULL;
        jobject info = (*env)->CallStaticObjectMethod(env, list_class, info_at, i);
        if (!info) {
            query_failed = true;
            goto next_codec;
        }
        bool encoder = (*env)->CallBooleanMethod(env, info, is_encoder);
        if ((*env)->ExceptionCheck(env) || encoder)
            goto next_codec;
        if (sdk >= 29) {
            bool software = (*env)->CallBooleanMethod(env, info, software_only);
            if ((*env)->ExceptionCheck(env) || software)
                goto next_codec;
        }

        codec_name = (*env)->CallObjectMethod(env, info, name);
        value = codec_name
            ? (*env)->GetStringUTFChars(env, codec_name, NULL) : NULL;
        if (!value) {
            query_failed = true;
            goto next_codec;
        }
        bool excluded = strstr(value, "OMX.google") ||
                        strstr(value, "OMX.ffmpeg") ||
                        (strstr(value, "OMX.SEC") && strstr(value, ".sw.")) ||
                        !strcmp(value, "OMX.qcom.video.decoder.hevcswvdec");
        if (excluded)
            goto next_codec;
        bool accelerated = sdk >= 29
            ? (*env)->CallBooleanMethod(env, info, hardware)
            : strncasecmp(value, "c2.android.", 11) &&
              strncasecmp(value, "c2.google.", 10);
        if ((*env)->ExceptionCheck(env))
            goto next_codec;

        jobjectArray codec_types = (*env)->CallObjectMethod(env, info, types);
        if (!codec_types) {
            query_failed = true;
            goto next_codec;
        }
        bool canonical_dovi = false;
        for (jsize j = 0; j < (*env)->GetArrayLength(env, codec_types); j++) {
            jstring type = (*env)->GetObjectArrayElement(env, codec_types, j);
            if (!type) {
                query_failed = true;
                goto next_codec;
            }
            const char *mime = (*env)->GetStringUTFChars(env, type, NULL);
            if (!mime) {
                (*env)->DeleteLocalRef(env, type);
                query_failed = true;
                goto next_codec;
            }
            canonical_dovi = !strcasecmp(mime, "video/dolby-vision");
            (*env)->ReleaseStringUTFChars(env, type, mime);
            (*env)->DeleteLocalRef(env, type);
            if (canonical_dovi)
                break;
        }
        if ((*env)->ExceptionCheck(env))
            goto next_codec;
        for (jsize j = 0; j < (*env)->GetArrayLength(env, codec_types); j++) {
            jstring type = (*env)->GetObjectArrayElement(env, codec_types, j);
            if (!type) {
                query_failed = true;
                goto next_codec;
            }
            const char *mime = (*env)->GetStringUTFChars(env, type, NULL);
            if (!mime) {
                query_failed = true;
                goto next_codec;
            }
            bool dovi = jni_dovi_mime_matches(value, mime, canonical_dovi);
            bool hevc = !strcasecmp(mime, "video/hevc");
            (*env)->ReleaseStringUTFChars(env, type, mime);
            if ((dovi && (selected & (MP_ANDROID_DOVI_P7 |
                                      MP_ANDROID_DOVI_P81)) !=
                         (MP_ANDROID_DOVI_P7 | MP_ANDROID_DOVI_P81)) ||
                (hevc && !(selected & MP_ANDROID_HEVC_MAIN10))) {
                jobject caps = (*env)->CallObjectMethod(env, info, caps_for_type,
                                                        type);
                if (mp_jni_exception_check(env, 1, log) < 0) {
                    query_failed = true;
                    goto next_codec;
                }
                jobjectArray levels = caps
                    ? (*env)->GetObjectField(env, caps, levels_field) : NULL;
                if (mp_jni_exception_check(env, 1, log) < 0 || !levels) {
                    query_failed = true;
                    goto next_codec;
                }
                bool no_profiles = !(*env)->GetArrayLength(env, levels);
                bool p7 = no_profiles, p81 = no_profiles, main10 = no_profiles;
                for (jsize k = 0; k < (*env)->GetArrayLength(env, levels); k++) {
                    jobject level = (*env)->GetObjectArrayElement(env, levels, k);
                    if (!level) {
                        query_failed = true;
                        goto next_codec;
                    }
                    jint profile = (*env)->GetIntField(env, level, profile_field);
                    if ((*env)->ExceptionCheck(env))
                        goto next_codec;
                    p7 |= profile == ANDROID_DVHE_DTB;
                    p81 |= profile == ANDROID_DVHE_ST;
                    main10 |= profile == ANDROID_HEVC_MAIN10;
                    (*env)->DeleteLocalRef(env, level);
                }
                if (mp_jni_exception_check(env, 1, log) < 0) {
                    query_failed = true;
                    goto next_codec;
                }
                if (caps && dovi) {
                    if (p7 && !(selected & MP_ANDROID_DOVI_P7)) {
                        selected |= MP_ANDROID_DOVI_P7;
                        if (accelerated && !no_profiles &&
                            jni_video_format_supported(env,
                            format_class, create,
                            set_int, set_float, supports, caps, type,
                            ANDROID_DVHE_DTB,
                            width, height, fps, sdk, log))
                            supported |= MP_ANDROID_DOVI_P7;
                    }
                    if (p81 && !(selected & MP_ANDROID_DOVI_P81)) {
                        selected |= MP_ANDROID_DOVI_P81;
                        if (accelerated && !no_profiles &&
                            jni_video_format_supported(env,
                            format_class, create,
                            set_int, set_float, supports, caps, type,
                            ANDROID_DVHE_ST,
                            width, height, fps, sdk, log))
                            supported |= MP_ANDROID_DOVI_P81;
                    }
                } else if (caps && hevc && main10 &&
                           !(selected & MP_ANDROID_HEVC_MAIN10)) {
                    selected |= MP_ANDROID_HEVC_MAIN10;
                    if (accelerated && !no_profiles &&
                        jni_video_format_supported(env,
                                format_class, create,
                                set_int, set_float, supports, caps, type,
                                ANDROID_HEVC_MAIN10,
                                width, height, fps, sdk, log))
                        supported |= MP_ANDROID_HEVC_MAIN10;
                }
                if (levels)
                    (*env)->DeleteLocalRef(env, levels);
                if (caps)
                    (*env)->DeleteLocalRef(env, caps);
            }
            (*env)->DeleteLocalRef(env, type);
            if ((*env)->ExceptionCheck(env))
                break;
        }

next_codec:
        if (value)
            (*env)->ReleaseStringUTFChars(env, codec_name, value);
        (*env)->PopLocalFrame(env, NULL);
        if (query_failed || (*env)->ExceptionCheck(env))
            break;
    }

done:
    error = mp_jni_exception_check(env, 1, log);
    if (query_failed && error == 0)
        mp_warn(log, "Failed to inspect Android MediaCodec capabilities.\n");
    if (query_failed || error < 0)
        supported = 0;
    (*env)->PopLocalFrame(env, NULL);
    return supported;
}

char *mp_jni_jstring_to_utf_chars(JNIEnv *env, jstring string, struct mp_log *log)
{
    if (!string)
        return NULL;

    const char *utf_chars = (*env)->GetStringUTFChars(env, string, NULL);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "getStringUTFChars() threw an exception\n");
        return NULL;
    }

    char *ret = talloc_strdup(NULL, utf_chars);

    (*env)->ReleaseStringUTFChars(env, string, utf_chars);

    return ret;
}

jstring mp_jni_utf_chars_to_jstring(JNIEnv *env, const char *utf_chars,
                                    struct mp_log *log)
{
    jstring ret = (*env)->NewStringUTF(env, utf_chars);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "NewStringUTF() threw an exception\n");
        return NULL;
    }

    return ret;
}

int mp_jni_exception_get_summary(JNIEnv *env, jthrowable exception,
                                 char **error, struct mp_log *log)
{
    int ret = 0;

    char *name = NULL;
    char *message = NULL;

    jclass class_class = NULL;
    jclass exception_class = NULL;
    jstring string = NULL;

    *error = NULL;

    exception_class = (*env)->GetObjectClass(env, exception);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find Throwable class\n");
        ret = -1;
        goto done;
    }

    class_class = (*env)->GetObjectClass(env, exception_class);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find Throwable class's class\n");
        ret = -1;
        goto done;
    }

    jmethodID get_name_id = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find method Class.getName()\n");
        ret = -1;
        goto done;
    }

    string = (*env)->CallObjectMethod(env, exception_class, get_name_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Class.getName() threw an exception\n");
        ret = -1;
        goto done;
    }

    if (string) {
        name = mp_jni_jstring_to_utf_chars(env, string, log);
        MP_JNI_LOCAL_FREEP(&string);
    }

    jmethodID get_message_id = (*env)->GetMethodID(env, exception_class, "getMessage", "()Ljava/lang/String;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Could not find method Throwable.getMessage()\n");
        ret = -1;
        goto done;
    }

    string = (*env)->CallObjectMethod(env, exception, get_message_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mp_err(log, "Throwable.getMessage() threw an exception\n");
        ret = -1;
        goto done;
    }

    if (string) {
        message = mp_jni_jstring_to_utf_chars(env, string, log);
        MP_JNI_LOCAL_FREEP(&string);
    }

    if (name && message) {
        *error = talloc_asprintf(NULL, "%s: %s", name, message);
    } else if (name && !message) {
        *error = talloc_asprintf(NULL, "%s occurred", name);
    } else if (!name && message) {
        *error = talloc_asprintf(NULL, "Exception: %s", message);
    } else {
        mp_warn(log, "Could not retrieve exception name and message\n");
        *error = talloc_strdup(NULL, "Exception occurred");
    }

done:

    talloc_free(name);
    talloc_free(message);

    MP_JNI_LOCAL_FREEP(&class_class);
    MP_JNI_LOCAL_FREEP(&exception_class);
    MP_JNI_LOCAL_FREEP(&string);

    return ret;
}

int mp_jni_exception_check(JNIEnv *env, int logging, struct mp_log *log)
{
    if (!(*env)->ExceptionCheck(env))
        return 0;

    if (!logging) {
        (*env)->ExceptionClear(env);
        return -1;
    }

    jthrowable exception = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);

    char *message = NULL;
    int ret = mp_jni_exception_get_summary(env, exception, &message, log);
    MP_JNI_LOCAL_FREEP(&exception);
    if (ret < 0)
        return ret;

    mp_err(log, "%s\n", message);
    talloc_free(message);
    return -1;
}

#define CHECK_EXC_MANDATORY() do { \
        if ((ret = mp_jni_exception_check(env, mandatory, log)) < 0 && \
             mandatory) { \
            goto done; \
        } \
    } while (0)

int mp_jni_init_jfields(JNIEnv *env, void *jfields,
                        const struct MPJniField *jfields_mapping,
                        int global, struct mp_log *log)
{
    int ret = 0;
    jclass last_clazz = NULL;

    for (int i = 0; jfields_mapping[i].name; i++) {
        bool mandatory = !!jfields_mapping[i].mandatory;
        enum MPJniFieldType type = jfields_mapping[i].type;

        void *jfield = (uint8_t*)jfields + jfields_mapping[i].offset;

        if (type == MP_JNI_CLASS) {
            last_clazz = NULL;

            jclass clazz = (*env)->FindClass(env, jfields_mapping[i].name);
            CHECK_EXC_MANDATORY();

            last_clazz = *(jclass*)jfield =
                    global ? (*env)->NewGlobalRef(env, clazz) : clazz;

            if (global)
                MP_JNI_LOCAL_FREEP(&clazz);

            continue;
        }

        if (!last_clazz) {
            ret = -1;
            break;
        }

        switch (type) {
        case MP_JNI_FIELD: {
            jfieldID field_id = (*env)->GetFieldID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jfieldID*)jfield = field_id;
            break;
        }
        case MP_JNI_STATIC_FIELD_AS_INT:
        case MP_JNI_STATIC_FIELD: {
            jfieldID field_id = (*env)->GetStaticFieldID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            if (type == MP_JNI_STATIC_FIELD_AS_INT) {
                if (field_id) {
                    jint value = (*env)->GetStaticIntField(env, last_clazz, field_id);
                    CHECK_EXC_MANDATORY();
                    *(jint*)jfield = value;
                }
            } else {
                *(jfieldID*)jfield = field_id;
            }
            break;
        }
        case MP_JNI_METHOD: {
            jmethodID method_id = (*env)->GetMethodID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jmethodID*)jfield = method_id;
            break;
        }
        case MP_JNI_STATIC_METHOD: {
            jmethodID method_id = (*env)->GetStaticMethodID(env, last_clazz,
                jfields_mapping[i].name, jfields_mapping[i].signature);
            CHECK_EXC_MANDATORY();

            *(jmethodID*)jfield = method_id;
            break;
        }
        default:
            mp_err(log, "Unknown JNI field type\n");
            ret = -1;
            goto done;
        }

        ret = 0;
    }

done:
    if (ret < 0) {
        /* reset jfields in case of failure so it does not leak references */
        mp_jni_reset_jfields(env, jfields, jfields_mapping, global, log);
    }

    return ret;
}

#undef CHECK_EXC_MANDATORY

int mp_jni_reset_jfields(JNIEnv *env, void *jfields,
                         const struct MPJniField *jfields_mapping,
                         int global, struct mp_log *log)
{
    for (int i = 0; jfields_mapping[i].name; i++) {
        enum MPJniFieldType type = jfields_mapping[i].type;

        void *jfield = (uint8_t*)jfields + jfields_mapping[i].offset;

        switch (type) {
        case MP_JNI_CLASS: {
            jclass clazz = *(jclass*)jfield;
            if (!clazz)
                continue;

            if (global) {
                MP_JNI_GLOBAL_FREEP(&clazz);
            } else {
                MP_JNI_LOCAL_FREEP(&clazz);
            }

            *(jclass*)jfield = NULL;
            break;
        }
        case MP_JNI_FIELD:
        case MP_JNI_STATIC_FIELD:
            *(jfieldID*)jfield = NULL;
            break;
        case MP_JNI_STATIC_FIELD_AS_INT:
            *(jint*)jfield = 0;
            break;
        case MP_JNI_METHOD:
        case MP_JNI_STATIC_METHOD:
            *(jmethodID*)jfield = NULL;
            break;
        default:
            mp_err(log, "Unknown JNI field type\n");
        }
    }

    return 0;
}
