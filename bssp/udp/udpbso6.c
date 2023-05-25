/*
 *      udpbso6.c:      IPv6 BSSP UDP-based link service output daemon.
 *
 *      Author:  Scott Mitchell Johnson 
 *      Based on udpbso.c by:
 *               Sotirios-Angelos Lenas, SPICE
 *               Scott Burleigh, JPL
 *
 *      Copyright (c) 2023, Spacely Packets, LLC.
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.

 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., at:
 *              51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "udpbsa.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>



#if defined(linux)

#define IPHDR_SIZE	(sizeof(struct iphdr) + sizeof(struct udphdr))

#elif defined(mingw)

#define IPHDR_SIZE	(20 + 8)

#else

#include "netinet/ip_var.h"
#include "netinet/udp_var.h"

#define IPHDR_SIZE	(sizeof(struct udpiphdr))

#endif

static sm_SemId		udpbsoSemaphore(sm_SemId *semid)
{
	static sm_SemId	semaphore = -1;
	
	if (semid)
	{
		semaphore = *semid;
	}

	return semaphore;
}

static void	shutDownBso()	/*	Commands LSO termination.	*/
{
	sm_SemEnd(udpbsoSemaphore(NULL));
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	int		linkSocket;
	int		running;
} ReceiverThreadParms;

static void	*handleDatagrams(void *parm)
{
	/*	Main loop for UDP datagram reception and handling.	*/

	ReceiverThreadParms	*rtp = (ReceiverThreadParms *) parm;
	char			*buffer;
	int			blockLength;
	struct sockaddr_in6	fromAddr;
	socklen_t		fromSize;

	snooze(1);	/*	Let main thread become interruptible.	*/

	/*	Initialize buffer.					*/

	buffer = MTAKE(UDPBSA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpbsi6 can't get UDP buffer.", NULL);
		shutDownBso();
		return NULL;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the BSO.						*/

	iblock(SIGTERM);
	while (rtp->running)
	{	
		fromSize = sizeof fromAddr;
		blockLength = irecvfrom(rtp->linkSocket, buffer, UDPBSA_BUFSZ,
				0, (struct sockaddr *) &fromAddr, &fromSize);
		switch (blockLength)
		{
		case -1:
			putSysErrmsg("Can't acquire block", NULL);
			shutDownBso();

			/*	Intentional fall-through to next case.	*/

		case 0:
		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;
		}

		if (bsspHandleInboundBlock(buffer, blockLength) < 0)
		{
			putErrmsg("Can't handle inbound block.", NULL);
			shutDownBso();
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	writeMemo("[i] udpbso6 receiver thread has ended.");
	writeErrmsgMemos();

	/*	Free resources.						*/

	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

int	sendBlockByUDP(int linkSocket, char *from, int length,
		struct sockaddr_in6 *destAddr )
{
	int	bytesWritten;

	while (1)	/*	Continue until not interrupted.		*/
	{
		bytesWritten = isendto(linkSocket, from, length, 0,
				(struct sockaddr *) destAddr,
				sizeof(struct sockaddr_in6));
		if (bytesWritten < 0)
		{
			if (errno == EINTR)	/*	Interrupted.	*/
			{
				continue;	/*	Retry.		*/
			}

			if (errno == ENETUNREACH)
			{
				return length;	/*	Just data loss.	*/
			}

			{
				char			memoBuf[1000];
				struct sockaddr_in6	*saddr = destAddr;
				char	dAddr;

				isprintf(memoBuf, sizeof(memoBuf),
					"udpbso6 sendto() error, dest=[%s:%d], nbytes=%d, rv=%d, errno=%d", inet_ntop(AF_INET6, saddr, &dAddr, sizeof(dAddr)), 
					ntohs(saddr->sin6_port), 
					length, bytesWritten, errno);
				writeMemo(memoBuf);
			}
		}

		return bytesWritten;
	}
}

static unsigned long	getUsecTimestamp()
{
	struct timeval	tv;

	getCurrentTime(&tv);
	return ((tv.tv_sec * 1000000) + tv.tv_usec);
}

#if defined (ION_LWT)
int	udpbso6(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
	       saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*endpointSpec = (char *) a1;
	unsigned int	txbps = (a2 != 0 ?  strtoul((char *) a2, NULL, 0) : 0);
	uvast		remoteEngineId = a3 != 0 ?  strtouvast((char *) a3) : 0;
#else
int	main(int argc, char *argv[])
{
	char		*endpointSpec = argc > 1 ? argv[1] : NULL;
	unsigned int	txbps = (argc > 2 ?  strtoul(argv[2], NULL, 0) : 0);
	uvast		remoteEngineId = argc > 3 ? strtouvast(argv[3]) : 0;
#endif
	Sdr			sdr;
	BsspVspan		*vspan;
	PsmAddress		vspanElt;
	struct sockaddr_in6	ownInetName;
	struct sockaddr_in6	peerInetName;
	socklen_t		nameLength;
	ReceiverThreadParms	rtp;
	pthread_t		receiverThread;
	IonNeighbor		*neighbor;
	PsmAddress		nextElt;
	int			blockLength;
	char			*block;
	int			bytesSent;
	int			fd;
	char			quit = '\0';

	/*	Rate control calculation is based on treating elapsed
	 *	time as a currency.					*/

	float			timeCostPerByte;/*	In seconds.	*/
	unsigned long		startTimestamp;	/*	Billing cycle.	*/
	unsigned int		totalPaid;	/*	Since last send.*/
	unsigned int		currentPaid;	/*	Sending seg.	*/
	float			totalCostSecs;	/*	For this seg.	*/
	unsigned int		totalCost;	/*	Microseconds.	*/
	unsigned int		balanceDue;	/*	Until next seg.	*/
	unsigned int		prevPaid = 0;	/*	Prior snooze.	*/

	if (txbps != 0 && remoteEngineId == 0)	/*	Now nominal.	*/
	{
		remoteEngineId = txbps;
		txbps = 0;
	}

	if (remoteEngineId == 0 || endpointSpec == NULL)
	{
		PUTS("Usage: udpbso6 {<remote engine's host name> | @}\
[:<its port number>] <remote engine ID>");
		return 0;
	}

	if (txbps != 0)
	{
		PUTS("NOTE: udpbso6 now gets its transmission data rate from \
the contact plan.  txbps is still accepted on the command line, for backward \
compatibility, but it is ignored.");
	}

	/*	Note that bsspadmin must be run before the first
	 *	invocation of bsspbso, to initialize the BSSP database
	 *	(as necessary) and dynamic database.			*/

	if (bsspInit(0) < 0)
	{
		putErrmsg("udpbso can't initialize BSSP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));	/*	Just to lock memory.	*/
	findBsspSpan(remoteEngineId, &vspan, &vspanElt);
	if (vspanElt == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No such engine in database.", itoa(remoteEngineId));
		return 1;
	}

	if (vspan->bsoBEPid != ERROR && vspan->bsoBEPid != sm_TaskIdSelf())
	{
		sdr_exit_xn(sdr);
		putErrmsg("BE-BSO task is already started for this span.",
				itoa(vspan->bsoBEPid));
		return 1;
	}

	sdr_exit_xn(sdr);

	/*	All command-line arguments are now validated.  First
	 *	get peer's socket address.				*/

	parseSocketSpecSix(endpointSpec, &peerInetName);
	if (peerInetName.sin6_port == 0)
	{
		peerInetName.sin6_port = htons(BsspUdpDefaultPortNbr);
	}


	/*	Now compute own socket address, used when the peer
	 *	responds to the link service output socket rather
	 *	than to the advertised link service input socket.	*/

        ownInetName.sin6_family = AF_INET6;
        ownInetName.sin6_addr = in6addr_any;
        ownInetName.sin6_port = htons(0);
        ownInetName.sin6_flowinfo = 0;

	/*	This socket needs to be bound to the local socket
	 *	address (just as in udpbsi), so that the udpbso
	 *	main thread can send a -1-byte datagramto that
	 *	socket to shut down the datagram handling thread.	*/

	/*	Now create the socket that will be used for sending
	 *	datagrams to the peer BSSP engine and possibly for
	 *	receiving datagrams from the peer BSSP engine.		*/

	rtp.linkSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("BE-BSO can't open UDP socket", NULL);
		return 1;
	}

	/*	Bind the socket to own socket address so that we can
	 *	send a 1-byte datagram to that address to shut down
	 *	the datagram handling thread.				*/

	nameLength = sizeof(struct sockaddr_in6);
	if (reUseAddress(rtp.linkSocket)
	|| bind(rtp.linkSocket, (struct sockaddr *) &ownInetName, nameLength) < 0
	|| getsockname(rtp.linkSocket, (struct sockaddr *) &ownInetName, &nameLength) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("BE-BSO can't bind UDP socket", NULL);
		return 1;
	}

	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(udpbsoSemaphore(&(vspan->beSemaphore)));
	signal(SIGTERM, shutDownBso);

	/*	Start the echo handler thread.				*/

	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, handleDatagrams,
			&rtp, "udpbso6_receiver"))
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("udpbso6 can't create receiver thread", NULL);
		return 1;
	}

	/*	Can now begin transmitting to remote engine.		*/

	{
		char	memoBuf[1024];

		isprintf(memoBuf, sizeof(memoBuf),
			"[i] udpbso6 is running, spec=[%s:%d], txbps=%d (0=unlimited), rengine=%d.", endpointSpec, ntohs(peerInetName.sin6_port), txbps, (int) remoteEngineId);
		writeMemo(memoBuf);
	}

	neighbor = findNeighbor(getIonVdb(), remoteEngineId, &nextElt);
	startTimestamp = getUsecTimestamp();
	while (rtp.running && !(sm_SemEnded(vspan->beSemaphore)))
	{
		blockLength = bsspDequeueBEOutboundBlock(vspan, &block);
		if (blockLength < 0)
		{
			rtp.running = 0;	/*	Terminate LSO.	*/
			continue;
		}

		if (blockLength == 0)		/*	Interrupted.	*/
		{
			continue;
		}

		if (blockLength > UDPBSA_BUFSZ)
		{
			putErrmsg("Block is too big for UDP BSO.",
					itoa(blockLength));
			rtp.running = 0;	/*	Terminate LSO.	*/
			continue;
		}

		bytesSent = sendBlockByUDP(rtp.linkSocket, block, blockLength,
				&peerInetName);
		if (bytesSent < blockLength)
		{
			rtp.running = 0;	/*	Terminate BSO.	*/
			continue;
		}

		/*	Rate control calculation is based on treating
		 *	elapsed time as a currency, the price you pay
		 *	(by microsnooze) for sending a block of a given
		 *	size.  All cost figures are expressed in
		 *	microseconds except the computed totalCostSecs
		 *	of the block.					*/

		totalPaid = getUsecTimestamp() - startTimestamp;

		/*	Start clock for next bill.			*/

		startTimestamp = getUsecTimestamp();

		/*	Compute time balance due.			*/

		if (totalPaid >= prevPaid)
		{
		/*	This should always be true provided that
		 *	clock_gettime() is supported by the O/S.	*/

			currentPaid = totalPaid - prevPaid;
		}
		else
		{
			currentPaid = 0;
		}

		/*	Get current time cost, in seconds, per byte.	*/

		if (neighbor && neighbor->xmitRate > 0)
		{
			timeCostPerByte = 1.0 / (neighbor->xmitRate);
		}
		else	/*	No link service rate control.		*/ 
		{
			timeCostPerByte = 0.0;
		}

		totalCostSecs = timeCostPerByte * (IPHDR_SIZE + blockLength);
		totalCost = totalCostSecs * 1000000.0;	/*	usec.	*/
		if (totalCost > currentPaid)
		{
			balanceDue = totalCost - currentPaid;
		}
		else
		{
			balanceDue = 1;
		}

		microsnooze(balanceDue);
		prevPaid = balanceDue;

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by opening a single-use
	 *	transmission socket and sending a 1-byte datagram
	 *	to the reception socket.				*/

	fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		if (isendto(fd, &quit, 1, 0, (struct sockaddr *) &ownInetName,
				sizeof(struct sockaddr_in6)) == 1)
		{
			pthread_join(receiverThread, NULL);
		}

		closesocket(fd);
	}

	closesocket(rtp.linkSocket);
	writeErrmsgMemos();
	writeMemo("[i] udpbso6 has ended.");
	ionDetach();
	return 0;
}
