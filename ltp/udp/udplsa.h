/*
 	udplsa.h:	common definitions for UDP link service
			adapter modules.

	Author: Scott Burleigh, JPL

	Copyright (c) 2007, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
 									*/
#ifndef _UDPLSA_H_
#define _UDPLSA_H_

#include "ltpP.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDPLSA_BUFSZ		((256 * 256) - 1)
#define LtpUdpDefaultPortNbr	1113

typedef struct
{
	int			linkSocket;
	int			running;
#ifdef LTPPARCEL
	int			parcelSocket;
#endif
#ifdef LTPSTAT
	int			sendSegs;
	int			recvSegs;
	int			recvGRO;
	int			recvBigMsgs;
	int			recvBigBytes;
#endif
} ReceiverThreadParms;

extern void			*udplsa_handle_datagrams(void *parm);

#ifdef LTPPARCEL
/* Internet checksum from linux kernel lib/checksum.c */
static inline unsigned short from32to16(unsigned int x)
{
	/* add up 16-bit and 16-bit for 16+c bit */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

static unsigned int do_csum(const unsigned char *buff, int len)
{
	int odd;
	unsigned int result = 0;

	if (len <= 0)
		goto out;
	odd = 1 & (unsigned long) buff;
	if (odd) {
#ifdef __LITTLE_ENDIAN
		result += (*buff << 8);
#else
		result = *buff;
#endif
		len--;
		buff++;
	}
	if (len >= 2) {
		if (2 & (unsigned long) buff) {
			result += *(unsigned short *) buff;
			len -= 2;
			buff += 2;
		}
		if (len >= 4) {
			const unsigned char *end = buff + ((unsigned)len & ~3);
			unsigned int carry = 0;
			do {
				unsigned int w = *(unsigned int *) buff;
				buff += 4;
				result += carry;
				result += w;
				carry = (w > result);
			} while (buff < end);
			result += carry;
			result = (result & 0xffff) + (result >> 16);
		}
		if (len & 2) {
			result += *(unsigned short *) buff;
			buff += 2;
		}
	}
	if (len & 1)
#ifdef __LITTLE_ENDIAN
		result += *buff;
#else
		result += (*buff << 8);
#endif
	result = from32to16(result);
	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);
out:
	return result;
}

uint16_t in_csum(char *buf, int len) {
	const unsigned char *buff = (unsigned char *)buf;
	uint16_t result =(~do_csum(buff, len) & 0xffff);
	return (result);
}

#ifdef LTPPARCEL_NOTDEF
/* Internet checksum from web: http://www.microhowto.info/howto/calculate_an_internet_protocol_checksum_in_c.html */
uint16_t in_csum(char *data, int length) {

	uint32_t acc = 0xffff;
	int i;

	for (i = 0; (i + 1) < length; i += 2) {
		uint16_t word;
		memcpy(&word, (data + i), 2);
		acc += ntohs(word);
		if (acc > 0xffff) {
			acc -= 0xffff;
		}
	}

	if (length & 1) {
		uint16_t word = 0;
		memcpy(&word, (data + (length -1)), 1);
		acc += ntohs(word);
		if (acc > 0xffff) {
			acc -= 0xffff;
		}
	}

	return htons(~acc);
}

/* Internet checksum from RFC1071 (does not handle endianness) */
static inline unsigned short in_csum(char *buf, int len)
{
	unsigned int sum = 0;
	
	while (len > 1) {
		sum += ntohs(*(unsigned short *)buf);
		len -= 2;
		buf += 2;
	}

	if (len > 0)
		sum += *(unsigned char *)buf;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	
	return (~sum);
}
#endif /* LTPPARCEL_NOTDEF */
#endif /* LTPPARCEL */

#ifdef __cplusplus
}
#endif

#endif	/* _UDPLSA_H */
