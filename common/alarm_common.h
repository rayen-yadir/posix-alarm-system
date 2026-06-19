#ifndef ALARM_COMMON_H
#define ALARM_COMMON_H

#include <semaphore.h>
#include <stdint.h>
#include <time.h>

/* Chemins des ressources IPC */
#define SHM_NAME        "/alarm_state"
#define SHM_SEM_NAME    "/alarm_sem"
#define CTL_FIFO_PATH   "/tmp/alarm_ctl_fifo"

/* Chemins GPIO (sysfs) */
#define GPIO_BUTTON_PIN   17
#define GPIO_LED_PIN      27
#define GPIO_BUZZER_PIN   22

#define GPIO_PATH_EXPORT     "/sys/class/gpio/export"
#define GPIO_PATH_UNEXPORT   "/sys/class/gpio/unexport"
#define GPIO_PATH_BASE       "/sys/class/gpio/gpio"

/* États possibles du système d'alarme */
typedef enum {
    ALARM_STATE_DISARMED = 0,
    ALARM_STATE_ARMED,
    ALARM_STATE_TRIGGERED
} alarm_state_t;

/* Structure stockée en mémoire partagée */
typedef struct {
    alarm_state_t state;
    uint32_t trigger_count;
    time_t last_trigger_time;
    uint8_t led_status;
    uint8_t buzzer_status;
} alarm_shared_data_t;

/* Commandes envoyées via le FIFO de contrôle */
typedef enum {
    CMD_ARM = 1,
    CMD_DISARM,
    CMD_STATUS,
    CMD_RESET
} alarm_cmd_t;

#endif /* ALARM_COMMON_H */