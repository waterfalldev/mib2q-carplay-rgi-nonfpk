/*
 * The stock CoreFoundation-lite in libairplay (Apple CoreUtils CFLite), resolved once by
 * name from the dio_manager process.  AirPlay's /info, SETUP and command dictionaries are
 * CF property lists; modules build and read them with these, never with their own copy.
 * Signatures are Apple's (CFLite / CoreUtils CFUtils), confirmed against the calls the
 * MU1329 cluster receiver in Build/Analysis/FullMapCarplay makes.
 */
#ifndef CARPLAY_CFLITE_H
#define CARPLAY_CFLITE_H

#include <stddef.h>
#include <stdint.h>

typedef const void *cf_ref_t;

#define CF_STRING_ENCODING_UTF8        0x08000100u
#define CF_PLIST_MUTABLE_ALL           2        /* kCFPropertyListMutableContainersAndLeaves */
#define CF_PLIST_BINARY_FORMAT         200      /* kCFPropertyListBinaryFormat_v1_0 */

typedef struct {
    cf_ref_t (*string_create_with_cstring)(cf_ref_t alloc, const char *s, uint32_t encoding);
    cf_ref_t (*retain)(cf_ref_t object);
    void (*release)(cf_ref_t object);
    cf_ref_t (*plist_create_deep_copy)(cf_ref_t alloc, cf_ref_t plist, unsigned long options);
    cf_ref_t (*plist_create_data)(cf_ref_t alloc, cf_ref_t plist, long format, unsigned long options, void *error);
    cf_ref_t (*dictionary_create_mutable)(cf_ref_t alloc, long capacity, const void *key_callbacks,
                                          const void *value_callbacks);
    cf_ref_t (*dictionary_get_value)(cf_ref_t dict, cf_ref_t key);
    void (*dictionary_set_value)(cf_ref_t dict, cf_ref_t key, cf_ref_t value);
    int64_t (*dictionary_get_int64)(cf_ref_t dict, cf_ref_t key, int32_t *err);
    void (*dictionary_set_int64)(cf_ref_t dict, cf_ref_t key, int64_t value);
    int32_t (*dictionary_set_cstring)(cf_ref_t dict, cf_ref_t key, const char *s, size_t length);
    cf_ref_t (*array_create_mutable)(cf_ref_t alloc, long capacity, const void *callbacks);
    cf_ref_t (*array_create_mutable_copy)(cf_ref_t alloc, long capacity, cf_ref_t array);
    long (*array_get_count)(cf_ref_t array);
    cf_ref_t (*array_get_value_at_index)(cf_ref_t array, long index);
    void (*array_append_value)(cf_ref_t array, cf_ref_t value);
    const void *dictionary_key_callbacks;       /* &kCFLDictionaryKeyCallBacksCFLTypes */
    const void *dictionary_value_callbacks;     /* &kCFLDictionaryValueCallBacksCFLTypes */
    const void *array_callbacks;                /* &kCFLArrayCallBacksCFLTypes */
    cf_ref_t boolean_false;                     /* kCFLBooleanFalse's value (a CFBooleanRef variable) */
} cflite_t;

/* The resolved API, or NULL (logged once) when any part is missing. */
const cflite_t *cflite(void);

/* Dictionary access by C-string key, as the stock code does (a UTF-8 CFString per call).
 * Getters return NULL / 0 with *ok = 0 when the key or dictionary is absent. */
cf_ref_t cf_get(const cflite_t *cf, cf_ref_t dict, const char *key);
int64_t cf_get_int64(const cflite_t *cf, cf_ref_t dict, const char *key, int *ok);
void cf_set(const cflite_t *cf, cf_ref_t dict, const char *key, cf_ref_t value);
void cf_set_int64(const cflite_t *cf, cf_ref_t dict, const char *key, int64_t value);
void cf_set_cstring(const cflite_t *cf, cf_ref_t dict, const char *key, const char *value);
/* A new, empty mutable dictionary / array (CF property-list callbacks), or NULL. */
cf_ref_t cf_dictionary(const cflite_t *cf);
cf_ref_t cf_array(const cflite_t *cf);
/* CFRelease that accepts NULL. */
void cf_release(const cflite_t *cf, cf_ref_t object);

#endif
