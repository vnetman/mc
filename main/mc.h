#ifndef __MC_H__
#define __MC_H__

/* Bits in the `mc_event_group` event group */
#define EVENT_WIFI_CONNECTED BIT0
#define EVENT_WIFI_FAILED BIT1
#define EVENT_OH_TANK_FULL BIT2
#define EVENT_MOTOR_RUNNING BIT3

struct mc_task_args_t_ {
  EventGroupHandle_t mc_event_group;
  QueueHandle_t beep_q;
  QueueHandle_t motor_on_off_q;
  QueueHandle_t ota_q;
};

/* main.c */
extern char const *LOG_TAG;

/* wifi.c */
extern void start_wifi(EventGroupHandle_t);

/* oh_tank_level.c */
extern void oh_tank_level_task(void *param);

/* motor.c */
extern void motor_task(void *param);

/* beep.c */
extern void beep_task(void *param);

/* http.c */
extern void http_server_task(void *param);

/* gpio.c */
extern void init_gpio_pins(void);

/* udp_logging.c */
extern void udp_logging_task(void *param);

/* ota.c */
extern void ota_task(void *param);

#endif
