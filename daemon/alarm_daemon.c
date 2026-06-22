#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <errno.h>
#include <syslog.h>
#include <gpiod.h>
#include "../common/alarm_common.h"

/* libgpiod v2 handles */
static struct gpiod_chip *chip;
static struct gpiod_line_request *req_button;
static struct gpiod_line_request *req_led;
static struct gpiod_line_request *req_buzzer;

static alarm_shared_data_t *shared_data;
static sem_t *shm_semaphore;

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  event_cond  = PTHREAD_COND_INITIALIZER;
static int event_pending = 0;

static volatile int running = 1;

/* --- Fonctions GPIO via libgpiod v2 --- */

static int gpio_init_all(void)
{
    struct gpiod_request_config *rcfg = NULL;
    struct gpiod_line_config *lcfg = NULL;
    struct gpiod_line_settings *settings = NULL;

    chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        syslog(LOG_ERR, "Impossible d'ouvrir /dev/gpiochip0: %s", strerror(errno));
        return -1;
    }

    /* --- Bouton : entree avec pull-up --- */
    settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    lcfg = gpiod_line_config_new();
    unsigned int btn_offset = GPIO_BUTTON_PIN;
    gpiod_line_config_add_line_settings(lcfg, &btn_offset, 1, settings);

    rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "alarm_button");

    req_button = gpiod_chip_request_lines(chip, rcfg, lcfg);

    gpiod_line_settings_free(settings);
    gpiod_line_config_free(lcfg);
    gpiod_request_config_free(rcfg);

    if (!req_button) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en entree", GPIO_BUTTON_PIN);
        return -1;
    }

    /* --- LED : sortie --- */
    settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    lcfg = gpiod_line_config_new();
    unsigned int led_offset = GPIO_LED_PIN;
    gpiod_line_config_add_line_settings(lcfg, &led_offset, 1, settings);

    rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "alarm_led");

    req_led = gpiod_chip_request_lines(chip, rcfg, lcfg);

    gpiod_line_settings_free(settings);
    gpiod_line_config_free(lcfg);
    gpiod_request_config_free(rcfg);

    if (!req_led) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en sortie", GPIO_LED_PIN);
        return -1;
    }

    /* --- Buzzer : sortie --- */
    settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

    lcfg = gpiod_line_config_new();
    unsigned int buz_offset = GPIO_BUZZER_PIN;
    gpiod_line_config_add_line_settings(lcfg, &buz_offset, 1, settings);

    rcfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(rcfg, "alarm_buzzer");

    req_buzzer = gpiod_chip_request_lines(chip, rcfg, lcfg);

    gpiod_line_settings_free(settings);
    gpiod_line_config_free(lcfg);
    gpiod_request_config_free(rcfg);

    if (!req_buzzer) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en sortie", GPIO_BUZZER_PIN);
        return -1;
    }

    syslog(LOG_INFO, "GPIO initialises avec libgpiod v2 (/dev/gpiochip0)");
    return 0;
}

static void gpio_write_led(int value)
{
    unsigned int offset = GPIO_LED_PIN;
    enum gpiod_line_value val = value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(req_led, offset, val);
}

static void gpio_write_buzzer(int value)
{
    unsigned int offset = GPIO_BUZZER_PIN;
    enum gpiod_line_value val = value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(req_buzzer, offset, val);
}

static int gpio_read_button(void)
{
    unsigned int offset = GPIO_BUTTON_PIN;
    return gpiod_line_request_get_value(req_button, offset);
}

/* --- Thread 1 : Surveillance du bouton GPIO --- */

static void *thread_gpio_monitor(void *arg)
{
    (void)arg;
    syslog(LOG_INFO, "Thread GPIO monitor demarre (pin %d)", GPIO_BUTTON_PIN);

    int last_value = gpio_read_button();

    while (running) {
        usleep(50000); /* poll toutes les 50ms */

        int value = gpio_read_button();
        if (value < 0) {
            syslog(LOG_ERR, "Erreur lecture GPIO bouton");
            continue;
        }

        /* Detection front descendant (bouton appuye = INACTIVE avec pull-up) */
        if (last_value == GPIOD_LINE_VALUE_ACTIVE &&
            value == GPIOD_LINE_VALUE_INACTIVE) {

            syslog(LOG_INFO, "Bouton appuye detecte !");

            sem_wait(shm_semaphore);
            if (shared_data->state == ALARM_STATE_ARMED) {
                shared_data->state = ALARM_STATE_TRIGGERED;
                shared_data->trigger_count++;
                shared_data->last_trigger_time = time(NULL);
            }
            sem_post(shm_semaphore);

            pthread_mutex_lock(&event_mutex);
            event_pending = 1;
            pthread_cond_signal(&event_cond);
            pthread_mutex_unlock(&event_mutex);
        }

        last_value = value;
    }

    syslog(LOG_INFO, "Thread GPIO monitor arrete");
    return NULL;
}

/* --- Thread 2 : Actionneur (LED + Buzzer) --- */

static void *thread_actuator(void *arg)
{
    (void)arg;
    syslog(LOG_INFO, "Thread actionneur demarre");

    while (running) {
        pthread_mutex_lock(&event_mutex);

        while (!event_pending && running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&event_cond, &event_mutex, &ts);
        }

        if (!running) {
            pthread_mutex_unlock(&event_mutex);
            break;
        }

        event_pending = 0;
        pthread_mutex_unlock(&event_mutex);

        syslog(LOG_INFO, "Activation de l'alarme (LED + buzzer)");

        for (int i = 0; i < 10 && running; i++) {
            int state = i % 2;
            gpio_write_led(state);
            gpio_write_buzzer(state);

            sem_wait(shm_semaphore);
            shared_data->led_status = state;
            shared_data->buzzer_status = state;
            sem_post(shm_semaphore);

            usleep(500000);
        }

        gpio_write_led(0);
        gpio_write_buzzer(0);

        sem_wait(shm_semaphore);
        shared_data->led_status = 0;
        shared_data->buzzer_status = 0;
        if (shared_data->state == ALARM_STATE_TRIGGERED)
            shared_data->state = ALARM_STATE_ARMED;
        sem_post(shm_semaphore);
    }

    syslog(LOG_INFO, "Thread actionneur arrete");
    return NULL;
}

/* --- Thread 3 : Lecture du FIFO de controle --- */

static void *thread_control_listener(void *arg)
{
    (void)arg;

    if (mkfifo(CTL_FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "mkfifo echoue: %s", strerror(errno));
        return NULL;
    }

    syslog(LOG_INFO, "Thread controle demarre (FIFO: %s)", CTL_FIFO_PATH);

    while (running) {
        int fd = open(CTL_FIFO_PATH, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            sleep(1);
            continue;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 1000);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            alarm_cmd_t cmd;
            ssize_t n = read(fd, &cmd, sizeof(cmd));

            if (n == sizeof(cmd)) {
                sem_wait(shm_semaphore);

                switch (cmd) {
                case CMD_ARM:
                    shared_data->state = ALARM_STATE_ARMED;
                    syslog(LOG_INFO, "Commande recue: ARM");
                    break;
                case CMD_DISARM:
                    shared_data->state = ALARM_STATE_DISARMED;
                    gpio_write_led(0);
                    gpio_write_buzzer(0);
                    syslog(LOG_INFO, "Commande recue: DISARM");
                    break;
                case CMD_RESET:
                    shared_data->trigger_count = 0;
                    syslog(LOG_INFO, "Commande recue: RESET");
                    break;
                case CMD_STATUS:
                    syslog(LOG_INFO, "Commande recue: STATUS (etat=%d)",
                           shared_data->state);
                    break;
                }

                sem_post(shm_semaphore);
            }
        }

        close(fd);
    }

    unlink(CTL_FIFO_PATH);
    syslog(LOG_INFO, "Thread controle arrete");
    return NULL;
}

/* --- Signal handler --- */

static void signal_handler(int sig)
{
    syslog(LOG_INFO, "Signal %d recu, arret en cours...", sig);
    running = 0;

    pthread_mutex_lock(&event_mutex);
    pthread_cond_signal(&event_cond);
    pthread_mutex_unlock(&event_mutex);
}

/* --- Initialisation SHM --- */

static int shm_init(void)
{
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        syslog(LOG_ERR, "shm_open echoue: %s", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, sizeof(alarm_shared_data_t)) < 0) {
        syslog(LOG_ERR, "ftruncate echoue: %s", strerror(errno));
        close(fd);
        return -1;
    }

    shared_data = mmap(NULL, sizeof(alarm_shared_data_t),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shared_data == MAP_FAILED) {
        syslog(LOG_ERR, "mmap echoue: %s", strerror(errno));
        return -1;
    }

    memset(shared_data, 0, sizeof(alarm_shared_data_t));
    shared_data->state = ALARM_STATE_DISARMED;

    shm_semaphore = sem_open(SHM_SEM_NAME, O_CREAT, 0666, 1);
    if (shm_semaphore == SEM_FAILED) {
        syslog(LOG_ERR, "sem_open echoue: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* --- Nettoyage GPIO --- */

static void gpio_cleanup(void)
{
    if (req_button) gpiod_line_request_release(req_button);
    if (req_led)    gpiod_line_request_release(req_led);
    if (req_buzzer) gpiod_line_request_release(req_buzzer);
    if (chip)       gpiod_chip_close(chip);
}

/* --- Main --- */

int main(void)
{
    pthread_t tid_gpio, tid_actuator, tid_control;

    openlog("alarm_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "Demarrage du demon d'alarme");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (gpio_init_all() < 0) {
        syslog(LOG_ERR, "Echec initialisation GPIO");
        return EXIT_FAILURE;
    }

    if (shm_init() < 0) {
        syslog(LOG_ERR, "Echec initialisation SHM");
        return EXIT_FAILURE;
    }

    pthread_create(&tid_gpio,     NULL, thread_gpio_monitor,     NULL);
    pthread_create(&tid_actuator, NULL, thread_actuator,         NULL);
    pthread_create(&tid_control,  NULL, thread_control_listener, NULL);

    syslog(LOG_INFO, "Demon pret - etat initial: DISARMED");

    pthread_join(tid_gpio,     NULL);
    pthread_join(tid_actuator, NULL);
    pthread_join(tid_control,  NULL);

    gpio_cleanup();
    munmap(shared_data, sizeof(alarm_shared_data_t));
    sem_close(shm_semaphore);

    syslog(LOG_INFO, "Demon arrete proprement");
    closelog();
    return EXIT_SUCCESS;
}