#include "platform.h"
#include <string.h>
#include <stdlib.h>

int parseUartCobsSpec(char *socketSpec, struct uartcobsdescriptor *uart)
{
    const char  ch = ',';
    char        *delimiter;
    char        *baudRate;
    char        *socketSpecCopy;

    if (socketSpec == NULL || *socketSpec == '\0' || uart == NULL) {
        writeMemo("[I] UART-COBS: Cannot parse empty or NULL UART socket descriptor!");
        return -1;
    }

    socketSpecCopy = strdup(socketSpec);
    if (socketSpecCopy == NULL) {
        return -1;
    }

    delimiter = strchr(socketSpecCopy, ',');
    if (delimiter == NULL) {
        delimiter = strchr(socketSpecCopy, ':');
    }
    if (delimiter == NULL) {
        writeMemoNote("[I] UART-COBS: Delimiter ',' or ':' missing in socket descriptor!", socketSpecCopy);
        free(socketSpecCopy);
        return -1;
    }

    *delimiter = '\0';
    baudRate = delimiter + 1;

    memset(uart->uart_file_descriptor, 0, sizeof(uart->uart_file_descriptor));
    strncpy(uart->uart_file_descriptor, socketSpecCopy, sizeof(uart->uart_file_descriptor) - 1);

    if (strlen(uart->uart_file_descriptor) != 0) {
        if (strlen(baudRate) != 0) {
            uart->baud_rate = (uint32_t)atoi(baudRate);
            free(socketSpecCopy);
            return 0;
        } else {
            writeMemoNote("[I] UART-COBS: Baud rate missing!", socketSpec);
            free(socketSpecCopy);
            return -1;
        }
    }

    writeMemoNote("[I] UART-COBS: Empty device path in socket descriptor!", socketSpec);
    free(socketSpecCopy);
    return -1;
}

