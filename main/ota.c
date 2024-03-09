#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "errno.h"
#include "mc.h"

#define BUFFSIZE 1024
#define HASH_LEN 32 /* SHA-256 digest length */

static char ota_write_data[BUFFSIZE + 1] = { 0 };

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

static char const *LOG_TAG = "mc|ota";

static void http_cleanup (esp_http_client_handle_t client) {
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

static void print_sha256 (const uint8_t *image_hash, const char *label) {
  char hash_print[HASH_LEN * 2 + 1];
  hash_print[HASH_LEN * 2] = 0;
  for (int i = 0; i < HASH_LEN; ++i) {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  ESP_LOGI(LOG_TAG, "%s: %s", label, hash_print);
}

static bool do_ota(char const *url) {
  esp_err_t err;
  esp_ota_handle_t update_handle = 0 ;
  esp_partition_t const *update_partition, *configured, *running, *last_invalid_app;
  esp_http_client_config_t config = {
    .url = url,
    .cert_pem = (char *)server_cert_pem_start,
    .timeout_ms = 10000,
    .keep_alive_enable = true,
  };
  int binary_file_length, data_read;
  bool image_header_was_checked;
  esp_app_desc_t new_app_info, running_app_info, invalid_app_info;
  esp_http_client_handle_t client;

  configured = esp_ota_get_boot_partition();
  running = esp_ota_get_running_partition();

  if (configured != running) {
    ESP_LOGW(LOG_TAG, "Configured OTA boot partition at offset 0x%08"PRIx32", but running "
	     "from offset 0x%08"PRIx32, configured->address, running->address);
    ESP_LOGW(LOG_TAG, "(This can happen if either the OTA boot data or preferred boot "
	     "image become corrupted somehow.)");
  }
  
  client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to initialize HTTP connection");
    return false;
  }
  
  err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(LOG_TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_fetch_headers(client);

  update_partition = esp_ota_get_next_update_partition(NULL);
  if (!update_partition) {
    ESP_LOGE(LOG_TAG, "Update partition is NULL, not doing OTA");
    http_cleanup(client);
    return false;
  }
  ESP_LOGI(LOG_TAG, "Writing to partition subtype %d at offset 0x%"PRIx32,
	   update_partition->subtype, update_partition->address);

  binary_file_length = 0;
  image_header_was_checked = false;

  while (1) {
    data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
    if (data_read < 0) {
      ESP_LOGE(LOG_TAG, "Error: SSL data read error");
      http_cleanup(client);
      return false;
    } else if (data_read > 0) {
      if (!image_header_was_checked) {
	if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
	    sizeof(esp_app_desc_t)) {
	  
	  memcpy(&new_app_info,
		 &ota_write_data[sizeof(esp_image_header_t) +
				 sizeof(esp_image_segment_header_t)],
		 sizeof(esp_app_desc_t));
	  ESP_LOGI(LOG_TAG, "Incoming version: %s", new_app_info.version);

	  if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
	    ESP_LOGI(LOG_TAG, "Running version: %s", running_app_info.version);
	  }

	  last_invalid_app = esp_ota_get_last_invalid_partition();
	  if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) ==
	      ESP_OK) {
	    ESP_LOGI(LOG_TAG, "Last invalid firmware version: %s", invalid_app_info.version);
	  }

	  if (last_invalid_app != NULL) {
	    if (memcmp(invalid_app_info.version, new_app_info.version,
		       sizeof(new_app_info.version)) == 0) {
	      ESP_LOGW(LOG_TAG, "New version is the same as invalid version.");
	      ESP_LOGW(LOG_TAG, "Previously, there was an attempt to launch the firmware "
		       "with %s version, but it failed.", invalid_app_info.version);
	      ESP_LOGW(LOG_TAG, "The firmware has been rolled back to the previous version.");
	      http_cleanup(client);
	      return false;
	    }
	  }
	  
	  image_header_was_checked = true;

	  err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
	  if (err != ESP_OK) {
	    ESP_LOGE(LOG_TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
	    http_cleanup(client);
	    esp_ota_abort(update_handle);
	    return false;
	  }
	  ESP_LOGI(LOG_TAG, "esp_ota_begin succeeded");
	} else {
	  ESP_LOGE(LOG_TAG, "data received from server is too small (%d bytes)", data_read);
	  http_cleanup(client);
	  esp_ota_abort(update_handle);
	  return false;
	}
      }

      err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);
      if (err != ESP_OK) {
	ESP_LOGE(LOG_TAG, "esp_ota_write() failed");
	http_cleanup(client);
	esp_ota_abort(update_handle);
	return false;
      }
      binary_file_length += data_read;
      ESP_LOGD(LOG_TAG, "Written image length %d", binary_file_length);
    } else if (data_read == 0) {
      /*
       * As esp_http_client_read never returns negative error code, we rely on
       * `errno` to check for underlying transport connectivity closure if any
       */
      if (errno == ECONNRESET || errno == ENOTCONN) {
	ESP_LOGE(LOG_TAG, "Connection closed, errno = %d", errno);
	break;
      }
      if (esp_http_client_is_complete_data_received(client) == true) {
	ESP_LOGI(LOG_TAG, "Connection closed");
	break;
      }
    }
  }
  
  ESP_LOGI(LOG_TAG, "Total Write binary data length: %d", binary_file_length);
  if (esp_http_client_is_complete_data_received(client) != true) {
    ESP_LOGE(LOG_TAG, "Error in receiving complete file");
    http_cleanup(client);
    esp_ota_abort(update_handle);
    return false;
  }

  err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      ESP_LOGE(LOG_TAG, "Image validation failed, image is corrupted");
    } else {
      ESP_LOGE(LOG_TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
    }
    http_cleanup(client);
    return false;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(LOG_TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
    http_cleanup(client);
    return false;
  }
  ESP_LOGI(LOG_TAG, "OTA firmware upgrade completed, restarting");
  stop_udp_logging();
  esp_restart();
  return true;
}

void ota_task (void *param) {
  char *firmware_upgrade_command, *url;
  QueueHandle_t ota_q = ((struct mc_task_args_t_ *) param)->ota_q;
  esp_partition_t partition;
  esp_partition_t const *running;
  uint8_t sha_256[HASH_LEN] = {0};
  esp_ota_img_states_t ota_state;

  /* get sha256 digest for the partition table */
  partition.address   = ESP_PARTITION_TABLE_OFFSET;
  partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
  partition.type      = ESP_PARTITION_TYPE_DATA;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for the partition table: ");

  /* get sha256 digest for bootloader */
  partition.address   = ESP_BOOTLOADER_OFFSET;
  partition.size      = ESP_PARTITION_TABLE_OFFSET;
  partition.type      = ESP_PARTITION_TYPE_APP;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for bootloader: ");

  /* get sha256 digest for running partition */
  esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
  print_sha256(sha_256, "SHA-256 for current firmware: ");

  running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }

  /* Loop forever, looking for enqueues to the ota_q */
  while (1) {
    if (pdTRUE == xQueueReceive(ota_q, (void *) &firmware_upgrade_command,
				portMAX_DELAY)) {
      ESP_LOGI(LOG_TAG, "upgrade request:  %s",firmware_upgrade_command);
      /* firmware-upgrade=https://192.168.29.76:59443/mc.bin */
      if (strstr(firmware_upgrade_command, "firmware-upgrade=") == firmware_upgrade_command) {
	url = firmware_upgrade_command + strlen("firmware-upgrade=");
	ESP_LOGI(LOG_TAG, "Invoking do_ota(\"%s\")", url);
	if (!do_ota(url)) {
	  ESP_LOGE(LOG_TAG, "do_ota() failed");
	  free(firmware_upgrade_command);
	}
      } else {
	ESP_LOGE(LOG_TAG, "\%s\" is not in the expected format", firmware_upgrade_command);
	free(firmware_upgrade_command);
      }
    }
  }
}
