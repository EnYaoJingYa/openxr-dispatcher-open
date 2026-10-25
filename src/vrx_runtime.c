/* SPDX-License-Identifier: MIT
 *
 * ThinkReality VRX runtime discovery.
 *
 * Snapdragon Spaces exposes its runtime through a vendor-specific provider:
 * content://com.qualcomm.qti.openxrruntime_broker/runtime
 *
 * The standard Khronos Android loader expects a different authority and URI
 * shape, so it reports XR_ERROR_RUNTIME_UNAVAILABLE on VRX. Querying the
 * provider returns the native library directory and libopenxr_monado.so. The
 * dispatcher can load that runtime directly and keep forwarding all calls
 * through its xrGetInstanceProcAddr.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "dispatch.h"

#ifdef __ANDROID__
#include <jni.h>

static void clear_exception(JNIEnv *env, const char *where)
{
    if ((*env)->ExceptionCheck(env)) {
        xd_log(XD_WARN, "VRX provider %s raised a JNI exception", where);
        (*env)->ExceptionClear(env);
    }
}

static int string_column(JNIEnv *env, jobject cursor, const char *column, char *out, size_t size)
{
    jclass cls = (*env)->GetObjectClass(env, cursor);
    if (!cls)
        return 0;
    jmethodID index_method = (*env)->GetMethodID(env, cls, "getColumnIndex", "(Ljava/lang/String;)I");
    jmethodID get_method = (*env)->GetMethodID(env, cls, "getString", "(I)Ljava/lang/String;");
    if (!index_method || !get_method) {
        clear_exception(env, "cursor method");
        (*env)->DeleteLocalRef(env, cls);
        return 0;
    }
    jstring name = (*env)->NewStringUTF(env, column);
    jint index = (*env)->CallIntMethod(env, cursor, index_method, name);
    (*env)->DeleteLocalRef(env, name);
    if ((*env)->ExceptionCheck(env) || index < 0) {
        clear_exception(env, "getColumnIndex");
        (*env)->DeleteLocalRef(env, cls);
        return 0;
    }
    jstring value = (jstring)(*env)->CallObjectMethod(env, cursor, get_method, index);
    if ((*env)->ExceptionCheck(env) || !value) {
        clear_exception(env, "getString");
        (*env)->DeleteLocalRef(env, cls);
        return 0;
    }
    const char *utf = (*env)->GetStringUTFChars(env, value, NULL);
    int copied = 0;
    if (utf && size) {
        snprintf(out, size, "%s", utf);
        copied = 1;
    }
    if (utf)
        (*env)->ReleaseStringUTFChars(env, value, utf);
    (*env)->DeleteLocalRef(env, value);
    (*env)->DeleteLocalRef(env, cls);
    return copied;
}

bool xd_load_vrx_runtime(void)
{
    JavaVM *vm = xd_java_vm;
    jobject context = xd_activity;
    if (!vm || !context)
        return false;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED)
        (*vm)->AttachCurrentThread(vm, &env, NULL);
    if (!env)
        return false;

    jclass context_class = (*env)->FindClass(env, "android/content/Context");
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!context_class || !uri_class || !string_class) {
        clear_exception(env, "system class lookup");
        return false;
    }
    jmethodID get_resolver = (*env)->GetMethodID(env, context_class, "getContentResolver",
                                                 "()Landroid/content/ContentResolver;");
    jmethodID parse_uri = (*env)->GetStaticMethodID(
        env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (!get_resolver || !parse_uri) {
        clear_exception(env, "context/URI method");
        return false;
    }

    jstring uri_text = (*env)->NewStringUTF(
        env, "content://com.qualcomm.qti.openxrruntime_broker/runtime");
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse_uri, uri_text);
    jobject resolver = (*env)->CallObjectMethod(env, context, get_resolver);
    if ((*env)->ExceptionCheck(env) || !uri || !resolver) {
        clear_exception(env, "URI/resolver");
        return false;
    }

    jclass resolver_class = (*env)->GetObjectClass(env, resolver);
    jmethodID query = (*env)->GetMethodID(
        env, resolver_class, "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;"
        "Ljava/lang/String;)Landroid/database/Cursor;");
    if (!query) {
        clear_exception(env, "ContentResolver.query");
        return false;
    }
    jobjectArray projection = (*env)->NewObjectArray(env, 2, string_class, NULL);
    (*env)->SetObjectArrayElement(env, projection, 0, (*env)->NewStringUTF(env, "native_lib_dir"));
    (*env)->SetObjectArrayElement(env, projection, 1, (*env)->NewStringUTF(env, "so_filename"));
    jobject cursor = (*env)->CallObjectMethod(env, resolver, query, uri, projection, NULL, NULL, NULL);
    if ((*env)->ExceptionCheck(env) || !cursor) {
        clear_exception(env, "runtime query");
        xd_log(XD_WARN, "VRX: Snapdragon Spaces runtime provider returned no cursor");
        return false;
    }

    jclass cursor_class = (*env)->GetObjectClass(env, cursor);
    jmethodID move_first = (*env)->GetMethodID(env, cursor_class, "moveToFirst", "()Z");
    jmethodID close = (*env)->GetMethodID(env, cursor_class, "close", "()V");
    jboolean has_row = move_first ? (*env)->CallBooleanMethod(env, cursor, move_first) : JNI_FALSE;
    char libdir[PATH_MAX];
    char filename[NAME_MAX];
    int found = has_row &&
        string_column(env, cursor, "native_lib_dir", libdir, sizeof libdir) &&
        string_column(env, cursor, "so_filename", filename, sizeof filename);
    if (close)
        (*env)->CallVoidMethod(env, cursor, close);
    clear_exception(env, "cursor close");
    if (!found || libdir[0] != '/' || strstr(filename, "/")) {
        xd_log(XD_WARN, "VRX: Snapdragon Spaces runtime provider returned no usable runtime");
        return false;
    }
    char path[PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/%s", libdir, filename);
    return xd_replace_next_loader(path);
}
#else
bool xd_load_vrx_runtime(void) { return false; }
#endif
