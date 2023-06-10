/*
 *	libtcpbsa6.c:	common functions for IPv6 TCP BSSP-based
 *			link service adapter modules.
 * 
 *			Author: Scott Johnson
 *       Copyright (c) 2023, Spacely Packets, LLC
 *
 *	Based on libtcpbsa.c by:
 *			 Sotirios-Angelos Lenas, SPICE
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., at:
 *               51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include "tcpbsa.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>


int	tcpDelayEnabled = 0;
int	tcpDelayNsecPerByte = 0;
static int bsspTcpConnectionOK = 1;

/*	*	*	Sender functions	*	*	*	*/

int	connectToBSI6(struct sockaddr_in6 *sn, int *sock)
{
	*sock = -1;
	if (sn == NULL)
	{

		return -1;	/*	Silently give up on connection.	*/
	}

	*sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (*sock < 0)
	{
		putSysErrmsg("BSO can't open TCP socket", NULL);
		return -1;
	}

	if (connect(*sock, (struct sockaddr *) sn, sizeof(struct sockaddr_in6))
			< 0)
	{
		closesocket(*sock);
		*sock = -1;
		/* suppress errmsg during long outage */
		if (bsspTcpConnectionOK == 1){
			putSysErrmsg("BSO can't connect to TCP socket", NULL);
			bsspTcpConnectionOK = 0;
		}
		return -1;
	}

	writeMemo("[i] tcpbso6 connection established.");
	bsspTcpConnectionOK = 1;

	return 0;
}

int	sendBlockByTCP6(struct sockaddr_in6 *socketName, int *blockSocket,
		int blockLength, char *block)
{
	int	header = htonl(blockLength);
	int	bytesSent;

	/*	Connect to BSI as necessary.				*/

	if (*blockSocket < 0)
	{
		if (connectToBSI6(socketName, blockSocket) < 0)
		{
			/*	Treat I/O error as a transient anomaly.	*/

			return 0;
		}
	}

	bytesSent = itcp_send(blockSocket, (char *) &header, 4);
	if (bytesSent < 0)
	{
		/*	Big problem; shut down.				*/

		putErrmsg("Failed to send preamble by TCP.", NULL);
		return -1;
	}

	if (bytesSent == 0)
	{
		/*	Just lost connection; treat as a transient
		 *	anomaly, note incomplete transmission.		*/

		writeMemo("[?] Lost connection to TCP BSI.");
		closesocket(*blockSocket);
		*blockSocket = -1;
		return 0;
	}

	if (blockLength == 0)		/*	Just a keep-alive.	*/
	{
		return 1;	/*	Impossible length; means "OK".	*/
	}

	bytesSent = itcp_send(blockSocket, block, blockLength);
	if (bytesSent < 0)
	{
		/*	Big problem; shut down.			*/

		putErrmsg("Failed to send block by TCP.", NULL);
		return -1;
	}

	if (bytesSent == 0)
	{
		/*	Just lost connection; treat as a transient
		 *	anomaly, note incomplete transmission.		*/

		writeMemo("[?] Lost connection to TCP BSI.");
		closesocket(*blockSocket);
		*blockSocket = -1;
		return 0;
	}

	return blockLength;
}
