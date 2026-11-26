/*
 * @file ssh_console.h
 * @brief Offer an application command prompt over SSH.
 * @author Ajay Varghese
 * @date 2026-11-26
*/

#ifndef RIFT_SSH_CONSOLE_H
#define RIFT_SSH_CONSOLE_H

/* Includes */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

/* Command interface */

typedef int (*ssh_console_command_t)(const char *line, FILE *reply, bool *restart);

typedef struct
{
    uint16_t port;
    const char *username;
    const char *password;
    const char *prompt;
    ssh_console_command_t command;
} ssh_console_config_t;

/*
 * @brief Start a single-session SSH console with a saved host key.
 * @param config Port, login, prompt, and command handler with storage for the lifetime of the app.
 * @return ESP_OK when the task starts or an ESP-IDF error code.
*/
esp_err_t ssh_console_start(const ssh_console_config_t *config);

#endif
