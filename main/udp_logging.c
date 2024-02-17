#include <string.h>
#include <errno.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mc.h"

static int fd_socket = -1;
static struct sockaddr_in logging_host_addr;
static vprintf_like_t uart_logging_fn;
static char logging_buf[256 + 1]; /* All our logs will have to fit in 256 characters */

static int udp_logging_fn (char const *fmt, va_list args) {
  int len, err;
  socklen_t optlen;
  /* Do not invoke ESP_LOG* macros in this function! */
  if (fd_socket != -1) {
    len = vsnprintf(logging_buf, sizeof(logging_buf) - 1, fmt, args);
    logging_buf[len] = '\0';
    if (0 > sendto(fd_socket, logging_buf, len, 0, (struct sockaddr *) &logging_host_addr,
		   sizeof(logging_host_addr))) {
      printf("sendto() failed: %s, %ld, %d bytes\n", strerror(errno),
	     (long) &logging_host_addr, len);
      err = 0;
      optlen = sizeof(err);
      getsockopt(fd_socket, SOL_SOCKET, SO_ERROR, &err, &optlen);
      printf("sendto() failed because of %d (%s)\n", err, strerror(err));
    }
  }
  /* Send to stdout */
  return vprintf(fmt, args);
}

static void stop_udp_logging (void) {
  esp_log_set_vprintf(uart_logging_fn);
  close(fd_socket);
  fd_socket = -1;
  ESP_LOGI("mc", "Stopped UDP logging");
}

static void start_udp_logging (void) {
  fd_socket = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_socket < 0) {
    ESP_LOGE("mc", "Failed to create socket for UDP logging: %s", strerror(errno));
    fd_socket = -1;
    return;
  }

  /* Prepare the logging_host_addr which will be used in the sendto() call */
  memset(&logging_host_addr, 0, sizeof(logging_host_addr));

  logging_host_addr.sin_family = AF_INET;
  inet_pton(AF_INET, CONFIG_WLM_UDP_LOGGING_IPV4_ADDRESS, &(logging_host_addr.sin_addr));
  logging_host_addr.sin_port = htons(CONFIG_WLM_UDP_LOGGING_PORT);

  uart_logging_fn = esp_log_set_vprintf(udp_logging_fn);
  ESP_LOGI("mc", "Started UDP logging");
}

void udp_logging_task (void *param) {
  EventBits_t bits;
  struct mc_task_args_t_ *mc_task_args = (struct mc_task_args_t_ *) param;
  
  while (pdTRUE) {
    bits = xEventGroupWaitBits(mc_task_args->mc_event_group,
			       EVENT_WIFI_CONNECTED | EVENT_WIFI_FAILED,
			       pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & EVENT_WIFI_CONNECTED) {
      if (fd_socket == -1) {
	ESP_LOGI("mc", "Wifi up, starting UDP logging");
	start_udp_logging();
      } else {
	ESP_LOGE("mc", "Wifi up, but looks like UDP logging is already enabled");
      }
    } else if (bits & EVENT_WIFI_FAILED) {
      if (fd_socket == -1) {
	ESP_LOGE("mc", "Wifi down, but looks like UDP logging is already disabled");
      } else {
	ESP_LOGI("mc", "Wifi down, stopping UDP logging");
	stop_udp_logging();
      }
    }
  }
}
