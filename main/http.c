#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "mc.h"

static char *firmware_upgrade_command = NULL;
extern bool oh_tank_level_last;
static char const *LOG_TAG = "mc|httpd";

static esp_err_t mc_version_info_handler (httpd_req_t *req) {
  esp_partition_t const *next_partition, *running_partition, *boot_partition,
    *last_invalid_partition;
  esp_app_desc_t next_app_info = {0}, running_app_info = {0}, invalid_app_info = {0};
  char *response, *cursor, *sentinel;
  int len, remaining_length;
  static char const *buf_too_small = "Buffer too small for response";

  /*
  Could not fetch running partition info\n
  Could not fetch other partition info\n
  Boot partition is not identical to running partition\n
  Invalid partition seen\n
  Could not fetch invalid partition info\n
  */
  response = malloc(300 + 1);
  if (!response) {
    ESP_LOGE(LOG_TAG, "Failed to allocate buffer for version info response");
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  response[300] = '\0';
  sentinel = &(response[299]);
  cursor = response;
  remaining_length = 300;
  
  running_partition = esp_ota_get_running_partition();
  next_partition = esp_ota_get_next_update_partition(running_partition);
  
  if (esp_ota_get_partition_description(running_partition, &running_app_info) == ESP_OK) {
    len = snprintf(cursor, remaining_length, "Running version: %s\n",
		   running_app_info.version);
  } else {
    len = snprintf(cursor, remaining_length, "Could not fetch running partition info\n");
  }
  remaining_length -= len;
  cursor += len;
  if (cursor >= sentinel) {
    ESP_LOGE(LOG_TAG, "%s\n", buf_too_small);
    free(response);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  
  if (esp_ota_get_partition_description(next_partition, &next_app_info) == ESP_OK) {
    len = snprintf(cursor, remaining_length, "Version in other partition: %s\n",
		   next_app_info.version);
  } else {
    len = snprintf(cursor, remaining_length, "Could not fetch other partition info\n");
  }
  remaining_length -= len;
  cursor += len;
  if (cursor >= sentinel) {
    ESP_LOGE(LOG_TAG, "%s\n", buf_too_small);
    free(response);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  boot_partition = esp_ota_get_boot_partition();
  len = snprintf(cursor, remaining_length, "Boot partition %s identical "
		 "to running partition\n",
		 (boot_partition != running_partition) ? "is not" : "is");
  remaining_length -= len;
  cursor += len;
  if (cursor >= sentinel) {
    ESP_LOGE(LOG_TAG, "%s\n", buf_too_small);
    free(response);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  last_invalid_partition = esp_ota_get_last_invalid_partition();
  if (last_invalid_partition) {
    len = snprintf(cursor, remaining_length, "Invalid partition seen\n");
    remaining_length -= len;
    cursor += len;
    
    if (esp_ota_get_partition_description(last_invalid_partition, &invalid_app_info) == ESP_OK) {
      len = snprintf(cursor, remaining_length, "Version in invalid partition: %s\n",
		     invalid_app_info.version);
    } else {
      len = snprintf(cursor, remaining_length, "Could not fetch invalid partition info\n");
    }
    remaining_length -= len;
    cursor += len;
  } else {
    len = snprintf(cursor, remaining_length, "Invalid partition not seen\n");
    remaining_length -= len;
    cursor += len;
  }
  if (cursor >= sentinel) {
    ESP_LOGE(LOG_TAG, "%s\n", buf_too_small);
    free(response);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  *cursor = '\0';
  
  if (ESP_OK != httpd_resp_send(req, response, strlen(response))) {
    free(response);
    ESP_LOGE(LOG_TAG, "Unable to send response to status req");
    return ESP_FAIL;
  } else {
    ESP_LOGI(LOG_TAG, "Response sent: \"%s\"", response);
    free(response);
    return ESP_OK;
  }
}

static httpd_uri_t mc_version_info_uri = {
    .uri       = "/mc_version_info",
    .method    = HTTP_GET,
    .handler   = mc_version_info_handler,
    .user_ctx  = NULL /* will be filled in in start_webserver */
};

static esp_err_t mc_status_handler (httpd_req_t *req) {
  struct mc_task_args_t_ *task_args;
  EventBits_t bits;
  static char const response_fmt[] = "Motor is %s, tank is %s\n";
  char response[sizeof(response_fmt)
		+ 11 /* "not running" */
		+ 8 /* "not full" */
		+ 8 /* margin */ ];

  task_args = (struct mc_task_args_t_ *) req->user_ctx;
  if (!task_args) {
    ESP_LOGE(LOG_TAG, "NULL context in %s", __FUNCTION__);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  bits = xEventGroupGetBits(task_args->mc_event_group);
  if (bits & EVENT_MOTOR_RUNNING) {
    snprintf(response, sizeof(response), response_fmt, "running",
	     oh_tank_level_last ? "full" : "not full");
  } else {
    snprintf(response, sizeof(response), response_fmt, "not running",
	     oh_tank_level_last ? "full" : "not full");
  }
  response[sizeof(response) - 1] = '\0';

  if (ESP_OK != httpd_resp_send(req, response, strlen(response))) {
    ESP_LOGE(LOG_TAG, "Unable to send response to status req");
  } else {
    ESP_LOGI(LOG_TAG, "Response sent: \"%s\"", response);
  }
  return ESP_OK;
}

static httpd_uri_t mc_status_uri = {
    .uri       = "/mc_status",
    .method    = HTTP_GET,
    .handler   = mc_status_handler,
    .user_ctx  = NULL /* will be filled in in start_webserver */
};

/* HTTP POST handler */
static esp_err_t mc_ctrl_handler (httpd_req_t *req) {
  char *buf;
  int ret;
  bool desired_motor_state;
  struct mc_task_args_t_ *task_args;

  /*
    motor=on
    motor=off
    firmware-upgrade=https://192.168.29.76:59443/mc.bin
  */
  ESP_LOGI(LOG_TAG, "Handling POST (%u bytes)", req->content_len);
  if (req->content_len > 128) {
    ESP_LOGE(LOG_TAG, "POST length suspicious");
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  task_args = (struct mc_task_args_t_ *) req->user_ctx;
  if (!task_args) {
    ESP_LOGE(LOG_TAG, "NULL context in %s", __FUNCTION__);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  buf = malloc(req->content_len + 1);
  if (!buf) {
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  
  if ((ret = httpd_req_recv(req, buf, req->content_len)) <= 0) {
    ESP_LOGE(LOG_TAG, "unable to receive POST request");
    free(buf);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  buf[req->content_len] = '\0';
  if (strstr(buf, "firmware-upgrade=") == buf) {
    /* Post the `buf` to the OTA queue */
    firmware_upgrade_command = strdup(buf);
    if (pdTRUE != xQueueSend(task_args->ota_q, (void *) &firmware_upgrade_command,
			     pdMS_TO_TICKS(2000))) {
      ESP_LOGE(LOG_TAG, "failed to enq firmware upgrade req");
    }
  } else if (strstr(buf, "motor=") == buf) {
    if (strcmp(buf, "motor=on") == 0) {
      ESP_LOGI(LOG_TAG, "Will set motor to ON state");
      desired_motor_state = true;
      if (pdTRUE != xQueueSend(task_args->motor_on_off_q, (void *) &desired_motor_state,
			       pdMS_TO_TICKS(1000))) {
	ESP_LOGE(LOG_TAG, "Failed to enqueue motor ON");
      }
    } else if (strcmp(buf, "motor=off") == 0) {
      ESP_LOGI(LOG_TAG, "Will set motor to OFF state");
      desired_motor_state = false;
      if (pdTRUE != xQueueSend(task_args->motor_on_off_q, (void *) &desired_motor_state,
			       pdMS_TO_TICKS(1000))) {
	ESP_LOGE(LOG_TAG, "failed to enqueue motor OFF");
      }
    } else {
      ESP_LOGE(LOG_TAG, "cannot understand motor desired state \"%s\"", buf);
      free(buf);
      httpd_resp_send_408(req);
      return ESP_FAIL;
    }
  } else {
    ESP_LOGE(LOG_TAG, "POST handler: unable to parse control word");
    free(buf);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  /* Respond with empty body */
  httpd_resp_send(req, NULL, 0);
  free(buf);
  return ESP_OK;
}

static httpd_uri_t mc_ctrl_uri = {
    .uri       = "/mc_ctrl",
    .method    = HTTP_POST,
    .handler   = mc_ctrl_handler,
    .user_ctx  = NULL /* will be filled in in start_webserver */
};

static httpd_handle_t start_webserver (struct mc_task_args_t_ *task_args) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  /* Start the httpd server */
  ESP_LOGI(LOG_TAG, "starting server on port: '%d'", config.server_port);
  
  if (httpd_start(&server, &config) == ESP_OK) {
    /* Set URI handlers */
    ESP_LOGI(LOG_TAG, "server started, registering URI handlers");
    mc_status_uri.user_ctx = task_args;
    mc_ctrl_uri.user_ctx = task_args;
    httpd_register_uri_handler(server, &mc_status_uri);
    httpd_register_uri_handler(server, &mc_ctrl_uri);
    httpd_register_uri_handler(server, &mc_version_info_uri);
    return server;
  }

  ESP_LOGI(LOG_TAG, "error starting server!");
  return NULL;
}

static void stop_webserver (httpd_handle_t server) {
  httpd_stop(server);
}

void http_server_task (void *param) {
  httpd_handle_t server = NULL;
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  
  while (pdTRUE) {
    bits = xEventGroupWaitBits(mc_task_args->mc_event_group,
			       EVENT_WIFI_CONNECTED | EVENT_WIFI_FAILED,
			       pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & EVENT_WIFI_CONNECTED) {
      if (server) {
	ESP_LOGE(LOG_TAG, "server is non-NULL before start_webserver()");
      }
      server = start_webserver(mc_task_args);
    } else if (bits & EVENT_WIFI_FAILED) {
      if (!server) {
	ESP_LOGE(LOG_TAG, "server is NULL before stop_webserver()");
      }
      stop_webserver(server);
      server = NULL;
    }
  }
}
