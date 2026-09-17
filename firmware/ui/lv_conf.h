/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal LVGL v9.2.2 configuration shared by the ESP32 firmware image and
 * the desktop simulator, via -DLV_CONF_PATH pointing here from both
 * firmware/platform/esp32's idf_component.yml-managed lvgl component and
 * simulator/CMakeLists.txt's FetchContent'd lvgl. lv_conf_internal.h fills
 * in a sane default for every setting left undefined here, so this file
 * only overrides what firmware/ui actually needs: basic widgets (label,
 * button, button matrix) for the diagnostic and calibration screens, no
 * demo/example/font bloat.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
/* The ILI9341 panel expects big-endian RGB565 over SPI; LVGL's internal
 * buffer is little-endian (native MCU byte order). Without this, blended/
 * anti-aliased pixel values (font edges) land on scrambled byte pairs,
 * which showed up on real hardware as green-tinted fringing around text.
 * lv_refr.c calls lv_draw_sw_rgb565_swap() automatically before flush_cb
 * when this is set (v8-compat path, still supported in v9). Harmless for
 * the simulator, which never inspects pixel color values. */
#define LV_COLOR_16_SWAP 1

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

/* Memory: LVGL's builtin allocator with a single pool, so lv_mem_monitor()
 * keeps reporting pool peak/available for the capacity probe. The pool is
 * obtained from the C heap (malloc) at lv_init() rather than declared as a
 * static array: on ESP32 only one DRAM segment is linkable for static
 * data, while the runtime heap also spans DRAM that the linker cannot
 * place .bss into. A 64 KiB static pool consumed ~60% of the linkable
 * segment and was the actual cause of the M1a radio link overflows. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#ifndef FIRMWARE_LV_MEM_SIZE_KIB
#define FIRMWARE_LV_MEM_SIZE_KIB 64
#endif
#define LV_MEM_SIZE (FIRMWARE_LV_MEM_SIZE_KIB * 1024U)
#define LV_MEM_ADR 0
#define LV_MEM_POOL_INCLUDE <stdlib.h>
#define LV_MEM_POOL_ALLOC malloc

#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_OBJ 1

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#define LV_TICK_CUSTOM 0

#endif /* LV_CONF_H */
