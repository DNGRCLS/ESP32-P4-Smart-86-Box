/*
 * LVGL heap in PSRAM.  Every widget, style, label text and tiny_ttf glyph LVGL allocates goes
 * to the 32 MB PSRAM instead of the ~300 KB internal RAM (which the default C allocator would
 * use for any block up to 4 KB).  Falls back to internal RAM only if PSRAM is exhausted.
 */
#include "../../lvgl/lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include <string.h>
#include "esp_heap_caps.h"

#define PS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, PS);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(PS);
    mon_p->free_size = heap_caps_get_free_size(PS);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(PS);
    mon_p->used_pct = mon_p->total_size ? 100 - (uint8_t)(mon_p->free_size * 100 / mon_p->total_size) : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif
