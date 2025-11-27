#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "fonts/ink_free_12.h"
#include "ssd1680_regs.h"  // E-Paper register definitions

// Pins
#define EPD_DC     8
#define EPD_RST    9
#define EPD_CS    10
#define EPD_BUSY  13
#define EPD_MOSI  11
#define EPD_SCLK  12

#define ROTATE 0

#define WS_154_V2 1
#define WS_213_V4 2
#define EPAPER_TYPE 2

#if EPAPER_TYPE == WS_154_V2
    // Waveshare 1.54" V2 correct dimensions with offset
    #define EPD_WIDTH        200 // Actual visible width
    #define EPD_HEIGHT       200 // Actual visible height
    #define EPD_WIDTH_BYTES  25  // Width in bytes (must be 25 for alignment)
#else
    // Waveshare 2.13" V4 correct dimensions with offset
    #define EPD_WIDTH        250 // Actual visible width
    #define EPD_HEIGHT       122 // Actual visible height
    #define EPD_WIDTH_BYTES  16  // Width in bytes (must be 16 for alignment)
    #if ROTATE == 1
        #undef EPD_WIDTH
        #undef EPD_HEIGHT
        #define EPD_WIDTH        122 // Actual visible width
        #define EPD_HEIGHT       250 // Actual visible height
    #endif
#endif

#define X_OFFSET         0    // X offset in bytes (0 = no offset)

#if ROTATE == 0
    #define EPD_BUFFER_SIZE  (EPD_WIDTH_BYTES * EPD_WIDTH)
#else
    #define EPD_BUFFER_SIZE  (EPD_WIDTH_BYTES * EPD_HEIGHT)
#endif

static const char *TAG = (EPAPER_TYPE==WS_154_V2?"LVGL_WS154":"LVGL_WS213");
static spi_device_handle_t spi;
static bool g_lvgl_ready = false;

// SPI transfer callback
static void spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc = (int)t->user;
    gpio_set_level(EPD_DC, dc);
}

// Send command
static void epd_send_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void*)0,  // DC = 0 for command
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
}

// Send data byte
static void epd_send_data(uint8_t data)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .user = (void*)1,  // DC = 1 for data
    };
    ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
}

// Send data buffer
static void epd_send_data_buffer(const uint8_t *buffer, size_t len)
{
    size_t chunk_size = 4000;
    size_t offset = 0;
    
    while (offset < len) {
        size_t to_send = (len - offset > chunk_size) ? chunk_size : (len - offset);
        
        spi_transaction_t t = {
            .length = to_send * 8,
            .tx_buffer = buffer + offset,
            .user = (void*)1,  // DC = 1 for data
        };
        ESP_ERROR_CHECK(spi_device_transmit(spi, &t));
        
        offset += to_send;
    }
}

// Wait for busy signal
static void epd_wait_busy(void)
{
    ESP_LOGD(TAG, "Waiting for display...");
    int timeout = 0;
    while(gpio_get_level(EPD_BUSY) == 0 && timeout < 500) {  // LOW = busy
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout++;
    }
    if (timeout >= 500) {
        ESP_LOGE(TAG, "Busy timeout!");
    } else {
        ESP_LOGD(TAG, "Ready (%dms)", timeout * 10);
    }
}

// Hardware reset
static void epd_reset(void)
{
    gpio_set_level(EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// Initialize display with correct settings for Waveshare 2.13" V4
static void epd_init(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare 2.13\" V4...");
    ESP_LOGI(TAG, "Width: %d, Height: %d, Bytes/row: %d", EPD_WIDTH, EPD_HEIGHT, EPD_WIDTH_BYTES);
    
    epd_reset();
    epd_wait_busy();
    
    // Software reset
    epd_send_cmd(SSD1680_CMD_SW_RESET);
    epd_wait_busy();
    
    // Driver output control - rotated coordinates
    epd_send_cmd(SSD1680_CMD_DRIVER_OUTPUT_CONTROL);
    epd_send_data(((ROTATE == 0?EPD_HEIGHT:EPD_WIDTH) - 1) & 0xFF);     // Gate drivers (rotated)
    epd_send_data(((ROTATE == 0?EPD_HEIGHT:EPD_WIDTH) >> 8) & 0xFF);
    epd_send_data(SSD1680_GATE_SCAN_UP);  // GD=0, SM=0, TB=0
    
    // Booster soft start
    epd_send_cmd(SSD1680_CMD_BOOSTER_SOFT_START);
    epd_send_data(SSD1680_BOOSTER_PHASE1_DEFAULT);
    epd_send_data(SSD1680_BOOSTER_PHASE2_DEFAULT);
    epd_send_data(SSD1680_BOOSTER_PHASE3_DEFAULT);
    
    // VCOM Voltage
    epd_send_cmd(SSD1680_CMD_WRITE_VCOM_REGISTER);
    epd_send_data(SSD1680_VCOM_VOLTAGE_DEFAULT);
    
    // Set dummy line period
    epd_send_cmd(SSD1680_CMD_DUMMY_LINE_PERIOD);
    epd_send_data(SSD1680_DUMMY_LINE_PERIOD_DEFAULT);
    
    // Set Gate line width
    epd_send_cmd(SSD1680_CMD_GATE_LINE_WIDTH);
    epd_send_data(SSD1680_GATE_LINE_WIDTH_DEFAULT);
    
    // Data entry mode - ROTATED 90 degrees + Y mirrored
    epd_send_cmd(SSD1680_CMD_DATA_ENTRY_MODE);
    epd_send_data(SSD1680_DATA_ENTRY_XINC_YINC);  // 90° rotation
    
    // Set RAM X address start/end position
    epd_send_cmd(SSD1680_CMD_SET_RAM_X_START_END);
    epd_send_data(X_OFFSET);  // RAM X start = offset
    epd_send_data(X_OFFSET + (EPD_WIDTH_BYTES - 1));  // RAM X end
    
    // Set RAM Y address start/end position - rotated
    epd_send_cmd(SSD1680_CMD_SET_RAM_Y_START_END);
    epd_send_data(0x00);  // Y start low
    epd_send_data(0x00);  // Y start high
    epd_send_data(((ROTATE == 0?EPD_HEIGHT:EPD_WIDTH) - 1) & 0xFF);  // Y end low (rotated)
    epd_send_data((((ROTATE == 0?EPD_HEIGHT:EPD_WIDTH) - 1) >> 8) & 0xFF);  // Y end high
    
    // Border waveform
    epd_send_cmd(SSD1680_CMD_BORDER_WAVEFORM_CONTROL);
    epd_send_data(SSD1680_BORDER_WAVEFORM_DEFAULT);
    
    // Display update control
    epd_send_cmd(SSD1680_CMD_DISPLAY_UPDATE_CONTROL_1);
    epd_send_data(SSD1680_UPDATE_CTRL1_DEFAULT_1);
    epd_send_data(SSD1680_UPDATE_CTRL1_DEFAULT_2);
    
    // Temperature sensor
    epd_send_cmd(SSD1680_CMD_TEMP_SENSOR_CONTROL);
    epd_send_data(SSD1680_TEMP_SENSOR_INTERNAL);
    
    ESP_LOGI(TAG, "Init complete with X offset: %d", X_OFFSET);
}

// Set cursor position
static void epd_set_cursor(uint8_t x, uint16_t y)
{
    epd_send_cmd(SSD1680_CMD_SET_RAM_X_COUNTER);  // Set RAM X address counter
    epd_send_data(x + X_OFFSET);  // Add offset
    
    epd_send_cmd(SSD1680_CMD_SET_RAM_Y_COUNTER);  // Set RAM Y address counter
    epd_send_data(y & 0xFF);
    epd_send_data((y >> 8) & 0xFF);
}

// Display update
static void epd_display_frame(void)
{
    epd_send_cmd(SSD1680_CMD_DISPLAY_UPDATE_CONTROL_2);  // Display Update Control 2
    epd_send_data(SSD1680_DISPLAY_UPDATE_FULL);  // Full update
    epd_send_cmd(SSD1680_CMD_MASTER_ACTIVATION);  // Master Activation
    epd_wait_busy();
}

// RGB565 → Monochrome conersion - 90 degree rotated
static void rgb565_to_mono(const lv_color16_t *src, uint8_t *dst, int w, int h)
{
    memset(dst, 0xFF, EPD_BUFFER_SIZE);  // Start with white
    
    // Rotate 90 degrees clockwise during conversion
    // LVGL coordinate (x, y) -> EPD coordinate (y, HEIGHT-1-x)
    for (int lvgl_y = 0; lvgl_y < h && lvgl_y < EPD_HEIGHT/*(ROTATE==0?EPD_HEIGHT:EPD_WIDTH)*/; lvgl_y++) {
        for (int lvgl_x = 0; lvgl_x < w && lvgl_x < EPD_WIDTH/*(ROTATE==0?EPD_WIDTH:EPD_HEIGHT)*/; lvgl_x++) {
            lv_color16_t px = src[lvgl_y * w + lvgl_x];
            
            // RGB565 → Grayscale
            uint8_t gray = ((px.red << 3) * 30 + (px.green << 2) * 59 + (px.blue << 3) * 11) / 100;
            
            // Rotate + Mirror X coordinates: (lvgl_x, lvgl_y) -> (WIDTH-1-lvgl_y, HEIGHT-1-lvgl_x)
            //int epd_x = EPD_WIDTH - 1 - lvgl_y;  // X mirrored
            //int epd_y = EPD_HEIGHT - 1 - lvgl_x;
			int epd_x = -1;              
			int epd_y = -1;

            if ((EPAPER_TYPE == WS_154_V2 && ROTATE==1) || (EPAPER_TYPE == WS_213_V4 && ROTATE==0)){
                epd_x = lvgl_y;              
			    epd_y = lvgl_x;
            }
            else {
                epd_x = (ROTATE==1?EPD_WIDTH:EPD_HEIGHT) - 1 - lvgl_x;
			    epd_y = lvgl_y;
            }
			
            // Convert to buffer position
            int byte_x = epd_x / 8;
            int bit_x = 7 - (epd_x % 8);  // MSB first
            int byte_idx = epd_y * EPD_WIDTH_BYTES + byte_x;
            
            if (byte_idx < EPD_BUFFER_SIZE) {
                // Threshold: <128 = black
                if (gray < 128) {
                    dst[byte_idx] &= ~(1 << bit_x);  // Clear bit for black
                } else {
                    dst[byte_idx] |= (1 << bit_x);   // Set bit for white
                }
            }
        }
    }
}

// Flush callback - Convert RGB565 to monochrome and send to e-paper
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ESP_LOGI(TAG, ">>> FLUSH CALLED <<<");
    
    static uint8_t mono_buf[EPD_BUFFER_SIZE];
    
    // Covert RGB565 buffer to mono - LVGL buffer is 250x122, converts to rotated 122x250
    //rgb565_to_mono((lv_color16_t *)px_map, mono_buf, (ROTATE==0?EPD_WIDTH:EPD_HEIGHT), (ROTATE==0?EPD_HEIGHT:EPD_WIDTH));
    rgb565_to_mono((lv_color16_t *)px_map, mono_buf, EPD_WIDTH, EPD_HEIGHT);
    
    // Send to E-paper
    epd_set_cursor(0, 0);
    epd_send_cmd(SSD1680_CMD_WRITE_RAM_BW);  // Write RAM (Black/White)
    epd_send_data_buffer(mono_buf, EPD_BUFFER_SIZE);
    epd_display_frame();
    
    lv_display_flush_ready(disp);
    g_lvgl_ready = true;
    ESP_LOGI(TAG, ">>> FLUSH DONE <<<");
}

// Tick task
static void lv_tick_task(void *arg)
{
    while (1) {
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Screen clean and Off task
static void screen_off_task(void *arg)
{
    // Wait 10 seconds
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    ESP_LOGI(TAG, "10 seconds passed - clearing and turning off screen");
    
    // Convert screen to white (clear)
    uint8_t *white_buf = heap_caps_malloc(EPD_BUFFER_SIZE, MALLOC_CAP_DMA);
    if (white_buf) {
        memset(white_buf, 0xFF, EPD_BUFFER_SIZE);  // 0xFF = White
        
        ESP_LOGI(TAG, "Clearing screen...");
        epd_set_cursor(0, 0);
        epd_send_cmd(SSD1680_CMD_WRITE_RAM_BW);  // Write RAM (Black/White)
        epd_send_data_buffer(white_buf, EPD_BUFFER_SIZE);
        epd_display_frame();
        ESP_LOGI(TAG, "Screen cleared");
        
        heap_caps_free(white_buf);
    }
    
    // Wait 1 second
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Turn off screen (deep sleep)
    ESP_LOGI(TAG, "Turning off display");
    epd_send_cmd(SSD1680_CMD_DEEP_SLEEP_MODE);  // Deep sleep
    epd_send_data(SSD1680_DEEP_SLEEP_MODE_1);  // Mode 1: Retain RAM
    
    ESP_LOGI(TAG, "Display is now OFF - entering idle mode");
    
    // End task
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Waveshare 2.13\" V4 LVGL ===");
    ESP_LOGI(TAG, "Display: %dx%d, Buffer: %d bytes", EPD_WIDTH, EPD_HEIGHT, EPD_BUFFER_SIZE);
    
    esp_task_wdt_deinit();
    
    // GPIO init
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << EPD_DC) | (1ULL << EPD_RST) | (1ULL << EPD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
    
    io_conf.pin_bit_mask = (1ULL << EPD_BUSY);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    
    gpio_set_level(EPD_CS, 1);
    gpio_set_level(EPD_RST, 1);
    
    // SPI init
    spi_bus_config_t buscfg = {
        .mosi_io_num = EPD_MOSI,
        .sclk_io_num = EPD_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,  // 4MHz
        .mode = 0,
        .spics_io_num = EPD_CS,
        .queue_size = 7,
        .pre_cb = spi_pre_transfer_callback,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi));
    
    // Initialize display
    epd_init();
    
    // LVGL
    lv_init();
    xTaskCreate(lv_tick_task, "lv_tick", 2048, NULL, 1, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "LVGL init");

    // RGB565 buffer - for user-facing rotated dimensions
    size_t buf_size = EPD_WIDTH * EPD_HEIGHT * 2;  // Width * Height * 2 bytes per pixel
    void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) {
        buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    assert(buf1);
    ESP_LOGI(TAG, "RGB565 buffer: %d bytes (%dx%d)",EPD_WIDTH,EPD_HEIGHT,buf_size);

    // LVGL display dimensions - these are the user-facing dimensions after rotation
    // Physical display is 122x250, but after 90° rotation user sees 250x122
    //lv_display_t *disp = lv_display_create((ROTATE==0?EPD_WIDTH:EPD_HEIGHT), (ROTATE==0?EPD_HEIGHT:EPD_WIDTH));  // Width x Height as user sees it
    lv_display_t *disp = lv_display_create(EPD_WIDTH, EPD_HEIGHT);  // Width x Height as user sees it
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, my_disp_flush);
    ESP_LOGI(TAG, "Display OK - User sees: %dx%d (Rotated: %s)", EPD_WIDTH,EPD_HEIGHT,ROTATE?"Yes":"No");

    // UI - White background
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    int X_LineOffset=5;
    int Y_LineOffset=3;
    int Y_Space = 20;

    // Label1
    lv_obj_t *label1 = lv_label_create(scr);
    lv_label_set_text(label1, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman1":"Süleyman Günel 1976 0001");
    lv_obj_set_style_text_color(label1, lv_color_black(), 0);
	lv_obj_set_style_text_font(label1, &ink_free_12 , 0);
	lv_obj_set_pos(label1, X_LineOffset, Y_LineOffset);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE1 created");
	
	// Label2
    lv_obj_t *label2 = lv_label_create(scr);
    lv_label_set_text(label2, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman2":"Süleyman Günel 1976 0002");
    lv_obj_set_style_text_color(label2, lv_color_black(), 0);
	lv_obj_set_style_text_font(label2, &ink_free_12 , 0);
	lv_obj_set_pos(label2, X_LineOffset, Y_LineOffset + Y_Space);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE2 created");

    // Label3
    lv_obj_t *label3 = lv_label_create(scr);
    lv_label_set_text(label3, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman3":"Süleyman Günel 1976 0003");
    lv_obj_set_style_text_color(label3, lv_color_black(), 0);
	lv_obj_set_style_text_font(label3, &ink_free_12 , 0);
	lv_obj_set_pos(label3, X_LineOffset, Y_LineOffset + Y_Space * 2);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE3 created");

    // Label4
    lv_obj_t *label4 = lv_label_create(scr);
    lv_label_set_text(label4, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman4":"Süleyman Günel 1976 0004");
    lv_obj_set_style_text_color(label4, lv_color_black(), 0);
	lv_obj_set_style_text_font(label4, &ink_free_12 , 0);
	lv_obj_set_pos(label4, X_LineOffset, Y_LineOffset + Y_Space * 3);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE4 created");

    // Label5
    lv_obj_t *label5 = lv_label_create(scr);
    lv_label_set_text(label5, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman5":"Süleyman Günel 1976 0005");
    lv_obj_set_style_text_color(label5, lv_color_black(), 0);
	lv_obj_set_style_text_font(label5, &ink_free_12 , 0);
	lv_obj_set_pos(label5, X_LineOffset, Y_LineOffset + Y_Space * 4);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE5 created");

    // Label6
    lv_obj_t *label6 = lv_label_create(scr);
    lv_label_set_text(label6, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman6":"Süleyman Günel 1976 0006");
    lv_obj_set_style_text_color(label6, lv_color_black(), 0);
	lv_obj_set_style_text_font(label6, &ink_free_12 , 0);
	lv_obj_set_pos(label6, X_LineOffset, Y_LineOffset + Y_Space * 5);
    //lv_obj_center(label);
    ESP_LOGI(TAG, "LINE6 created");

    if((ROTATE==1 && EPAPER_TYPE==WS_213_V4) || EPAPER_TYPE==WS_154_V2){
        // Label7
        lv_obj_t *label7 = lv_label_create(scr);
        lv_label_set_text(label7, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman7":"Süleyman Günel 1976 0007");
        lv_obj_set_style_text_color(label7, lv_color_black(), 0);
        lv_obj_set_style_text_font(label7, &ink_free_12 , 0);
        lv_obj_set_pos(label7, X_LineOffset, Y_LineOffset + Y_Space * 6);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE7 created");

        // Label8
        lv_obj_t *label8 = lv_label_create(scr);
        lv_label_set_text(label8, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman8":"Süleyman Günel 1976 0008");
        lv_obj_set_style_text_color(label8, lv_color_black(), 0);
        lv_obj_set_style_text_font(label8, &ink_free_12 , 0);
        lv_obj_set_pos(label8, X_LineOffset, Y_LineOffset + Y_Space * 7);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE8 created");

        // Label9
        lv_obj_t *label9 = lv_label_create(scr);
        lv_label_set_text(label9, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman9":"Süleyman Günel 1976 0009");
        lv_obj_set_style_text_color(label9, lv_color_black(), 0);
        lv_obj_set_style_text_font(label9, &ink_free_12 , 0);
        lv_obj_set_pos(label9, X_LineOffset, Y_LineOffset + Y_Space * 8);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE9 created");

        // Label10
        lv_obj_t *label10 = lv_label_create(scr);
        lv_label_set_text(label10, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman10":"Süleyman Günel 1976 0007");
        lv_obj_set_style_text_color(label10, lv_color_black(), 0);
        lv_obj_set_style_text_font(label10, &ink_free_12 , 0);
        lv_obj_set_pos(label10, X_LineOffset, Y_LineOffset + Y_Space * 9);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE10 created");

        // Label11
        lv_obj_t *label11 = lv_label_create(scr);
        lv_label_set_text(label11, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman11":"Süleyman Günel 1976 0007");
        lv_obj_set_style_text_color(label11, lv_color_black(), 0);
        lv_obj_set_style_text_font(label11, &ink_free_12 , 0);
        lv_obj_set_pos(label11, X_LineOffset, Y_LineOffset + Y_Space * 10);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE11 created");

        // Label12
        lv_obj_t *label12 = lv_label_create(scr);
        lv_label_set_text(label12, EPAPER_TYPE==WS_154_V2 || ROTATE == 1?"Süleyman12":"Süleyman Günel 1976 0007");
        lv_obj_set_style_text_color(label12, lv_color_black(), 0);
        lv_obj_set_style_text_font(label12, &ink_free_12 , 0);
        lv_obj_set_pos(label12, X_LineOffset, Y_LineOffset + Y_Space * 11);
        //lv_obj_center(label);
        ESP_LOGI(TAG, "LINE12 created");
    }

    // First render
    ESP_LOGI(TAG, "Starting render...");
    for (int i = 0; i < 30; i++) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(50));
        
        if (g_lvgl_ready) {
            ESP_LOGI(TAG, "Rendered at iteration %d", i);
            break;
        }
    }
    
    if (g_lvgl_ready) {
        ESP_LOGI(TAG, "Done!");
    } else {
        ESP_LOGE(TAG, "Timeout!");
    }

    // Call screen off task after 10 seconds
    xTaskCreate(screen_off_task, "screen_off", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Screen off task started (will trigger in 10 seconds)");

    // Main loop
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
