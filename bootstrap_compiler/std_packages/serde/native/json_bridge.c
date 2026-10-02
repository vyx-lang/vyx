#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct vyx_json_owner {
    cJSON *value;
} vyx_json_owner;

enum {
    VYX_JSON_INVALID = 0,
    VYX_JSON_NULL = 1,
    VYX_JSON_BOOL = 2,
    VYX_JSON_NUMBER = 3,
    VYX_JSON_STRING = 4,
    VYX_JSON_ARRAY = 5,
    VYX_JSON_OBJECT = 6
};

static vyx_json_owner *vyx_json_wrap(cJSON *value) {
    if (value == NULL) {
        return NULL;
    }
    vyx_json_owner *owner = (vyx_json_owner *)malloc(sizeof(vyx_json_owner));
    if (owner == NULL) {
        cJSON_Delete(value);
        return NULL;
    }
    owner->value = value;
    return owner;
}

static int32_t vyx_json_type_of_value(const cJSON *value) {
    if (value == NULL || cJSON_IsInvalid(value)) return VYX_JSON_INVALID;
    if (cJSON_IsNull(value)) return VYX_JSON_NULL;
    if (cJSON_IsBool(value)) return VYX_JSON_BOOL;
    if (cJSON_IsNumber(value)) return VYX_JSON_NUMBER;
    if (cJSON_IsString(value)) return VYX_JSON_STRING;
    if (cJSON_IsArray(value)) return VYX_JSON_ARRAY;
    if (cJSON_IsObject(value)) return VYX_JSON_OBJECT;
    return VYX_JSON_INVALID;
}

void *vyx_json_parse(const char *text, int64_t *error_offset) {
    if (error_offset != NULL) *error_offset = -1;
    if (text == NULL) {
        if (error_offset != NULL) *error_offset = 0;
        return NULL;
    }
    const char *parse_end = NULL;
    cJSON *value = cJSON_ParseWithOpts(text, &parse_end, 1);
    if (value == NULL) {
        if (error_offset != NULL && parse_end != NULL) {
            *error_offset = (int64_t)(parse_end - text);
        }
        return NULL;
    }
    return vyx_json_wrap(value);
}

void *vyx_json_new_null(void) { return vyx_json_wrap(cJSON_CreateNull()); }
void *vyx_json_new_bool(int32_t value) { return vyx_json_wrap(cJSON_CreateBool(value != 0)); }
void *vyx_json_new_number(double value) { return vyx_json_wrap(cJSON_CreateNumber(value)); }
void *vyx_json_new_string(const char *value) { return vyx_json_wrap(cJSON_CreateString(value)); }
void *vyx_json_new_array(void) { return vyx_json_wrap(cJSON_CreateArray()); }
void *vyx_json_new_object(void) { return vyx_json_wrap(cJSON_CreateObject()); }

void vyx_json_release(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL) return;
    cJSON_Delete(owner->value);
    owner->value = NULL;
    free(owner);
}

void *vyx_json_duplicate(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || owner->value == NULL) return NULL;
    return vyx_json_wrap(cJSON_Duplicate(owner->value, 1));
}

int32_t vyx_json_type(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    return owner == NULL ? VYX_JSON_INVALID : vyx_json_type_of_value(owner->value);
}

int64_t vyx_json_size(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || owner->value == NULL) return -1;
    if (!cJSON_IsArray(owner->value) && !cJSON_IsObject(owner->value)) return -1;
    return (int64_t)cJSON_GetArraySize(owner->value);
}

const char *vyx_json_string_value(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    return owner == NULL ? NULL : cJSON_GetStringValue(owner->value);
}

double vyx_json_number_value(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    return owner == NULL ? 0.0 : cJSON_GetNumberValue(owner->value);
}

int32_t vyx_json_bool_value(void *raw_owner) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsBool(owner->value)) return -1;
    return cJSON_IsTrue(owner->value) ? 1 : 0;
}

int32_t vyx_json_object_item_type(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return -2;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    return item == NULL ? -1 : vyx_json_type_of_value(item);
}

const char *vyx_json_object_string(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    return item == NULL ? NULL : cJSON_GetStringValue(item);
}

double vyx_json_object_number(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return 0.0;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    return item == NULL ? 0.0 : cJSON_GetNumberValue(item);
}

int32_t vyx_json_object_bool(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return 0;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    return item != NULL && cJSON_IsTrue(item) ? 1 : 0;
}

void *vyx_json_object_get(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    if (item == NULL) return NULL;
    return vyx_json_wrap(cJSON_Duplicate(item, 1));
}

void *vyx_json_array_get(void *raw_owner, int64_t index) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsArray(owner->value) || index < 0 || index > INT32_MAX) return NULL;
    cJSON *item = cJSON_GetArrayItem(owner->value, (int32_t)index);
    if (item == NULL) return NULL;
    return vyx_json_wrap(cJSON_Duplicate(item, 1));
}

const char *vyx_json_object_key_at(void *raw_owner, int64_t index) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || index < 0) return NULL;
    cJSON *item = owner->value->child;
    while (item != NULL && index > 0) {
        item = item->next;
        --index;
    }
    return item == NULL ? NULL : item->string;
}

static int32_t vyx_json_object_put_item(vyx_json_owner *owner, const char *key, cJSON *item) {
    if (item == NULL) return -2;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) {
        cJSON_Delete(item);
        return -1;
    }
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(owner->value, key);
    cJSON_bool ok = existing != NULL
        ? cJSON_ReplaceItemInObjectCaseSensitive(owner->value, key, item)
        : cJSON_AddItemToObject(owner->value, key, item);
    if (!ok) {
        cJSON_Delete(item);
        return -2;
    }
    return 0;
}

int32_t vyx_json_object_put_null(void *raw_owner, const char *key) {
    return vyx_json_object_put_item((vyx_json_owner *)raw_owner, key, cJSON_CreateNull());
}

int32_t vyx_json_object_put_bool(void *raw_owner, const char *key, int32_t value) {
    return vyx_json_object_put_item((vyx_json_owner *)raw_owner, key, cJSON_CreateBool(value != 0));
}

int32_t vyx_json_object_put_number(void *raw_owner, const char *key, double value) {
    return vyx_json_object_put_item((vyx_json_owner *)raw_owner, key, cJSON_CreateNumber(value));
}

int32_t vyx_json_object_put_string(void *raw_owner, const char *key, const char *value) {
    return vyx_json_object_put_item((vyx_json_owner *)raw_owner, key, cJSON_CreateString(value));
}

int32_t vyx_json_object_put_value(void *raw_owner, const char *key, void *raw_value) {
    vyx_json_owner *value = (vyx_json_owner *)raw_value;
    cJSON *copy = value == NULL ? NULL : cJSON_Duplicate(value->value, 1);
    return vyx_json_object_put_item((vyx_json_owner *)raw_owner, key, copy);
}

static int32_t vyx_json_array_push_item(vyx_json_owner *owner, cJSON *item) {
    if (item == NULL) return -2;
    if (owner == NULL || !cJSON_IsArray(owner->value)) {
        cJSON_Delete(item);
        return -1;
    }
    if (!cJSON_AddItemToArray(owner->value, item)) {
        cJSON_Delete(item);
        return -2;
    }
    return 0;
}

int32_t vyx_json_array_push_null(void *raw_owner) {
    return vyx_json_array_push_item((vyx_json_owner *)raw_owner, cJSON_CreateNull());
}

int32_t vyx_json_array_push_bool(void *raw_owner, int32_t value) {
    return vyx_json_array_push_item((vyx_json_owner *)raw_owner, cJSON_CreateBool(value != 0));
}

int32_t vyx_json_array_push_number(void *raw_owner, double value) {
    return vyx_json_array_push_item((vyx_json_owner *)raw_owner, cJSON_CreateNumber(value));
}

int32_t vyx_json_array_push_string(void *raw_owner, const char *value) {
    return vyx_json_array_push_item((vyx_json_owner *)raw_owner, cJSON_CreateString(value));
}

int32_t vyx_json_array_push_value(void *raw_owner, void *raw_value) {
    vyx_json_owner *value = (vyx_json_owner *)raw_value;
    cJSON *copy = value == NULL ? NULL : cJSON_Duplicate(value->value, 1);
    return vyx_json_array_push_item((vyx_json_owner *)raw_owner, copy);
}

int32_t vyx_json_object_remove(void *raw_owner, const char *key) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || !cJSON_IsObject(owner->value) || key == NULL) return -1;
    cJSON *item = cJSON_DetachItemFromObjectCaseSensitive(owner->value, key);
    if (item == NULL) return 0;
    cJSON_Delete(item);
    return 1;
}

char *vyx_json_print(void *raw_owner, int32_t pretty) {
    vyx_json_owner *owner = (vyx_json_owner *)raw_owner;
    if (owner == NULL || owner->value == NULL) return NULL;
    return pretty ? cJSON_Print(owner->value) : cJSON_PrintUnformatted(owner->value);
}

void vyx_json_string_free(void *value) { cJSON_free(value); }
