#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_system.h"
#include "pins.h"
#include "esp_log.h"
#include "mc.h"

static char const *LOG_TAG = "mc|main";

void app_main() {
  BaseType_t ret;
  QueueHandle_t beep_q, motor_on_off_q, ota_q;
  EventGroupHandle_t mc_event_group;

  struct mc_task_args_t_ mc_task_args;
  
  init_gpio_pins();

  /* We'll start off by turning the error LED on, and turn it off once
     everything starts off fine */
  gpio_set_level(ERR_STATUS_OUT, 1);
  vTaskDelay(5000 / portTICK_PERIOD_MS);

  ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  /* Create the event group that the tasks in this application will use */
  mc_event_group = xEventGroupCreate();
  start_wifi(mc_event_group);

  /* Create the queue that sends beep requests */
  beep_q = xQueueCreate(1, sizeof(bool));
  if (beep_q == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create queue for beep task");
    return;
  }

  /* Create the queue that sends requests to turn the motor on/off */
  motor_on_off_q = xQueueCreate(1, sizeof(bool));
  if (motor_on_off_q == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create queue for motor task");
    return;
  }

  /* Create the queue that sends requests to upgrade the firmware over OTA */
  ota_q = xQueueCreate(1, sizeof(char *));
  if (ota_q == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create queue for OTA");
    return;
  }

  /* Prepare the task args data structure that we will pass to all the
     tasks that we will start shortly. */
  mc_task_args.mc_event_group = mc_event_group;
  mc_task_args.beep_q = beep_q;
  mc_task_args.motor_on_off_q = motor_on_off_q;
  mc_task_args.ota_q = ota_q;

  /* Start the task that accepts requests to sound the beep */
  ret = xTaskCreate(beep_task, "Beep Task", 2048, (void *) &mc_task_args,
		    tskIDLE_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create beep task");
    return;
  }
  
  /* Start the task that starts/stops/handles the HTTP server. */
  ret = xTaskCreate(http_server_task, "HTTP Server Task", 4096, &mc_task_args,
		    tskIDLE_PRIORITY,NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create http server task");
    return;
  }
  
  /* Start the task that handles UDP logging. */
  ret = xTaskCreate(udp_logging_task, "UDP Logging Task", 2048, &mc_task_args,
		    tskIDLE_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create UDP logger task");
    return;
  }
  
  /* Start the task that turns the motor on/off. */
  ret = xTaskCreate(motor_task, "Motor on/off Task", 2048, (void *) &mc_task_args,
		    tskIDLE_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create motor on/off task");
    return;
  }

  /* Start the task that monitors the tank level. */
  ret = xTaskCreate(oh_tank_level_task, "OH Tank Level Task", 2048, (void *) &mc_task_args,
		    tskIDLE_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create OH Tank Level task");
    return;
  }

  /* Start the OTA task */
  ret = xTaskCreate(ota_task, "OTA Task", 8192, (void *) &mc_task_args,
		    tskIDLE_PRIORITY, NULL);
  if (ret != pdPASS) {
    ESP_LOGE(LOG_TAG, "Failed to create OTA task");
    return;
  }
  
  /* Delay a bit to allow the tasks to start */
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  fflush(stdout);

  /* If we got here, that means everything started off fine, and we can turn the
     Err/Status LED off */
  gpio_set_level(ERR_STATUS_OUT, 0);
  
  while (pdTRUE) {
    vTaskDelay(portMAX_DELAY);
  }
}
