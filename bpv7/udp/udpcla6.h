/*
 	udpcla6.h:	common definitions for IPv6 UDP convergence layer
			adapter modules.

        Author: Scott Johnson
        based on udpcla.h by Scott Burleigh and Ted Piotrowski
        Copyright (c) 2022, Scott Mitchell Johnson

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., at:
		51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 									*/
#ifndef _UDPCLA_H_
#define _UDPCLA_H_

#include "bpP.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDPCLA_BUFSZ		((256 * 256) - 1)

extern int	sendBytesBy6UDP(int *bundleSocket, char *from, int length,
			struct sockaddr_in6 *socketName);
extern int	sendBundleBy6UDP(struct sockaddr_in6 *socketName,
			int *bundleSocket, unsigned int bundleLength,
			Object bundleZco, unsigned char *buffer);
extern int	receiveBytesBy6UDP(int bundleSocket,
			struct sockaddr_in6 *fromAddr,char *into, int length);

#ifdef __cplusplus
}
#endif

#endif	/* _UDPCLA_H */
