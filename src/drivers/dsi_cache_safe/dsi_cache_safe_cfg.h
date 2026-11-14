#pragma once
/*
 * P4Box: private builds of ESP-IDF v5.5.5 (b774170ff46, the version the Arduino core 3.3.12 libraries
 * were built from) of the MIPI-DSI DPI panel driver and the DW-GDMA driver, compiled with the options
 *     CONFIG_LCD_DSI_ISR_CACHE_SAFE   "Allow DSI ISR to execute when cache is disabled"
 *     CONFIG_DW_GDMA_ISR_IRAM_SAFE    (selected by the above)
 * which the precompiled Arduino libraries leave off.
 *
 * Why: the DPI driver restarts the display DMA from an interrupt at the end of every frame. Without
 * these options that interrupt is masked while the SPI flash is being written (NVS saves), so a long
 * flash erase stops the picture for a few frames and the panel flashes white.
 *
 * The public functions are renamed (p4box_...) so they don't clash with the copies in the libraries.
 */
#include "sdkconfig.h"
#ifndef CONFIG_LCD_DSI_ISR_CACHE_SAFE
#define CONFIG_LCD_DSI_ISR_CACHE_SAFE 1
#endif
#ifndef CONFIG_LCD_DSI_ISR_HANDLER_IN_IRAM
#define CONFIG_LCD_DSI_ISR_HANDLER_IN_IRAM 1
#endif
#ifndef CONFIG_LCD_DSI_OBJ_FORCE_INTERNAL
#define CONFIG_LCD_DSI_OBJ_FORCE_INTERNAL 1
#endif
#ifndef CONFIG_DW_GDMA_ISR_IRAM_SAFE
#define CONFIG_DW_GDMA_ISR_IRAM_SAFE 1
#endif
#ifndef CONFIG_DW_GDMA_OBJ_DRAM_SAFE
#define CONFIG_DW_GDMA_OBJ_DRAM_SAFE 1
#endif

/* renamed driver API */
#define dw_gdma_channel_abort p4box_dw_gdma_channel_abort
#define dw_gdma_channel_config_transfer p4box_dw_gdma_channel_config_transfer
#define dw_gdma_channel_continue p4box_dw_gdma_channel_continue
#define dw_gdma_channel_enable_ctrl p4box_dw_gdma_channel_enable_ctrl
#define dw_gdma_channel_get_id p4box_dw_gdma_channel_get_id
#define dw_gdma_channel_lock p4box_dw_gdma_channel_lock
#define dw_gdma_channel_register_event_callbacks p4box_dw_gdma_channel_register_event_callbacks
#define dw_gdma_channel_set_block_markers p4box_dw_gdma_channel_set_block_markers
#define dw_gdma_channel_suspend_ctrl p4box_dw_gdma_channel_suspend_ctrl
#define dw_gdma_channel_unlock p4box_dw_gdma_channel_unlock
#define dw_gdma_channel_use_link_list p4box_dw_gdma_channel_use_link_list
#define dw_gdma_del_channel p4box_dw_gdma_del_channel
#define dw_gdma_del_link_list p4box_dw_gdma_del_link_list
#define dw_gdma_link_list_get_item p4box_dw_gdma_link_list_get_item
#define dw_gdma_lli_config_transfer p4box_dw_gdma_lli_config_transfer
#define dw_gdma_lli_set_block_markers p4box_dw_gdma_lli_set_block_markers
#define dw_gdma_lli_set_next p4box_dw_gdma_lli_set_next
#define dw_gdma_new_channel p4box_dw_gdma_new_channel
#define dw_gdma_new_link_list p4box_dw_gdma_new_link_list
#define dw_gdma_channel_default_isr p4box_dw_gdma_channel_default_isr
#define esp_lcd_new_panel_dpi p4box_esp_lcd_new_panel_dpi
#define esp_lcd_dpi_panel_get_frame_buffer p4box_esp_lcd_dpi_panel_get_frame_buffer
#define esp_lcd_dpi_panel_register_event_callbacks p4box_esp_lcd_dpi_panel_register_event_callbacks
#define esp_lcd_dpi_panel_set_color_conversion p4box_esp_lcd_dpi_panel_set_color_conversion
#define esp_lcd_dpi_panel_set_pattern p4box_esp_lcd_dpi_panel_set_pattern
#define esp_lcd_dpi_panel_enable_dma2d p4box_esp_lcd_dpi_panel_enable_dma2d
#define mipi_dsi_dma_trans_done_cb p4box_mipi_dsi_dma_trans_done_cb
#define mipi_dsi_bridge_isr_handler p4box_mipi_dsi_bridge_isr_handler
