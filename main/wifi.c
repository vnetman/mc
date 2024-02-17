#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "mc.h"

#ifdef NEVER
#include <string.h>
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "nvs_flash.h"
#endif

/* FreeRTOS event group to signal when we are connected*/

static BaseType_t wifi_connect_retry = 0;

static void wifi_event_handler (void* arg, esp_event_base_t event_base,
				int32_t event_id, void* event_data) {
  EventGroupHandle_t mc_event_group = (EventGroupHandle_t) arg;
  
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    ESP_LOGI("mc", "wifi: WIFI_EVENT_STA_START, invoking esp_wifi_connect()");
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (wifi_connect_retry < 3) {
      ESP_LOGI("mc", "wifi: disconnected, will retry (%d)", wifi_connect_retry);
      esp_wifi_connect();
      wifi_connect_retry++;
    } else {
      xEventGroupSetBits(mc_event_group, EVENT_WIFI_FAILED);
      ESP_LOGI("mc","wifi: connection to the AP failed");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    ESP_LOGI("mc", "wifi: connected, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    wifi_connect_retry = 0;
    xEventGroupSetBits(mc_event_group, EVENT_WIFI_CONNECTED);
  }
}

void start_wifi (EventGroupHandle_t mc_event_group) {
  esp_event_handler_instance_t wifi_event_instance;
  esp_event_handler_instance_t ip_event_instance;
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  wifi_config_t wifi_config = {
    .sta = {
      .ssid = CONFIG_WLM_WIFI_SSID,
      .password = CONFIG_WLM_WIFI_PASSWORD
    },
  };

  ESP_ERROR_CHECK(esp_netif_init());

  esp_netif_create_default_wifi_sta();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
						      &wifi_event_handler,
						      (void *) mc_event_group,
						      &wifi_event_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
						      &wifi_event_handler,
						      (void *) mc_event_group,
						      &ip_event_instance));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI("mc", "wifi: start_wifi finished.");
}
