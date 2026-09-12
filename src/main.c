#include <stdio.h>  //standard C library for printing
#include "freertos/FreeRTOS.h"   //The Manager OS
#include "freertos/task.h"   // The "Task" system
#include "driver/gpio.h"     // The "Hardware" driver for pins
#include "freertos/queue.h"  // The FreeRTOS queues: send/receive data between tasks
#include "esp_log.h"         // Logging
#include "nvs_flash.h"       // NVS - Non Volatile Storage
#include "nvs.h"             // NVS helper functions
#include <stdint.h>          // Fixed-width integer types like int32_t, int16_t
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"           // Library to create JSON data
#include "mqtt_client.h"     // MQTT functionality
#include "esp_timer.h"       // Software timer - Replace vdelay


#define RED_PIN 2
#define GREEN_PIN 18
#define BLUE_PIN 19
#define BUTTON_PIN 4

// Struct to bundle voltage + current + charger state
typedef struct {
  int state;
  float voltage;
  float current;
  uint32_t uptime_s;
} charger_data_t;

// Global handle for the MQTT client
esp_mqtt_client_handle_t client;

static const char *TAG = "IOT_NODE";     // ID for logs

// Global struct to keep track of last known values
charger_data_t last_known_data = { .state = 0, .voltage = 230.0, .current = 0.0 };

// Timer object reference and variable to remember led state
esp_timer_handle_t blink_timer;
bool led_state_toggle = false;

// The "Worker" function - Blink red LED every 250ms
void blink_timer_callback(void* arg) {
    led_state_toggle = !led_state_toggle;
    gpio_set_level(RED_PIN, led_state_toggle ? 0 : 1);
}

// Define the queue
QueueHandle_t state_mailbox;

// Save state to memory
void save_state_to_nvs(int state) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err == ESP_OK) {
    nvs_set_i32(my_handle, "last_state", state);
    nvs_commit(my_handle);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "State %d saved to NVS", state);
  }
}

// Read state from memory
int read_state_from_nvs() {
  nvs_handle_t my_handle;
  int32_t saved_state = 0; // Default to 0 (IDLE)
  esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
  if (err == ESP_OK) {
    nvs_get_i32(my_handle, "last_state", &saved_state);
    nvs_close(my_handle);
    ESP_LOGI(TAG, "Restored state %ld from NVS", saved_state);
  }
  return (int)saved_state;
}

// TASK: LED Lights worker
void LedTask(void *pvParameter){
  // worker's private/local copy
  charger_data_t internal_bundle;
  internal_bundle.state = 0;
  int last_logged_state = -1;

  // Initialize the blink timer
  const esp_timer_create_args_t blink_timer_args = {
    .callback = &blink_timer_callback,
    .name = "blink_timer"
  };
  esp_timer_create(&blink_timer_args, &blink_timer);

  while(1){
    // Check the queue
    xQueueReceive(state_mailbox, &internal_bundle, pdMS_TO_TICKS(100));

    // Only log if the state is changed:
    if (internal_bundle.state != last_logged_state) {
        // Stop timer whenever the state changes
        esp_timer_stop(blink_timer);

        // Turn everything off
        gpio_set_level(RED_PIN, 1);
        gpio_set_level(GREEN_PIN, 1);
        gpio_set_level(BLUE_PIN, 1);

      switch (internal_bundle.state) {
        case 0:
            gpio_set_level(BLUE_PIN, 0);  // Blue = IDLE
            ESP_LOGI(TAG, ">>> STATUS CHANGE: Charger is now IDLE (Blue LED)");
            break;
        case 1:
            gpio_set_level(GREEN_PIN, 0); // Green = CHARGING
            ESP_LOGI(TAG, ">>> STATUS CHANGE: Vehicle Connected. CHARGING... (Green LED)");
            break;
        case 2:
            esp_timer_start_periodic(blink_timer, 250000);
            ESP_LOGW(TAG, ">>> ALERT: System FAULT detected! (Red Blinking)");
            break;
        default:
          ESP_LOGE(TAG, "Unknown state received: %d", internal_bundle.state);
      }
      last_logged_state = internal_bundle.state; // Update memory of the last log
    }

  }
}

// Fix ghost data: delay between button press & cloud seeing the change
void publish_status(charger_data_t data) {
    // Don't publish if MQTT client hasn't started yet
    if (client == NULL) {
        ESP_LOGW(TAG, "MQTT not ready, skipping publish");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", "NODE_JAMILA_01");

    // Calculate uptime: microseconds to seconds
    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "uptime_s", uptime);

    cJSON_AddNumberToObject(root, "voltage", data.voltage);
    cJSON_AddNumberToObject(root, "current", data.current);
    cJSON_AddNumberToObject(root, "state", data.state);

    char *json_str = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(client, "iot-node/device/jamila_01/status", json_str, 0, 1, 0);
    
    cJSON_Delete(root);
    free(json_str);
}

// Sensor Task to simulate sensor data
void SensorTask(void *pvParameter){
    charger_data_t data;

    while(1){
        data.state = read_state_from_nvs(); // Get saved state from memory (every 5secs)

        // Random - 220V + random decimal
        data.voltage = 220.0 + ((float)rand() / (float)(RAND_MAX)) * 20.0;
        data.current = (data.state == 1) ? 12.5 : 0.0;

        // Send data to LED Task through queue
        xQueueSend(state_mailbox, &data, 0);

        // Send to cloud
        publish_status(data);

        // Set global struct
        last_known_data = data;

        ESP_LOGI("SENSOR", "Reading: %.2fV, %.2fA, %d", data.voltage, data.current, data.state);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// MQTT Event Handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // Cast the incoming data to the correct pointer type
    esp_mqtt_event_handle_t event = event_data; 
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            esp_mqtt_client_subscribe(client, "iot-node/device/jamila_01/cmd", 0);
            break;

        case MQTT_EVENT_DATA:
            if (strncmp(event->data, "START_CHARGE", event->data_len) == 0) {
                ESP_LOGI(TAG, "Cloud Command: START");
                int new_state = 1;
                save_state_to_nvs(new_state);
                charger_data_t cmd_data = { .state = new_state };
                xQueueSend(state_mailbox, &cmd_data, 0);
            } 
            else if (strncmp(event->data, "STOP_CHARGE", event->data_len) == 0) {
                ESP_LOGI(TAG, "Cloud Command: STOP");
                int new_state = 0;
                save_state_to_nvs(new_state);
                charger_data_t cmd_data = { .state = new_state };
                xQueueSend(state_mailbox, &cmd_data, 0);
            }
            break;
        default:
            break;
    }
}

void mqtt_app_start(void){
    esp_mqtt_client_config_t mqtt_cfg ={
        .broker.address.uri = "mqtt://broker.hivemq.com",
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

// Wi-Fi handler - Called automatically by the system when Wi-Fi events happen
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi Connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGW(TAG, "Retrying WiFi Connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));

        // Start MQTT
        mqtt_app_start();
    }
}

// Init function
void wifi_init_sta(void) {
    // 1. Initialize the underlying TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 2. Create the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Register the handler function
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,&wifi_event_handler,NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,&wifi_event_handler,NULL, NULL));

    // 5. Setup credentials from platformio.ini
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_CHARGER_WIFI_SSID,
            .password = CONFIG_CHARGER_WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}


void app_main() {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize Wi-Fi
  wifi_init_sta();

  // Hardware Setup
  gpio_reset_pin(RED_PIN);
  gpio_reset_pin(GREEN_PIN);
  gpio_reset_pin(BLUE_PIN);
  gpio_reset_pin(BUTTON_PIN);

  gpio_set_direction(RED_PIN, GPIO_MODE_OUTPUT);
  gpio_set_direction(GREEN_PIN, GPIO_MODE_OUTPUT);
  gpio_set_direction(BLUE_PIN, GPIO_MODE_OUTPUT);
  gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);

  // Restore last state from memory
  int local_state = read_state_from_nvs();

  charger_data_t initial_data = { .state = local_state, .voltage = 0, .current = 0 };

  // Create queue
//   state_mailbox = xQueueCreate(5, sizeof(charger_data_t));
  state_mailbox = xQueueCreate(1, sizeof(charger_data_t));

  // Send initial restored state to the LED task immediately
  xQueueSend(state_mailbox, &initial_data, 10);

  // Start Led worker
  xTaskCreate(
    LedTask,   // Function name
    "LED_Manager",    // Name for debugging
    2048,        // Stack size (memory)
    NULL,        // Parameters to pass
    5,           // Priority 
    NULL         // Task handle
  );

  // Start Sensor Worker
  xTaskCreate(
    SensorTask,
    "Sensor_Manager",
    4096,
    NULL,
    2,
    NULL
  );


  int last_btn_state = 1;
  ESP_LOGI(TAG, "System Started. Current State: %d", local_state);

  // Main loop to handle button click
  while(1) {

    int btn_state = gpio_get_level(BUTTON_PIN);

    // Logic Fix: 0 is PRESSED when using a Pull-up to GND
    if(last_btn_state == 1 && btn_state == 0){
        local_state = (local_state + 1) % 3; 

        // Save to NVS
        save_state_to_nvs(local_state);

        // Update the global data with the new state
        last_known_data.state = local_state;

        // Send state to queue
        // xQueueSend(state_mailbox, &last_known_data, portMAX_DELAY);
        xQueueOverwrite(state_mailbox, &last_known_data);

        // 2. Tell the Cloud (Instant Update)
        publish_status(last_known_data);

        vTaskDelay(pdMS_TO_TICKS(200)); // Simple Debounce
    }
    last_btn_state = btn_state;

    vTaskDelay(pdMS_TO_TICKS(10)); // Keep CPU responsive
  }
}
