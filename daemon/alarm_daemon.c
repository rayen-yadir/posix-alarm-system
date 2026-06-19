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
#include "../common/alarm_common.h"

static alarm_shared_data_t *shared_data;
static sem_t *shm_semaphore;

static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  event_cond  = PTHREAD_COND_INITIALIZER;
static int event_pending = 0;

static volatile int running = 1;

/* --- Fonctions GPIO via sysfs --- */

static int gpio_export(int pin)
{
    int fd = open(GPIO_PATH_EXPORT, O_WRONLY);
    if (fd < 0) return -1;
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "%d", pin);
    write(fd, buf, len);
    close(fd);
    usleep(100000);
    return 0;
}

static int gpio_set_direction(int pin, const char *direction)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%d/direction", GPIO_PATH_BASE, pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, direction, strlen(direction));
    close(fd);
    return 0;
}

static int gpio_set_edge(int pin, const char *edge)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%d/edge", GPIO_PATH_BASE, pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    write(fd, edge, strlen(edge));
    close(fd);
    return 0;
}

static int gpio_write_value(int pin, int value)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%d/value", GPIO_PATH_BASE, pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char buf[2];
    buf[0] = value ? '1' : '0';
    buf[1] = '\0';
    write(fd, buf, 1);
    close(fd);
    return 0;
}

/* --- Thread 1 : Surveillance du bouton GPIO --- */

static void *thread_gpio_monitor(void *arg)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%d/value", GPIO_PATH_BASE, GPIO_BUTTON_PIN);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        syslog(LOG_ERR, "Thread GPIO: impossible d'ouvrir %s", path);
        return NULL;
    }

    char value;
    lseek(fd, 0, SEEK_SET);
    read(fd, &value, 1);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLPRI | POLLERR;

    syslog(LOG_INFO, "Thread GPIO monitor démarré (pin %d)", GPIO_BUTTON_PIN);

    while (running) {
        int ret = poll(&pfd, 1, 1000);

        if (ret < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "Erreur poll() sur GPIO: %s", strerror(errno));
            break;
        }

        if (ret == 0) continue;

        if (pfd.revents & POLLPRI) {
            lseek(fd, 0, SEEK_SET);
            read(fd, &value, 1);

            int button_pressed = (value == '1');

            if (button_pressed) {
                syslog(LOG_INFO, "Bouton appuyé détecté !");

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
        }
    }

    close(fd);
    syslog(LOG_INFO, "Thread GPIO monitor arrêté");
    return NULL;
}

/* --- Thread 2 : Actionneur (LED + Buzzer) --- */

static void *thread_actuator(void *arg)
{
    syslog(LOG_INFO, "Thread actionneur démarré");

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
            int led_state = i % 2;
            gpio_write_value(GPIO_LED_PIN, led_state);
            gpio_write_value(GPIO_BUZZER_PIN, led_state);

            sem_wait(shm_semaphore);
            shared_data->led_status = led_state;
            shared_data->buzzer_status = led_state;
            sem_post(shm_semaphore);

            usleep(500000);
        }

        gpio_write_value(GPIO_LED_PIN, 0);
        gpio_write_value(GPIO_BUZZER_PIN, 0);

        sem_wait(shm_semaphore);
        shared_data->led_status = 0;
        shared_data->buzzer_status = 0;
        if (shared_data->state == ALARM_STATE_TRIGGERED)
            shared_data->state = ALARM_STATE_ARMED;
        sem_post(shm_semaphore);
    }

    syslog(LOG_INFO, "Thread actionneur arrêté");
    return NULL;
}

/* --- Thread 3 : Lecture du FIFO de contrôle --- */

static void *thread_control_listener(void *arg)
{
    if (mkfifo(CTL_FIFO_PATH, 0666) < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "mkfifo échoué: %s", strerror(errno));
        return NULL;
    }

    syslog(LOG_INFO, "Thread contrôle démarré (FIFO: %s)", CTL_FIFO_PATH);

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
                    syslog(LOG_INFO, "Commande reçue: ARM");
                    break;
                case CMD_DISARM:
                    shared_data->state = ALARM_STATE_DISARMED;
                    gpio_write_value(GPIO_LED_PIN, 0);
                    gpio_write_value(GPIO_BUZZER_PIN, 0);
                    syslog(LOG_INFO, "Commande reçue: DISARM");
                    break;
                case CMD_RESET:
                    shared_data->trigger_count = 0;
                    syslog(LOG_INFO, "Commande reçue: RESET");
                    break;
                case CMD_STATUS:
                    syslog(LOG_INFO, "Commande reçue: STATUS (état=%d)",
                           shared_data->state);
                    break;
                }

                sem_post(shm_semaphore);
            }
        }

        close(fd);
    }

    unlink(CTL_FIFO_PATH);
    syslog(LOG_INFO, "Thread contrôle arrêté");
    return NULL;
}

/* --- Signal handler --- */

static void signal_handler(int sig)
{
    syslog(LOG_INFO, "Signal %d reçu, arrêt en cours...", sig);
    running = 0;
    pthread_mutex_lock(&event_mutex);
    pthread_cond_signal(&event_cond);
    pthread_mutex_unlock(&event_mutex);
}

/* --- Initialisation GPIO --- */

static int gpio_init_all(void)
{
    gpio_export(GPIO_BUTTON_PIN);
    gpio_set_direction(GPIO_BUTTON_PIN, "in");
    gpio_set_edge(GPIO_BUTTON_PIN, "both");

    gpio_export(GPIO_LED_PIN);
    gpio_set_direction(GPIO_LED_PIN, "out");
    gpio_write_value(GPIO_LED_PIN, 0);

    gpio_export(GPIO_BUZZER_PIN);
    gpio_set_direction(GPIO_BUZZER_PIN, "out");
    gpio_write_value(GPIO_BUZZER_PIN, 0);

    return 0;
}

/* --- Initialisation SHM --- */

static int shm_init(void)
{
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        syslog(LOG_ERR, "shm_open échoué: %s", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, sizeof(alarm_shared_data_t)) < 0) {
        syslog(LOG_ERR, "ftruncate échoué: %s", strerror(errno));
        close(fd);
        return -1;
    }

    shared_data = mmap(NULL, sizeof(alarm_shared_data_t),
                        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shared_data == MAP_FAILED) {
        syslog(LOG_ERR, "mmap échoué: %s", strerror(errno));
        return -1;
    }

    memset(shared_data, 0, sizeof(alarm_shared_data_t));
    shared_data->state = ALARM_STATE_DISARMED;

    shm_semaphore = sem_open(SHM_SEM_NAME, O_CREAT, 0666, 1);
    if (shm_semaphore == SEM_FAILED) {
        syslog(LOG_ERR, "sem_open échoué: %s", strerror(errno));
        return -1;
    }

    return 0;
}

/* --- Main --- */

int main(void)
{
    pthread_t tid_gpio, tid_actuator, tid_control;

    openlog("alarm_daemon", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "Démarrage du démon d'alarme");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (gpio_init_all() < 0) {
        syslog(LOG_ERR, "Échec initialisation GPIO");
        return EXIT_FAILURE;
    }

    if (shm_init() < 0) {
        syslog(LOG_ERR, "Échec initialisation SHM");
        return EXIT_FAILURE;
    }

    pthread_create(&tid_gpio, NULL, thread_gpio_monitor, NULL);
    pthread_create(&tid_actuator, NULL, thread_actuator, NULL);
    pthread_create(&tid_control, NULL, thread_control_listener, NULL);

    syslog(LOG_INFO, "Démon prêt - état initial: DISARMED");

    pthread_join(tid_gpio, NULL);
    pthread_join(tid_actuator, NULL);
    pthread_join(tid_control, NULL);

    munmap(shared_data, sizeof(alarm_shared_data_t));
    sem_close(shm_semaphore);

    syslog(LOG_INFO, "Démon arrêté proprement");
    closelog();
    return EXIT_SUCCESS;
}