#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mc.h"

static char *firmware_upgrade_command = NULL;
extern uint32_t oh_tank_level_voltage_last;

static esp_err_t mc_status_handler (httpd_req_t *req) {
  struct mc_task_args_t_ *task_args;
  EventBits_t bits;
  char const *response_fmt = "Motor is %s, tank level voltage is %lumV\n";
  /* not running 1200 */
  char response[100];

  task_args = (struct mc_task_args_t_ *) req->user_ctx;
  if (!task_args) {
    ESP_LOGE(LOG_TAG, "http_server: task args not found in user context in handler");
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  bits = xEventGroupGetBits(task_args->mc_event_group);
  if (bits & EVENT_MOTOR_RUNNING) {
    snprintf(response, sizeof(response), response_fmt, "running",
	     oh_tank_level_voltage_last);
  } else {
    snprintf(response, sizeof(response), response_fmt, "not running",
	     oh_tank_level_voltage_last);
  }
  response[sizeof(response) - 1] = '\0';

  if (ESP_OK != httpd_resp_send(req, response, strlen(response))) {
    ESP_LOGE(LOG_TAG, "http_server: unable to send response");
  } else {
    ESP_LOGI(LOG_TAG, "http_server: response sent");
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
  ESP_LOGI(LOG_TAG, "http_server: POST handler received %u bytes of request", req->content_len);
  if (req->content_len > 128) {
    ESP_LOGE(LOG_TAG, "http_server: handler received too many (%u) bytes", req->content_len);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  task_args = (struct mc_task_args_t_ *) req->user_ctx;
  if (!task_args) {
    ESP_LOGE(LOG_TAG, "http_server: task args not found in user context in handler");
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  buf = malloc(req->content_len + 1);
  if (!buf) {
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  
  if ((ret = httpd_req_recv(req, buf, req->content_len)) <= 0) {
    ESP_LOGE(LOG_TAG, "http_server: mc_ctrl_handler(): unable to receive");
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
      ESP_LOGE(LOG_TAG, "http_server: failed to enqueue firmware upgrade request");
    } else {
      ESP_LOGI(LOG_TAG, "http_server: enqueued firmware upgrade request");
    }
  } else if (strstr(buf, "motor=") == buf) {
    if (strcmp(buf, "motor=on") == 0) {
      ESP_LOGI(LOG_TAG, "http_server: Will set motor to ON state");
      desired_motor_state = true;
      if (pdTRUE != xQueueSend(task_args->motor_on_off_q, (void *) &desired_motor_state,
			       pdMS_TO_TICKS(1000))) {
	ESP_LOGE(LOG_TAG, "http_server: failed to enqueue motor ON");
      }
    } else if (strcmp(buf, "motor=off") == 0) {
      ESP_LOGI(LOG_TAG, "http_server: Will set motor to OFF state");
      desired_motor_state = false;
      if (pdTRUE != xQueueSend(task_args->motor_on_off_q, (void *) &desired_motor_state,
			       pdMS_TO_TICKS(1000))) {
	ESP_LOGE(LOG_TAG, "http_server: failed to enqueue motor OFF");
      }
    } else {
      ESP_LOGE(LOG_TAG, "http_server: cannot understand motor desired state \"%s\"", buf);
      free(buf);
      httpd_resp_send_408(req);
      return ESP_FAIL;
    }
  } else {
    ESP_LOGE(LOG_TAG, "http_server: mc_ctrl_handler(): unable to parse control word");
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
  ESP_LOGI(LOG_TAG, "http_server: starting server on port: '%d'", config.server_port);
  
  if (httpd_start(&server, &config) == ESP_OK) {
    /* Set URI handlers */
    ESP_LOGI(LOG_TAG, "http_server: registering URI handlers");
    mc_status_uri.user_ctx = task_args;
    mc_ctrl_uri.user_ctx = task_args;
    httpd_register_uri_handler(server, &mc_status_uri);
    httpd_register_uri_handler(server, &mc_ctrl_uri);
    return server;
  }

  ESP_LOGI(LOG_TAG, "http_server: error starting server!");
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
	ESP_LOGE(LOG_TAG, "http_server: server is non-NULL before start_webserver()");
      }
      server = start_webserver(mc_task_args);
    } else if (bits & EVENT_WIFI_FAILED) {
      if (!server) {
	ESP_LOGE(LOG_TAG, "http_server: server is NULL before stop_webserver()");
      }
      stop_webserver(server);
      server = NULL;
    }
  }
}
