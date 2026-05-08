#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
       
#include "freertos/semphr.h"        // mutex / semaphore
#include "freertos/event_groups.h"  // event group
#include "driver/gpio.h"
#include "dht11.h"
#include "ssd1306.h"
#include "driver/adc.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

// ---------- Globals & FreeRTOS objects ----------
static int led_state_global = 0; 
dht11_t dht11_sensor;
static int mode = 0; // 0 = Manual, 1 = Auto

// GPIO define
#define CONFIG_DHT11_PIN     GPIO_NUM_3
#define BUTTON_GPIO          GPIO_NUM_2
#define LED_GPIO             GPIO_NUM_5
#define LDR_ADC_CHANNEL   ADC1_CHANNEL_4   

#define CONFIG_CONNECTION_TIMEOUT 5
#define BUZZER_GPIO  GPIO_NUM_7

// NEW: FreeRTOS objects
static SemaphoreHandle_t data_mutex = NULL;         
static EventGroupHandle_t sys_event_group = NULL;   
static TaskHandle_t ledTaskHandle = NULL;          
static int light_level = 0;

// Event bits
#define BIT_LED_ON      (1 << 0)
#define BIT_MODE_AUTO   (1 << 1)
#define BIT_OVER_TEMP   (1 << 2)

// ---------- WEB HANDLER ----------
// Handler send file HTML from LittleFS/SPIFFS
static esp_err_t root_get_handler(httpd_req_t *req) {
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[]   asm("_binary_index_html_end");
    size_t index_html_len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_len);
    return ESP_OK;
}

// Handler send JSON DHT11
static esp_err_t data_get_handler(httpd_req_t *req) {
    char json_resp[128];

    
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    int temp = (int)dht11_sensor.temperature;
    int hum  = (int)dht11_sensor.humidity;
    int led  = led_state_global;
    int m    = mode;
    xSemaphoreGive(data_mutex);

    snprintf(json_resp, sizeof(json_resp),
        "{\"temp\":%d,\"hum\":%d,\"led\":%d,\"mode\":\"%s\"}",
        temp, hum, led, m ? "Auto" : "Manual");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler toggle BULB (web)
esp_err_t toggle_handler(httpd_req_t *req) {
    // chỉ toggle khi ở Manual
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    int m = mode;
    xSemaphoreGive(data_mutex);

    if (m == 0) {   // Manual
        
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        int new_state = !led_state_global;
        xSemaphoreGive(data_mutex);

        if (ledTaskHandle != NULL) {
            // notify with new_state (0 hoặc 1)
            xTaskNotify(ledTaskHandle, (uint32_t)new_state, eSetValueWithOverwrite);
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


static esp_err_t mode_handler(httpd_req_t *req) {
    
    xSemaphoreTake(data_mutex, portMAX_DELAY);
    mode = !mode;
    int m = mode;
    xSemaphoreGive(data_mutex);

    if (m) {
        xEventGroupSetBits(sys_event_group, BIT_MODE_AUTO);
    } else {
        xEventGroupClearBits(sys_event_group, BIT_MODE_AUTO);
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// --- URI ---
static const httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
static const httpd_uri_t uri_data = { .uri = "/data", .method = HTTP_GET, .handler = data_get_handler };
static const httpd_uri_t uri_toggle = { .uri = "/toggle", .method = HTTP_GET, .handler = toggle_handler };
static const httpd_uri_t uri_mode = { .uri = "/mode", .method = HTTP_GET, .handler = mode_handler };

// Start web server
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_data);
        httpd_register_uri_handler(server, &uri_toggle);
        httpd_register_uri_handler(server, &uri_mode);
    }
    return server;
}

// Start WiFi AP
void wifi_init_softap(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_AP",
            .ssid_len = strlen("ESP32_AP"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen("12345678") == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    printf("WiFi AP started, SSID: ESP32_AP, Password: 12345678\n");
}

// ---------- Tasks ----------

// Task read DHT11 and display OLED
void dht11_task(void *pvParameter) {
    char temp_str[20];
    char humidity_str[25];
    char mode_str[20];
    char led_str[20];
    static uint8_t prev_temp = 0xFF;
    static uint8_t prev_hum  = 0xFF;
    static int prev_mode = -1;
    static int prev_led  = -1;
    char light_str[25];
    static int prev_light = -1;
    while (1) {
        if (!dht11_read(&dht11_sensor, CONFIG_CONNECTION_TIMEOUT)) {
            uint8_t curr_temp = (uint8_t)dht11_sensor.temperature;
            uint8_t curr_hum  = (uint8_t)dht11_sensor.humidity;
            printf("Temperature: %d°C, Humidity: %d%%\n", curr_temp, curr_hum);
            
            // UPDATE shared dht11_sensor under mutex 
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            dht11_sensor.temperature = curr_temp;
            dht11_sensor.humidity = curr_hum;
            xSemaphoreGive(data_mutex);

            // quản lý event OVER_TEMP
            if (curr_temp > 40) {
                xEventGroupSetBits(sys_event_group, BIT_OVER_TEMP);
            } else {
                xEventGroupClearBits(sys_event_group, BIT_OVER_TEMP);
            }

            
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            int led_state = led_state_global;
            int m = mode;
            int curr_light = light_level;
            xSemaphoreGive(data_mutex);

            if (curr_temp != prev_temp || curr_hum != prev_hum || 
                m != prev_mode || led_state != prev_led || curr_light != prev_light) {

                ssd1306_clear();

                sprintf(temp_str, "Temp: %dC", curr_temp);
                sprintf(humidity_str, "Humidity: %d%%", curr_hum);
                sprintf(mode_str, "Mode: %s", m ? "Auto" : "Manual");
                sprintf(led_str, "BULB: %s", led_state ? "ON" : "OFF");
                sprintf(light_str, "Light: %d", curr_light);
                ssd1306_print_str(8, 8, temp_str, false);
                ssd1306_print_str(8, 20, humidity_str, false);
                ssd1306_print_str(8, 32, mode_str, false);
                ssd1306_print_str(8, 44, led_str, false);
                ssd1306_print_str(8, 56, light_str, false);
                ssd1306_display();

                prev_temp = curr_temp;
                prev_hum  = curr_hum;
                prev_mode = m;
                prev_led  = led_state;
                prev_light = curr_light;
            }
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

// Task button 
void button_task(void *pvParameter) {
    int last_state = 1;  
    TickType_t last_press_time = 0;

    while (1) {
        int current_state = gpio_get_level(BUTTON_GPIO);

        if (last_state == 1 && current_state == 0) {
            TickType_t now = xTaskGetTickCount();

            if ((now - last_press_time) < (500 / portTICK_PERIOD_MS)) {
                // double click -> đổi mode
                xSemaphoreTake(data_mutex, portMAX_DELAY);
                mode = !mode;
                int m = mode;
                xSemaphoreGive(data_mutex);

                if (m) xEventGroupSetBits(sys_event_group, BIT_MODE_AUTO);
                else   xEventGroupClearBits(sys_event_group, BIT_MODE_AUTO);

                printf("Double click detected -> mode = %d\n", mode);
            } else {
                // single click -> toggle khi Manual
                xSemaphoreTake(data_mutex, portMAX_DELAY);
                int m = mode;
                xSemaphoreGive(data_mutex);

                if (m == 0) { // Manual
                    xSemaphoreTake(data_mutex, portMAX_DELAY);
                    int new_state = !led_state_global;
                    xSemaphoreGive(data_mutex);

                    if (ledTaskHandle != NULL) {
                        xTaskNotify(ledTaskHandle, (uint32_t)new_state, eSetValueWithOverwrite);
                    }
                }
            }

            last_press_time = now;
            vTaskDelay(200 / portTICK_PERIOD_MS); // debounce
        }

        last_state = current_state;
        vTaskDelay(20 / portTICK_PERIOD_MS); 
    }
}

void light_task(void *pvParameter) {
    while (1) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
        int m = mode;
        xSemaphoreGive(data_mutex);

        
        int adc_raw = adc1_get_raw(LDR_ADC_CHANNEL);

        xSemaphoreTake(data_mutex, portMAX_DELAY);
        light_level = adc_raw;      
        xSemaphoreGive(data_mutex);

        if (m == 1) { // Auto
            int led_state = (adc_raw < 2000) ? 0 : 1;  

            if (ledTaskHandle != NULL) {
                xTaskNotify(ledTaskHandle, (uint32_t)led_state, eSetValueWithOverwrite);
            }
        }

        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
}

// LED task 
void led_task(void *pvParameters) {
    uint32_t led_state;
    while (1) {
        
        if (xTaskNotifyWait(0x00, 0xFFFFFFFF, &led_state, portMAX_DELAY) == pdTRUE) {
            
            xSemaphoreTake(data_mutex, portMAX_DELAY);
            led_state_global = (int)led_state;
            xSemaphoreGive(data_mutex);

            gpio_set_level(LED_GPIO, led_state_global);

            if (led_state_global)
                xEventGroupSetBits(sys_event_group, BIT_LED_ON);
            else
                xEventGroupClearBits(sys_event_group, BIT_LED_ON);
        }
    }
}

// Buzzer task 
void buzzer_task(void *pvParameter) {
    while (1) {
        EventBits_t bits = xEventGroupGetBits(sys_event_group);
        if (bits & BIT_OVER_TEMP) {
            gpio_set_level(BUZZER_GPIO, 1);  // on
        } else {
            gpio_set_level(BUZZER_GPIO, 0);  // off
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS); 
    }
}

// ---------- app_main ----------
void app_main() {
    // Init DHT11
    dht11_sensor.dht11_pin = CONFIG_DHT11_PIN;

    // Init OLED
    init_ssd1306();

    // Init LED GPIO
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    // Init Button GPIO
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_GPIO);   // pull-up

    // Init LDR GPIO
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LDR_ADC_CHANNEL, ADC_ATTEN_DB_11);


    // Init Buzzer GPIO
    gpio_reset_pin(BUZZER_GPIO);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0); 

    // NEW: create mutex & event group
    data_mutex = xSemaphoreCreateMutex();
    if (data_mutex == NULL) {
        printf("Failed to create data_mutex\n");
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    sys_event_group = xEventGroupCreate();
    if (sys_event_group == NULL) {
        printf("Failed to create sys_event_group\n");
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // Init NVS to WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // start WiFi AP
    wifi_init_softap();

    // start web server
    start_webserver();

    // create tasks 
    xTaskCreate(led_task, "led_task", 2048, NULL, 4, &ledTaskHandle);
    xTaskCreate(dht11_task, "dht11_task", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button_task", 2048, NULL, 6, NULL);
    xTaskCreate(light_task, "light_task", 2048, NULL, 5, NULL);
    xTaskCreate(buzzer_task, "buzzer_task", 2048, NULL, 5, NULL);
}
