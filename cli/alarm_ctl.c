#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>
#include "../common/alarm_common.h"

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s {arm|disarm|status|reset}\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    alarm_cmd_t cmd;

    if (strcmp(argv[1], "arm") == 0) {
        cmd = CMD_ARM;

    } else if (strcmp(argv[1], "disarm") == 0) {
        cmd = CMD_DISARM;

    } else if (strcmp(argv[1], "status") == 0) {

        int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
        if (fd < 0) {
            fprintf(stderr, "Erreur: le daemon est-il lancé ? (%s)\n",
                    strerror(errno));
            return EXIT_FAILURE;
        }

        alarm_shared_data_t *data = mmap(NULL, sizeof(alarm_shared_data_t),
                                          PROT_READ, MAP_SHARED, fd, 0);
        close(fd);

        if (data == MAP_FAILED) {
            fprintf(stderr, "Erreur mmap: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }

        sem_t *sem = sem_open(SHM_SEM_NAME, 0);
        sem_wait(sem);

        const char *state_str[] = {"DISARMED", "ARMED", "TRIGGERED"};
        printf("État          : %s\n", state_str[data->state]);
        printf("Déclenchements: %u\n", data->trigger_count);
        printf("LED           : %s\n", data->led_status ? "ON" : "OFF");
        printf("Buzzer        : %s\n", data->buzzer_status ? "ON" : "OFF");
        if (data->last_trigger_time > 0)
            printf("Dernier décl. : %s", ctime(&data->last_trigger_time));

        sem_post(sem);
        sem_close(sem);
        munmap(data, sizeof(alarm_shared_data_t));

        return EXIT_SUCCESS;

    } else if (strcmp(argv[1], "reset") == 0) {
        cmd = CMD_RESET;

    } else {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int fd = open(CTL_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Erreur: impossible d'ouvrir le FIFO (%s)\n"
                        "Le daemon est-il lancé ?\n", strerror(errno));
        return EXIT_FAILURE;
    }

    write(fd, &cmd, sizeof(cmd));
    close(fd);

    printf("Commande '%s' envoyée\n", argv[1]);
    return EXIT_SUCCESS;
}