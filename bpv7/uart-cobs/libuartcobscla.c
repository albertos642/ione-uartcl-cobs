#include "uartcobscla.h"
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>

static int openUartCobsPort(int *uartPort, struct uartcobsdescriptor *uartDes,
                            int mode) {
    int fd = open(uartDes->uart_file_descriptor, mode | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (fd < 0) {
        writeMemo("[i] UART-COBS: Error opening specified port.");
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return -1;

    tty.c_cflag &= ~PARENB;
  	tty.c_cflag &= ~CSTOPB;
  	tty.c_cflag &= ~CSIZE;
  	tty.c_cflag |= CS8;
  	tty.c_cflag &= ~CRTSCTS;
  	tty.c_cflag |= CREAD | CLOCAL;

	tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);
  	tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
	tty.c_oflag &= ~(OPOST | ONLCR);

    tty.c_cc[VTIME] = 0; // Do not block
    tty.c_cc[VMIN]  = 0; // Return data immediately

    speed_t speed = B115200;
    switch(uartDes->baud_rate) {
        case 9600:   speed = B9600;   break;
        case 38400:  speed = B38400;  break;
        case 115200: speed = B115200; break;
    }
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return -1;

    *uartPort = fd;
    return 0;
}

int sendBundleByUartCobs(struct uartcobsdescriptor *socketName, 
                        int *bundleSocket, unsigned int bundleLength,
                        Object bundleZco, unsigned char *cleartext_buffer)
{
    Sdr sdr = getIonsdr();
    ZcoReader reader;
    unsigned char zero = 0x00;

    if (bundleLength > UARTCOBS_MAX_PAYLOAD) return -1;

    if (*bundleSocket < 0) {
        if (openUartCobsPort(bundleSocket, socketName, O_WRONLY) < 0) return 0;
    }

    cleartext_buffer[0] = UARTCOBS_VERSION_1 | UARTCOBS_FLAG_DATA;
    zco_start_transmitting(bundleZco, &reader);
    zco_track_file_offset(&reader);
    CHKERR(sdr_begin_xn(sdr));
    int bytesRead = zco_transmit(sdr, &reader, bundleLength, (char *)(cleartext_buffer + 1));
    if (sdr_end_xn(sdr) < 0 || bytesRead < 0) return -1;

    uint16_t crc = compute_crc16(cleartext_buffer, bundleLength + 1);
    cleartext_buffer[bundleLength + 1] = (crc >> 8) & 0xFF;
    cleartext_buffer[bundleLength + 2] = crc & 0xFF;

    unsigned char *cobs_buffer = MTAKE(UARTCOBS_MAX_RAW_BUFSZ);
    if (!cobs_buffer) return -1;

    size_t cobs_len = cobs_encode(cleartext_buffer, bundleLength + 3, cobs_buffer);

    write(*bundleSocket, &zero, 1);
    int bytesSent = write(*bundleSocket, cobs_buffer, cobs_len);
    write(*bundleSocket, &zero, 1);

    MRELEASE(cobs_buffer);

    if(bytesSent < 0) {
        bpHandleXmitFailure(bundleZco);
        return (*bundleSocket == -1) ? 0 : -1;
    } else {
        bpHandleXmitSuccess(bundleZco);
        return bytesSent;
    }
}

int receiveFramesByUartCobs(int *bundleSocket, 
    struct uartcobsdescriptor *socketName, unsigned char *into_payload)
{
    static unsigned char raw_buffer[UARTCOBS_MAX_RAW_BUFSZ];
    static int raw_len = 0;
    unsigned char byte;

    if (*bundleSocket < 0) {
        if (openUartCobsPort(bundleSocket, socketName, O_RDONLY) < 0) return -1;
    }
    while (1) {
		int r = read(*bundleSocket, &byte, 1);
		
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; // Yield to CLI loop
			return -1; // Hardware fault
		}
		
		if (r == 0) return 0; // No byte available, sm_TaskYield in CLI

		if (byte == 0x00) {
			if (raw_len > 0) {
				// frame delimiter found, reuse buffer
				size_t decoded_len = cobs_decode(raw_buffer, raw_len, into_payload);
				raw_len = 0;

				// validation
				if (decoded_len >= 4) {
					if ((into_payload[0] & 0xF0) == UARTCOBS_VERSION_1) {
						uint16_t received_crc = (into_payload[decoded_len - 2] << 8) | into_payload[decoded_len - 1];
						uint16_t calculated_crc = compute_crc16(into_payload, decoded_len - 2);

						if (received_crc == calculated_crc) {
							// extract payload
							size_t payload_len = decoded_len - 3;
							memmove(into_payload, into_payload + 1, payload_len);
							return (int)payload_len;
						}
					}
				}
				return 0; 
			}
		} else {
			if (raw_len < UARTCOBS_MAX_RAW_BUFSZ) {
				raw_buffer[raw_len++] = byte;
			} else {
				raw_len = 0; // Buffer overflowed, flush
			}
		}
    }
}
