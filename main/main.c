#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "bsp/rtc.h"
#include "bsp/tanmatsu.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"

#include "esp_log.h"
#include "hal/lcd_types.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_codecs.h"
#include "portmacro.h"

#include "sdcard.h"

#include "wifi_connection.h"
#include "wifi_remote.h"

#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "lwip/ip4_addr.h"
#include "lwip/inet.h"


// Constants
static char const TAG[] = "main";

#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"

#define LED_GREEN 0x03FC03
#define LED_YELLOW 0xF4FC03
#define LED_RED 0xFC0303
#define LED_OFF 0x0
#define LED_BLUE 0x0303FC

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb                   = {0};

static esp_lcd_panel_handle_t    lcd_panel         = NULL;
static QueueHandle_t                input_event_queue    = NULL;
SemaphoreHandle_t line_mutex;
esp_mqtt_client_handle_t client = NULL;
bsp_power_battery_information_t battery_info;

extern uint8_t const wallpaper_start[] asm("_binary_wallpaper_png_start");
extern uint8_t const wallpaper_end[] asm("_binary_wallpaper_png_end");

bool inactive_show_time = false;

#define num_chars 60

char line_buffer[num_chars] = {0};
uint8_t led_buffer[6 * 3] = {0};

time_t now_time;
struct tm timeinfo;

uint8_t msg_led_cnt = 0;
bool mqtt_initialized = false;
bool mqtt_msg_event = false;
bool mqtt_msg_transmit = false;
bool wifi_connected = false;
bool wifi_connecting = false;
bool sd_card_present = false;


#define HEADER_HEIGHT  30
#define FOOTER_HEIGHT  30
#define BUTTON_WIDTH   100
#define BUTTON_HEIGHT  100
#define BUTTON_GAP     20
#define TEXT_FIELD_HEIGTH  24


const char* menu_title = "Event Notifier";
const char* connection_status = "Wi-Fi: Connected";
const char* footer_text = "Use left/right to navigate. Press return to select.";
const char* buttons[] = {"Nyan", "Coffee", "Lunch"};
#define NUM_BUTTONS 3


void init(void) {
    line_mutex = xSemaphoreCreateMutex();
}

void add_line(char* text) {
    if (xSemaphoreTake(line_mutex, portMAX_DELAY)) {

        size_t len = strnlen(text, num_chars - 1);
        for (size_t i = 0; i < len; i++) {
            if (text[i] == '\r' || text[i] == '\n') {
                text[i] = ' ';
            }
        }
        text[len] = '\0';


        // for (size_t i = 0; i < num_lines; i++) {
        //     line_index[i] = (line_index[i] + 1) % num_lines;
        // }
        strncpy(line_buffer, text, num_chars);
        xSemaphoreGive(line_mutex);
    }
}

void set_led_color(uint8_t led, uint32_t color) {
    led_buffer[led * 3 + 0] = (color >> 8) & 0xFF;  // G
    led_buffer[led * 3 + 1] = (color >> 16) & 0xFF; // R
    led_buffer[led * 3 + 2] = (color >> 0) & 0xFF;  // B
}

static void led_task(void* pvParameters) {
    while (1) {
        esp_err_t res = bsp_power_get_battery_information(&battery_info);
        if(res == ESP_OK) {
            if(battery_info.power_supply_available) {
                set_led_color(0, LED_GREEN);
            } else if(battery_info.remaining_percentage < 50.0) {
                set_led_color(0, LED_YELLOW);
            } else if(battery_info.remaining_percentage < 15.0) {
                set_led_color(0, LED_RED);
            } else {
                set_led_color(0, LED_BLUE); 
            }
        } 


        if(wifi_connected) set_led_color(1, LED_GREEN); // wifi led
        else if(wifi_connecting) set_led_color(1, LED_YELLOW);
        else set_led_color(1, LED_RED);

        if(mqtt_msg_event) {
            if(msg_led_cnt % 2 == 0) {
                set_led_color(2, LED_YELLOW); // message led
            } else {
                set_led_color(2, LED_OFF); // message led
                
            }
            if(msg_led_cnt >= 10)
            {
                msg_led_cnt = 0;
                mqtt_msg_event = false;
            }
            msg_led_cnt++;
        } else {
            set_led_color(2, LED_OFF);
        }

        if(mqtt_msg_transmit) {
            set_led_color(4, LED_BLUE); // A led
            mqtt_msg_transmit = false;
        } else {
            set_led_color(4, LED_OFF); // A led
        }
        // set_led_color(4, LED_OFF); // A led 0x0303FC
        set_led_color(5, LED_OFF); // B led 0x03FC03
    
        bsp_led_write(led_buffer, sizeof(led_buffer));
        vTaskDelay(pdMS_TO_TICKS(500)); // Delay 1 second
    }
}

static void mqtt_publish_task(void* pvParameters) {
    char* text = (char*)pvParameters;
    if(client) esp_mqtt_client_publish(client, "/esp32/coffee", text, 0, 1, 0);
    mqtt_msg_transmit = true;
    vTaskDelete(NULL);
}

int selected_button = 0;

// Example callback handlers
void nyanButton_Action() {
    printf("Button 1 pressed!\n");
    if(client) esp_mqtt_client_publish(client, "/esp32/coffee", "Event: Nyan", 0, 1, 0);
    // xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, "Event: Nyan", 1, NULL);
}

void coffeeButton_Action() {
    printf("Button 2 pressed!\n");
    if(client) esp_mqtt_client_publish(client, "/esp32/coffee", "Event: Coffee", 0, 1, 0);
    // xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, "Event: Coffee", 1, NULL);
}

void lunchButton_Action() {
    printf("Button 3 pressed!\n");
    if(client) esp_mqtt_client_publish(client, "/esp32/coffee", "Event: Lunch", 0, 1, 0);
    // xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, "Event: Lunch", 1, NULL);
}

// Hook button callbacks
void (*button_callbacks[])() = {
    nyanButton_Action,
    coffeeButton_Action,
    lunchButton_Action
};

// Drawing functions
void draw_header(pax_buf_t *buf) {
    // pax_draw_rect(buf, pax_col_rgb(50, 50, 200), 0, 0, display_v_res, HEADER_HEIGHT);

    //	pax_buf_t *buf, pax_col_t color, float x0, y0, x1, y1
    pax_draw_line(buf, 0xFF2B2C3A, 10, HEADER_HEIGHT, display_v_res - 20, HEADER_HEIGHT);
    pax_draw_text(buf, 0xFF2B2C3A, pax_font_sky_mono, 18, 5, 5, menu_title);
    pax_draw_text(buf, 0xFF2B2C3A, pax_font_sky_mono, 18, display_h_res - 35, 5, connection_status);
}

void draw_footer(pax_buf_t *buf) {
    pax_draw_line(buf, 0xFF2B2C3A, 10, display_h_res - FOOTER_HEIGHT, display_v_res - 20, display_h_res - FOOTER_HEIGHT);

    // pax_draw_rect(buf, pax_col_rgb(30, 30, 200), 0, display_h_res - FOOTER_HEIGHT, display_v_res, FOOTER_HEIGHT);
    pax_draw_text(buf, 0xFFFFFFFF, pax_font_sky_mono, 16, 5, display_v_res - FOOTER_HEIGHT, footer_text);
}

void draw_buttons(pax_buf_t *buf) {
    float start_x = (display_h_res - (NUM_BUTTONS * BUTTON_WIDTH + (NUM_BUTTONS - 1) * BUTTON_GAP)) / 2;
    float y = HEADER_HEIGHT + 40;

    for (int i = 0; i < NUM_BUTTONS; i++) {
        // pax_col_t color = (i == selected_button) ? pax_col_rgb(255, 255, 0) : pax_col_rgb(100, 100, 100);
        pax_col_t color = pax_col_rgb(100, 100, 100);
        pax_col_t highlight_color = pax_col_rgb(150, 150, 150);
        float x = start_x + i * (BUTTON_WIDTH + BUTTON_GAP);
        pax_outline_rect(buf, color, x, y, BUTTON_WIDTH, BUTTON_HEIGHT);
        if((i == selected_button))
            pax_draw_rect(buf, highlight_color, x, y, BUTTON_WIDTH, BUTTON_HEIGHT);
        pax_draw_text(buf, 0xFF000000, pax_font_sky_mono, 16, x + 10, y + 42, buttons[i]);
    }
}

void draw_text_field(pax_buf_t *buf){
    uint16_t offset_y = display_h_res - FOOTER_HEIGHT - TEXT_FIELD_HEIGTH;
    if (xSemaphoreTake(line_mutex, portMAX_DELAY)) {
    // pax_draw_rect(buf, 0xFF2B2C3A, 0, offset_y, display_v_res, TEXT_FIELD_HEIGTH);
    pax_draw_text(buf, 0xFF2B2C3A, pax_font_sky_mono, 18, 5, offset_y, line_buffer);
    // for (int line = 0; line < num_lines; line++) {
    //     if (line_index[line] >= 0) {
    //         // printf("%d: %d = %s\r\n", line, line_index[line], line_buffers[line_index[line]]);
    //     } else {
    //         // printf("%d: (empty)\r\n", line);
    //     }
    // }
    xSemaphoreGive(line_mutex);
    }
}

void selectNextButton(bool indexRight)
{
    if(indexRight)
    {
        selected_button++;

    } else {
        selected_button--;
    }

    if(selected_button > 2) selected_button = 0;
    else if (selected_button < 0) selected_button = 2;
}

void render_gui() {
    pax_background(&fb, pax_col_rgb(220, 220, 220));
    draw_header(&fb);
    draw_buttons(&fb);
    draw_footer(&fb);
    draw_text_field(&fb);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

void render_wallpaper_clock(bool includeClock) {
    pax_insert_png_buf(&fb, wallpaper_start, wallpaper_end - wallpaper_start, 0, 0, 0);
    pax_draw_text(&fb, 0xFFFFFFFF, pax_font_sky_mono, 40, 180, 380, menu_title);
    //TODO: render text Event Notifier on wallpper
    if(includeClock){
        char strftime_buf[64];

        time(&now_time);
        localtime_r(&now_time, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%H:%M:%S", &timeinfo);
        pax_draw_text(&fb, 0xFFFFFFFF, pax_font_sky_mono, 100, 100, 140, strftime_buf);
    }
    //TODO: show current time 
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));

}

void blit() {
    if(!inactive_show_time) render_gui();
    else render_wallpaper_clock(true);
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    // printf("mqtt event");
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            client = event->client;
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(client, "/esp32/coffee", 0);
            mqtt_msg_transmit = true;
            break;
        case MQTT_EVENT_DATA:
            char buffer[num_chars];
            memset(buffer, 0, sizeof(buffer));
            snprintf(buffer, sizeof(buffer), "data: %.*s\r\n", event->data_len, event->data);
            add_line(buffer);
            mqtt_msg_event = true;
            break;
        default:
            break;
    }
}

static void mqtt_task(void* pvParameters) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_mqtt_client_start(client);
    vTaskDelete(NULL);
}

// static void initialize_sntp_task(void* pvParameters) {
//     esp_sntp_init();
//     esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL); // Use polling
//     esp_sntp_setservername(0, "pool.ntp.org");     // Use a public NTP server

//     // Wait for the system time to be set
//     time_t now = 0;
//     struct tm timeinfo = { 0 };
//     int retry = 0;
//     const int retry_count = 10;

//     while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
//         ESP_LOGI("sntp", "Waiting for system time to be set... (retry %d)", retry);
//         vTaskDelay(2000 / portTICK_PERIOD_MS);
//         time(&now);
//         localtime_r(&now, &timeinfo);
//     }

//     // If the time is not set after multiple retries, print an error
//     if (timeinfo.tm_year < (2016 - 1900) || retry >= retry_count) {
//         ESP_LOGE("sntp", "Failed to sync with NTP server after %d retries", retry);
//     } else {
//         ESP_LOGI("sntp", "Time has been set successfully from NTP server");
//     }

//     vTaskDelete(NULL);
// }


static void wifi_connection_task(void* pvParameters) {
    wifi_connect_try_all();
    
    if(wifi_connection_is_connected()) {
        if(!mqtt_initialized)
        {
            mqtt_initialized = true;
            xTaskCreate(mqtt_task, "mqtt_task", 8192, NULL, 8, NULL);
            // xTaskCreate(initialize_sntp_task, "initialize_sntp_task", 8192, NULL, 8, NULL);
        }
        wifi_connected = true;
        wifi_connecting = false;
        esp_netif_ip_info_t* ip_info = wifi_get_ip_info();
        add_line(ip4addr_ntoa((const ip4_addr_t*)&ip_info->ip));
    }
    vTaskDelete(NULL);
}

static void render_task(void* pvParameters) {
    while(1) {
        blit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


esp_err_t apply_timezone(void) {
    char tzstring[64] = {0};
    nvs_handle_t nvs_handle;
    esp_err_t    res = nvs_open("system", NVS_READWRITE, &nvs_handle);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return res;
    }
    size_t size = 0;
    res         = nvs_get_str(nvs_handle, "tz", NULL, &size);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to find NVS entry");
        nvs_close(nvs_handle);
        return res;
    }
    if (size > sizeof(tzstring)) {
        ESP_LOGE(TAG, "Value in NVS is too long");
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }
    res = nvs_get_str(nvs_handle, "tz", tzstring, &size);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read NVS entry");
        nvs_close(nvs_handle);
        return res;
    }
    nvs_close(nvs_handle);

    setenv("TZ", tzstring, 1);
    tzset();

    return res;
}


void app_main(void) {
    init();
    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    ESP_ERROR_CHECK(bsp_device_initialize());


    apply_timezone();


    ESP_LOGW(TAG, "Switching radio off...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "Switching radio to application mode...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_ERROR_CHECK(bsp_display_get_panel(&lcd_panel));
    
    // Get display parameters and rotation
    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    ESP_ERROR_CHECK(res);  // Check that the display parameters have been initialized
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    
    // Convert ESP-IDF color format into PAX buffer type
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (display_color_format) {
        case LCD_COLOR_PIXEL_FORMAT_RGB565:
        format = PAX_BUF_16_565RGB;
        break;
        case LCD_COLOR_PIXEL_FORMAT_RGB888:
        format = PAX_BUF_24_888RGB;
        break;
        default:
        break;
    }
    
    // Convert BSP display rotation format into PAX orientation type
    pax_orientation_t      orientation      = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:
        orientation = PAX_O_ROT_CCW;
        break;
        case BSP_DISPLAY_ROTATION_180:
        orientation = PAX_O_ROT_HALF;
        break;
        case BSP_DISPLAY_ROTATION_270:
        orientation = PAX_O_ROT_CW;
        break;
        case BSP_DISPLAY_ROTATION_0:
        default:
        orientation = PAX_O_UPRIGHT;
        break;
    }
    
    // Initialize graphics stack
    pax_buf_init(&fb, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb, orientation);

    render_wallpaper_clock(false);

    if (wifi_remote_initialize() == ESP_OK) {
        wifi_connection_init_stack();        
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, did you flash ESP-HOSTED firmware?");
        return;
    }
    
    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
    
    ESP_LOGW(TAG, "Hello world!");
    
    
    bsp_led_initialize();
    xTaskCreate(led_task, "led_task", 4096, NULL, 5, NULL);
    
    // bool sdcard_inserted = false;
    // bsp_input_read_action(BSP_INPUT_ACTION_TYPE_SD_CARD, &sdcard_inserted);
    
    // if (sdcard_inserted) {
    //     printf("SD card detected\r\n");
    //     #if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL) || defined(CONFIG_BSP_TARGET_HACKERHOTEL_2026)
    //     sd_pwr_ctrl_handle_t sd_pwr_handle = initialize_sd_ldo();
    //     sd_mount_spi(sd_pwr_handle);
    //     sd_card_present = true;
    //     test_sd();
    //     #endif
    // }
    
    // xTaskCreate(wifi_task, "wifi_task", 8192, NULL, 10, NULL);
    xTaskCreate(render_task, "render_task", 4096, NULL, 10, NULL);

    while (1) {
        //TODO: 
        // 1. Show big clock by default, when any button is pressed show graphical interface - done
        // 2. Add graphics interface with buttons. - done
        // 3. Parse json data received from mqtt
        // 4. generate json data to be transmitted
        // 5. Add encryption for messages
        // 6. Add player that shows gifs on screen
        // 7. read mqtt settings from sd card. Ask to continue with default settings if no sd card present
        // 8. Add wallpaper - done

        bsp_input_event_t event;
        if(!wifi_connection_is_connected() && !wifi_connecting) {
            wifi_connected = false;
            wifi_connecting = true;
            xTaskCreate(wifi_connection_task, "wifi_connection_task", 4096, NULL, 10, NULL);
        }
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    if (event.args_navigation.state) {
                        switch (event.args_navigation.key) {
                            case BSP_INPUT_NAVIGATION_KEY_F1:
                                // xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, "Debug: I require coffee!", 1, NULL);
                                if(client) esp_mqtt_client_publish(client, "/esp32/coffee", "Debug: I require coffee!", 0, 1, 0);
                                // mqtt_msg_transmit = true;
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                                selectNextButton(true);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_LEFT:
                                selectNextButton(false);
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RETURN:
                                button_callbacks[selected_button]();
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_ESC:
                                inactive_show_time ^= 1;
                                break;
                            default:
                            break;

                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}