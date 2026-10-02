#include "miniz.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct vyx_miniz_buffer {
    unsigned char *data;
    int64_t size;
    int64_t original_size;
} vyx_miniz_buffer;

enum {
    VYX_MINIZ_OK = 0,
    VYX_MINIZ_INVALID_ARGUMENT = 1,
    VYX_MINIZ_ALLOCATION_FAILED = 2,
    VYX_MINIZ_COMPRESS_FAILED = 3,
    VYX_MINIZ_DECOMPRESS_FAILED = 4,
    VYX_MINIZ_OUTPUT_TOO_SMALL = 5
};

static void vyx_miniz_set_status(int32_t *status, int32_t value) {
    if (status != NULL) *status = value;
}

void *vyx_miniz_compress(const void *source, int64_t source_len, int32_t level,
                         int32_t *status) {
    vyx_miniz_set_status(status, VYX_MINIZ_OK);
    if (source_len < 0 || (source == NULL && source_len != 0) || level < 0 || level > 9 ||
        (uint64_t)source_len > UINT32_MAX) {
        vyx_miniz_set_status(status, VYX_MINIZ_INVALID_ARGUMENT);
        return NULL;
    }

    mz_ulong bound = mz_compressBound((mz_ulong)source_len);
    if ((uint64_t)bound > SIZE_MAX) {
        vyx_miniz_set_status(status, VYX_MINIZ_ALLOCATION_FAILED);
        return NULL;
    }
    vyx_miniz_buffer *buffer = (vyx_miniz_buffer *)calloc(1, sizeof(vyx_miniz_buffer));
    if (buffer == NULL) {
        vyx_miniz_set_status(status, VYX_MINIZ_ALLOCATION_FAILED);
        return NULL;
    }
    buffer->data = (unsigned char *)malloc(bound == 0 ? 1 : (size_t)bound);
    if (buffer->data == NULL) {
        free(buffer);
        vyx_miniz_set_status(status, VYX_MINIZ_ALLOCATION_FAILED);
        return NULL;
    }

    mz_ulong compressed_len = bound;
    int rc = mz_compress2(buffer->data, &compressed_len,
                          (const unsigned char *)source, (mz_ulong)source_len, level);
    if (rc != MZ_OK) {
        free(buffer->data);
        free(buffer);
        vyx_miniz_set_status(status, VYX_MINIZ_COMPRESS_FAILED);
        return NULL;
    }
    buffer->size = (int64_t)compressed_len;
    buffer->original_size = source_len;
    return buffer;
}

void *vyx_miniz_buffer_clone(const void *raw_buffer) {
    const vyx_miniz_buffer *source = (const vyx_miniz_buffer *)raw_buffer;
    if (source == NULL || source->size < 0 || (source->data == NULL && source->size != 0)) {
        return NULL;
    }
    vyx_miniz_buffer *copy = (vyx_miniz_buffer *)calloc(1, sizeof(vyx_miniz_buffer));
    if (copy == NULL) return NULL;
    copy->data = (unsigned char *)malloc(source->size == 0 ? 1 : (size_t)source->size);
    if (copy->data == NULL) {
        free(copy);
        return NULL;
    }
    if (source->size != 0) memcpy(copy->data, source->data, (size_t)source->size);
    copy->size = source->size;
    copy->original_size = source->original_size;
    return copy;
}

int64_t vyx_miniz_buffer_len(const void *raw_buffer) {
    const vyx_miniz_buffer *buffer = (const vyx_miniz_buffer *)raw_buffer;
    return buffer == NULL ? 0 : buffer->size;
}

int64_t vyx_miniz_buffer_original_len(const void *raw_buffer) {
    const vyx_miniz_buffer *buffer = (const vyx_miniz_buffer *)raw_buffer;
    return buffer == NULL ? 0 : buffer->original_size;
}

void vyx_miniz_buffer_release(void *raw_buffer) {
    vyx_miniz_buffer *buffer = (vyx_miniz_buffer *)raw_buffer;
    if (buffer == NULL) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->original_size = 0;
    free(buffer);
}

void *vyx_miniz_decompress_text(const void *raw_buffer, int64_t max_output_len,
                                int64_t *output_len, int32_t *status) {
    const vyx_miniz_buffer *buffer = (const vyx_miniz_buffer *)raw_buffer;
    if (output_len != NULL) *output_len = 0;
    vyx_miniz_set_status(status, VYX_MINIZ_OK);
    if (buffer == NULL || buffer->size < 0 || max_output_len < 0 ||
        (buffer->data == NULL && buffer->size != 0) ||
        buffer->original_size < 0 || (uint64_t)buffer->size > UINT32_MAX ||
        (uint64_t)max_output_len > UINT32_MAX || (uint64_t)max_output_len >= SIZE_MAX) {
        vyx_miniz_set_status(status, VYX_MINIZ_INVALID_ARGUMENT);
        return NULL;
    }
    if (max_output_len < buffer->original_size) {
        vyx_miniz_set_status(status, VYX_MINIZ_OUTPUT_TOO_SMALL);
        return NULL;
    }

    unsigned char *output = (unsigned char *)malloc((size_t)max_output_len + 1);
    if (output == NULL) {
        vyx_miniz_set_status(status, VYX_MINIZ_ALLOCATION_FAILED);
        return NULL;
    }
    mz_ulong actual_len = (mz_ulong)(max_output_len == 0 ? 1 : max_output_len);
    int rc = mz_uncompress(output, &actual_len, buffer->data, (mz_ulong)buffer->size);
    if (rc != MZ_OK) {
        free(output);
        vyx_miniz_set_status(status,
            rc == MZ_BUF_ERROR ? VYX_MINIZ_OUTPUT_TOO_SMALL : VYX_MINIZ_DECOMPRESS_FAILED);
        return NULL;
    }
    output[actual_len] = 0;
    if (output_len != NULL) *output_len = (int64_t)actual_len;
    return output;
}

void vyx_miniz_free(void *value) { free(value); }

const char *vyx_miniz_version(void) { return mz_version(); }

int64_t vyx_miniz_crc32(const void *data, int64_t len) {
    if (len < 0 || (data == NULL && len != 0) || (uint64_t)len > UINT32_MAX) return -1;
    return (int64_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)data, (size_t)len);
}

int64_t vyx_miniz_adler32(const void *data, int64_t len) {
    if (len < 0 || (data == NULL && len != 0) || (uint64_t)len > UINT32_MAX) return -1;
    return (int64_t)mz_adler32(MZ_ADLER32_INIT, (const unsigned char *)data, (size_t)len);
}
