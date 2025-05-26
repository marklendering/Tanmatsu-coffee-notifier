#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
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


#define num_lines 17
#define num_chars 60

char line_buffers[num_lines][num_chars];
int  line_index[num_lines];
char input_buffer[num_chars];
uint8_t led_buffer[6 * 3] = {0};

uint8_t msg_led_cnt = 0;
bool mqtt_msg_event = false;
bool mqtt_msg_transmit = false;
bool wifi_connected = false;
bool wifi_connecting = false;
bool sd_card_present = false;


void init(void) {
    line_mutex = xSemaphoreCreateMutex();
    for (size_t i = 0; i < num_lines; i++) {
        line_index[i] = i;
    }
}

void add_line(char* text) {
    if (xSemaphoreTake(line_mutex, portMAX_DELAY)) {
        for (size_t i = 0; i < strlen(text); i++) {
            if (text[i] == '\r' || text[i] == '\n') {
                text[i] = ' ';
            }
        }
        for (size_t i = 0; i < num_lines; i++) {
            line_index[i] = (line_index[i] + 1) % num_lines;
        }
        strncpy(line_buffers[line_index[num_lines - 1]], text, num_chars);
        xSemaphoreGive(line_mutex);
    }
}

// static void wifi_task(void* pvParameters) {
//     if (wifi_remote_initialize() == ESP_OK) {
//         wifi_connection_init_stack();
//         wifi_connect_try_all();
//     } else {
//         bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
//         ESP_LOGE(TAG, "WiFi radio not responding, did you flash ESP-HOSTED firmware?");
//     }
//     wifi_stack_task_done = true;
//     vTaskDelete(NULL);
// }

void set_led_color(uint8_t led, uint32_t color) {
    led_buffer[led * 3 + 0] = (color >> 8) & 0xFF;  // G
    led_buffer[led * 3 + 1] = (color >> 16) & 0xFF; // R
    led_buffer[led * 3 + 2] = (color >> 0) & 0xFF;  // B
}

static void led_task(void* pvParameters) {
    while (1) {
        set_led_color(0, LED_GREEN); // power led, change color depending on battery level
        if(wifi_connected) set_led_color(1, LED_GREEN); // wifi led
        else if(wifi_connecting) set_led_color(1, LED_YELLOW);
        else set_led_color(1, LED_RED);

        if(mqtt_msg_event) {
            //todo: toggle led with 1hz frequency
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

void blit() {
    // pax_background(&fb, 0xFF64E38F);
    pax_background(&fb, 0xFFFFFFFF);
    printf("----\r\n");
    for (int line = 0; line < num_lines; line++) {
        if (line_index[line] >= 0) {
            printf("%d: %d = %s\r\n", line, line_index[line], line_buffers[line_index[line]]);
            pax_draw_text(&fb, 0xFF2B2C3A, pax_font_sky_mono, 24, 0, 24 * line, line_buffers[line_index[line]]);
        } else {
            printf("%d: (empty)\r\n", line);
        }
    }
    printf("\r\n");
    pax_draw_rect(&fb, 0xFFFFFFFF, 0, pax_buf_get_height(&fb) - 24, pax_buf_get_width(&fb), 24);
    pax_draw_text(&fb, 0xFF000000, pax_font_sky_mono, 24, 0, pax_buf_get_height(&fb) - 24, input_buffer);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(client, "/esp32/coffee", 0);
            esp_mqtt_client_publish(client, "/esp32/coffee", "Hello MQTT", 0, 1, 0);
            mqtt_msg_transmit = true;
            // add_line("published data");
            // blit();
            break;
        case MQTT_EVENT_DATA:
            char buffer[num_chars];
            memset(buffer, 0, sizeof(buffer));
            // snprintf(buffer, sizeof(buffer), "topic: %.*s\r\n", event->topic_len, event->topic);
            // add_line(buffer);
            // ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            snprintf(buffer, sizeof(buffer), "data: %.*s\r\n", event->data_len, event->data);
            add_line(buffer);
            blit();
            // printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            // printf("DATA=%.*s\r\n", event->data_len, event->data);
            mqtt_msg_event = true;
            break;
        default:
            break;
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void) {
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
    
    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));
    
    ESP_LOGW(TAG, "Hello world!");
    
    init();
    blit();
    
    bsp_led_initialize();
    xTaskCreate(led_task, TAG, 4096, NULL, 10, NULL);
    
    bool sdcard_inserted = false;
    bsp_input_read_action(BSP_INPUT_ACTION_TYPE_SD_CARD, &sdcard_inserted);

    if (sdcard_inserted) {
        printf("SD card detected\r\n");
#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL) || \
    defined(CONFIG_BSP_TARGET_HACKERHOTEL_2026)
        sd_pwr_ctrl_handle_t sd_pwr_handle = initialize_sd_ldo();
        sd_mount_spi(sd_pwr_handle);
        sd_card_present = true;
        test_sd();
#endif
    }
    
     if (wifi_remote_initialize() == ESP_OK) {
        wifi_connection_init_stack();
        wifi_connecting = true;
        wifi_connect_try_all();
        if(wifi_connection_is_connected()) {
            wifi_connected = true;
            esp_netif_ip_info_t* ip_info = wifi_get_ip_info();
            add_line(ip4addr_ntoa((const ip4_addr_t*)&ip_info->ip));
            blit();
        } 
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio not responding, did you flash ESP-HOSTED firmware?");
        add_line("wifi radio fail");
        blit();
    }
    
    vTaskDelay(pdMS_TO_TICKS(500)); // make sure wifi is properly connected
    mqtt_app_start();

    while (1) {
        //TODO: 
        // 1. Show big clock by default, when any button is pressed show graphical interface
        // 2. Add graphics interface with buttons.
        // 3. Parse json data received from mqtt
        // 4 generate json data to be transmitted
        // 5. Add encryption for messages
        // 6. Add player that shows gifs on screen

        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            switch (event.type) {
                case INPUT_EVENT_TYPE_KEYBOARD: {
                    char ascii = event.args_keyboard.ascii;
                    if (ascii == '\b') {
                        size_t length = strlen(input_buffer);
                        if (length > 0) {
                            input_buffer[length - 1] = '\0';
                        }
                    } else if (ascii == '\r' || ascii == '\n') {
                        // Ignore
                    } else {
                        size_t length = strlen(input_buffer);
                        if (length < num_chars - 1) {
                            input_buffer[length]     = ascii;
                            input_buffer[length + 1] = '\0';
                        }
                    }
                    // blit();
                    break;
                }
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    if (event.args_navigation.state) {
                        /*if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
                            size_t length = strlen(input_buffer);
                            if (length > 0) {
                                input_buffer[length - 1] = '\0';
                            }
                        }*/
                        if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                            // uart_write_bytes(RADIO_UART, input_buffer, strlen(input_buffer));
                            // uart_write_bytes(RADIO_UART, "\r\n", 2);

                            // uart_write_bytes(RADIO_UART, (const char*)mock_tcp_packet, sizeof(mock_tcp_packet));

                            add_line(input_buffer);
                            memset(input_buffer, 0, num_chars);
                        }
                        // blit();
                        // if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                        //     wallpaper(h_res, v_res);
                        // }
                    }
                    break;
                }
                default:
                    break;
            }
        } 
        blit();
    }
}