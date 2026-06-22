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

/* libgpiod handles */
static struct gpiod_chip *chip;
static struct gpiod_line *line_button;
static struct gpiod_line *line_led;
static struct gpiod_line *line_buzzer;

static alarm_shared_data_t *shared_data;
static sem_t *shm_semaphore;

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  event_cond  = PTHREAD_COND_INITIALIZER;
static int event_pending = 0;

static volatile int running = 1;

/* --- Fonctions GPIO via libgpiod --- */

static int gpio_init_all(void)
{
    chip = gpiod_chip_open_by_name("gpiochip0");
    if (!chip) {
        syslog(LOG_ERR, "Impossible d'ouvrir gpiochip0: %s", strerror(errno));
        return -1;
    }

    /* Bouton : entree */
    line_button = gpiod_chip_get_line(chip, GPIO_BUTTON_PIN);
    if (!line_button) {
        syslog(LOG_ERR, "Impossible d'obtenir GPIO%d", GPIO_BUTTON_PIN);
        return -1;
    }
    if (gpiod_line_request_input_flags(line_button, "alarm_daemon",
                                        GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en entree", GPIO_BUTTON_PIN);
        return -1;
    }

    /* LED : sortie */
    line_led = gpiod_chip_get_line(chip, GPIO_LED_PIN);
    if (!line_led) {
        syslog(LOG_ERR, "Impossible d'obtenir GPIO%d", GPIO_LED_PIN);
        return -1;
    }
    if (gpiod_line_request_output(line_led, "alarm_daemon", 0) < 0) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en sortie", GPIO_LED_PIN);
        return -1;
    }

    /* Buzzer : sortie */
    line_buzzer = gpiod_chip_get_line(chip, GPIO_BUZZER_PIN);
    if (!line_buzzer) {
        syslog(LOG_ERR, "Impossible d'obtenir GPIO%d", GPIO_BUZZER_PIN);
        return -1;
    }
    if (gpiod_line_request_output(line_buzzer, "alarm_daemon", 0) < 0) {
        syslog(LOG_ERR, "Impossible de configurer GPIO%d en sortie", GPIO_BUZZER_PIN);
        return -1;
    }

    syslog(LOG_INFO, "GPIO initialises avec libgpiod (gpiochip0)");
    return 0;
}

static void gpio_write_led(int value)
{
    gpiod_line_set_value(line_led, value);
}

static void gpio_write_buzzer(int value)
{
    gpiod_line_set_value(line_buzzer, value);
}

/* --- Thread 1 : Surveillance du bouton GPIO --- */

static void *thread_gpio_monitor(void *arg)
{
    syslog(LOG_INFO, "Thread GPIO monitor demarre (pin %d)", GPIO_BUTTON_PIN);

    int last_value = gpiod_line_get_value(line_button);

    while (running) {
        usleep(50000); /* poll toutes les 50ms */

        int value = gpiod_line_get_value(line_button);
        if (value < 0) {
            syslog(LOG_ERR, "Erreur lecture GPIO bouton");
            continue;
        }

        /* Detection front descendant (bouton appuye = 0 avec pull-up) */
        if (last_value == 1 && value == 0) {
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
    if (line_button) gpiod_line_release(line_button);
    if (line_led)    gpiod_line_release(line_led);
    if (line_buzzer) gpiod_line_release(line_buzzer);
    if (chip)        gpiod_chip_close(chip);
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

    pthread_create(&tid_gpio,    NULL, thread_gpio_monitor,    NULL);
    pthread_create(&tid_actuator, NULL, thread_actuator,       NULL);
    pthread_create(&tid_control,  NULL, thread_control_listener, NULL);

    syslog(LOG_INFO, "Demon pret - etat initial: DISARMED");

    pthread_join(tid_gpio,    NULL);
    pthread_join(tid_actuator, NULL);
    pthread_join(tid_control,  NULL);

    gpio_cleanup();
    munmap(shared_data, sizeof(alarm_shared_data_t));
    sem_close(shm_semaphore);

    syslog(LOG_INFO, "Demon arrete proprement");
    closelog();
    return EXIT_SUCCESS;
}