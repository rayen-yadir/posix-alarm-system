#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>
#include "../common/alarm_common.h"

static volatile int running = 1;

static void signal_handler(int sig)
{
    running = 0;
}

static const char *state_to_string(alarm_state_t s)
{
    switch (s) {
        case ALARM_STATE_DISARMED:  return "DISARMED";
        case ALARM_STATE_ARMED:     return "ARMED";
        case ALARM_STATE_TRIGGERED: return "TRIGGERED";
        default: return "UNKNOWN";
    }
}

int main(void)
{
    openlog("alarm_logger", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "Démarrage du logger d'alarme");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) {
        syslog(LOG_ERR, "shm_open échoué (le daemon est-il lancé ?): %s",
               strerror(errno));
        return EXIT_FAILURE;
    }

    alarm_shared_data_t *data = mmap(NULL, sizeof(alarm_shared_data_t),
                                      PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (data == MAP_FAILED) {
        syslog(LOG_ERR, "mmap échoué: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    sem_t *sem = sem_open(SHM_SEM_NAME, 0);
    if (sem == SEM_FAILED) {
        syslog(LOG_ERR, "sem_open échoué: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    alarm_state_t last_state = -1;
    uint32_t last_count = 0;

    while (running) {
        sem_wait(sem);

        alarm_state_t current_state = data->state;
        uint32_t current_count = data->trigger_count;

        sem_post(sem);

        if (current_state != last_state) {
            syslog(LOG_INFO, "Changement d'état: %s -> %s",
                   state_to_string(last_state),
                   state_to_string(current_state));
            last_state = current_state;
        }

        if (current_count != last_count) {
            syslog(LOG_WARNING, "Déclenchement #%u détecté", current_count);
            last_count = current_count;
        }

        usleep(500000);
    }

    munmap(data, sizeof(alarm_shared_data_t));
    sem_close(sem);
    syslog(LOG_INFO, "Logger arrêté");
    closelog();
    return EXIT_SUCCESS;
}