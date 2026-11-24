/*
 * Minimal LVGL stub for pod_layout unit tests.
 * Provides only the types that pod_layout.h needs.
 */
#ifndef LVGL_LVGL_H
#define LVGL_LVGL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* lv_obj_t forward declaration */
typedef struct _lv_obj_t lv_obj_t;

/* lv_image_dsc_t (simplified – only size/pointer needed for tests) */
typedef struct {
    uint32_t magic   : 8;
    uint32_t cf      : 8;
    uint32_t flags   : 16;
    uint32_t w       : 16;
    uint32_t h       : 16;
    uint32_t stride  : 16;
    uint32_t reserved_2 : 16;
} lv_image_header_t;

typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;

#endif /* LVGL_LVGL_H */
