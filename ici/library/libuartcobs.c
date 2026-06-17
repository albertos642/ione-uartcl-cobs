#include "platform.h"
#include <string.h>

int parseUartCobsSpec(char *socketSpec, struct uartcobsdescriptor *uart)
{
    const char  ch = ',';
    char        *delimiter;
    char        *baudRate;
    char        *socketSpecCopy = strdup(socketSpec);

    delimiter = strchr(socketSpecCopy, ch);
    baudRate = delimiter + 1;

    if (socketSpecCopy == NULL || *socketSpecCopy == '\0') {
        writeMemoNote("[I] UART-COBS: Cannot parse UART socket descriptor!", socketSpecCopy);
        return -1;
    }

    int n = strcspn(socketSpecCopy, ",");
    *delimiter = '\0';
    strncpy(uart->uart_file_descriptor, socketSpecCopy, n + 1);

    if (strlen(uart->uart_file_descriptor) != 0) {
        if (strlen(baudRate) != 0){
            uart->baud_rate = atoi(baudRate);
            return 0;
        } else {
            writeMemoNote("[I] UART-COBS: Baud rate missing!", socketSpecCopy);
            return -1;
        }
    }
    writeMemoNote("[I] UART-COBS: Parsing error!", socketSpecCopy);
    return -1;
}
