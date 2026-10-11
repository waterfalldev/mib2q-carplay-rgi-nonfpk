/* The stock CoreFoundation-lite in libairplay (cflite.h). */
#include "cflite.h"
#include "logging.h"

#include <dlfcn.h>
#include <pthread.h>

#define LOG_MODULE "cflite"

static cflite_t g_cf;
static int g_cf_ready;
static pthread_once_t g_cf_once = PTHREAD_ONCE_INIT;

#define RESOLVE(field, name) \
    do { *(void **)&g_cf.field = dlsym(RTLD_DEFAULT, name); if (!g_cf.field) missing = name; } while (0)

static void resolve(void) {
    const char *missing = NULL;
    RESOLVE(string_create_with_cstring, "CFStringCreateWithCString");
    RESOLVE(retain, "CFRetain");
    RESOLVE(release, "CFRelease");
    RESOLVE(plist_create_deep_copy, "CFPropertyListCreateDeepCopy");
    RESOLVE(plist_create_data, "CFPropertyListCreateData");
    RESOLVE(dictionary_create_mutable, "CFDictionaryCreateMutable");
    RESOLVE(dictionary_get_value, "CFDictionaryGetValue");
    RESOLVE(dictionary_set_value, "CFDictionarySetValue");
    RESOLVE(dictionary_get_int64, "CFDictionaryGetInt64");
    RESOLVE(dictionary_set_int64, "CFDictionarySetInt64");
    RESOLVE(dictionary_set_cstring, "CFDictionarySetCString");
    RESOLVE(array_create_mutable, "CFArrayCreateMutable");
    RESOLVE(array_create_mutable_copy, "CFArrayCreateMutableCopy");
    RESOLVE(array_get_count, "CFArrayGetCount");
    RESOLVE(array_get_value_at_index, "CFArrayGetValueAtIndex");
    RESOLVE(array_append_value, "CFArrayAppendValue");
    RESOLVE(dictionary_key_callbacks, "kCFLDictionaryKeyCallBacksCFLTypes");
    RESOLVE(dictionary_value_callbacks, "kCFLDictionaryValueCallBacksCFLTypes");
    RESOLVE(array_callbacks, "kCFLArrayCallBacksCFLTypes");
    {
        /* A CFBooleanRef variable, not the object: MU1329's is 4 bytes relocated to it (map04). */
        const cf_ref_t *false_ref = (const cf_ref_t *)dlsym(RTLD_DEFAULT, "kCFLBooleanFalse");
        g_cf.boolean_false = false_ref ? *false_ref : NULL;
        if (!g_cf.boolean_false) missing = "kCFLBooleanFalse";
    }
    if (missing) LOG_WARN(LOG_MODULE, "stock CoreFoundation-lite unavailable (%s)", missing);
    g_cf_ready = missing == NULL;
}

const cflite_t *cflite(void) {
    pthread_once(&g_cf_once, resolve);
    return g_cf_ready ? &g_cf : NULL;
}

static cf_ref_t key_string(const cflite_t *cf, const char *key) {
    return cf->string_create_with_cstring(NULL, key, CF_STRING_ENCODING_UTF8);
}

cf_ref_t cf_get(const cflite_t *cf, cf_ref_t dict, const char *key) {
    cf_ref_t k, value;
    if (!dict || !(k = key_string(cf, key))) return NULL;
    value = cf->dictionary_get_value(dict, k);
    cf->release(k);
    return value;
}

int64_t cf_get_int64(const cflite_t *cf, cf_ref_t dict, const char *key, int *ok) {
    cf_ref_t k;
    int32_t err = -1;
    int64_t value = 0;
    if (dict && (k = key_string(cf, key))) {
        value = cf->dictionary_get_int64(dict, k, &err);
        cf->release(k);
    }
    *ok = err == 0;
    return err == 0 ? value : 0;
}

void cf_set(const cflite_t *cf, cf_ref_t dict, const char *key, cf_ref_t value) {
    cf_ref_t k = key_string(cf, key);
    if (!k) return;
    cf->dictionary_set_value(dict, k, value);
    cf->release(k);
}

void cf_set_int64(const cflite_t *cf, cf_ref_t dict, const char *key, int64_t value) {
    cf_ref_t k = key_string(cf, key);
    if (!k) return;
    cf->dictionary_set_int64(dict, k, value);
    cf->release(k);
}

void cf_set_cstring(const cflite_t *cf, cf_ref_t dict, const char *key, const char *value) {
    cf_ref_t k = key_string(cf, key);
    if (!k) return;
    cf->dictionary_set_cstring(dict, k, value, (size_t)-1);
    cf->release(k);
}

cf_ref_t cf_dictionary(const cflite_t *cf) {
    return cf->dictionary_create_mutable(NULL, 0, cf->dictionary_key_callbacks, cf->dictionary_value_callbacks);
}

cf_ref_t cf_array(const cflite_t *cf) {
    return cf->array_create_mutable(NULL, 0, cf->array_callbacks);
}

void cf_release(const cflite_t *cf, cf_ref_t object) {
    if (object) cf->release(object);
}
