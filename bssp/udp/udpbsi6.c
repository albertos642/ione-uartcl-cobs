/*
 *	udpbsi6.c:	IPv6 BSSP UDP-based link service input daemon.
 *
 *	Author:  Scott Mitchell Johnson 
 *	Based on udpbsi.c by:
 *		 Sotirios-Angelos Lenas, SPICE
 *		 Scott Burleigh, JPL
 *
 *	Copyright (c) 2023, Spacely Packets, LLC.
 *	This program is free software; you can redistribute it and/or modify
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

static void	interruptThread(int signum)
{
	isignal(SIGTERM, interruptThread);
	ionKillMainThread("udpbsi6");
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
	char			*procName = "udpbsi6";
	char			*buffer;
	int			blockLength;
	struct sockaddr_in6	fromAddr;
	socklen_t		fromSize;

	snooze(1);	/*	Let main thread become interruptable.	*/
	buffer = MTAKE(UDPBSA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("udpbsi can't get UDP buffer.", NULL);
		ionKillMainThread(procName);
		return NULL;
	}

	/*	Can now start receiving bundles.  On failure, take
	 *	down the BSI.						*/

	while (rtp->running)
	{	
		fromSize = sizeof fromAddr;
		blockLength = irecvfrom(rtp->linkSocket, buffer, UDPBSA_BUFSZ,
				0, (struct sockaddr *) &fromAddr, &fromSize);
		switch (blockLength)
		{
		case -1:
			putSysErrmsg("Can't acquire block", NULL);
			ionKillMainThread(procName);

			/*	Intentional fall-through to next case.	*/

		case 0:
		case 1:				/*	Normal stop.	*/
			rtp->running = 0;
			continue;
		}

		if (bsspHandleInboundBlock(buffer, blockLength) < 0)
		{
			putErrmsg("Can't handle inbound block.", NULL);
			ionKillMainThread(procName);
			rtp->running = 0;
			continue;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	writeErrmsgMemos();
	writeMemo("[i] udpbsi6 receiver thread has ended.");

	/*	Free resources.						*/

	MRELEASE(buffer);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (ION_LWT)
int	udpbsi(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*endpointSpec = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*endpointSpec = (argc > 1 ? argv[1] : NULL);
#endif
	Sdr			sdr;
	char			beBsiCmd[256];
	BsspVseat		*vseat;
	PsmAddress		vseatElt;
	struct sockaddr_in6	inetName;
	ReceiverThreadParms	rtp;
	socklen_t		nameLength;
	pthread_t		receiverThread;
	int			fd;
	char			quit = '\0';

	/*	Note that bsspadmin must be run before the first
	 *	invocation of bsspbsi, to initialize the BSSP database
	 *	(as necessary) and dynamic database.			*/ 

	if (bsspInit(0) < 0)
	{
		putErrmsg("udpbsi6 can't initialize BSSP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	isprintf(beBsiCmd, sizeof beBsiCmd, "udpbsi6 %s", endpointSpec);
	CHKERR(sdr_begin_xn(sdr));
	findBsspSeat(beBsiCmd, NULL, &vseat, &vseatElt);
	sdr_exit_xn(sdr);
	if (vseatElt == 0)
	{
		putErrmsg("Undefined BE-BSI", beBsiCmd);
		return 1;
	}

	if (vseat->beBsiPid != ERROR && vseat->beBsiPid != sm_TaskIdSelf())
	{
		putErrmsg("BE-BSI task is already started.",
				itoa(vseat->beBsiPid));
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	if (endpointSpec)
	{
		if(parseSocketSpecSix(endpointSpec, (struct sockaddr_in6 *) &inetName) != 0)
		{
			putErrmsg("BE-BSI Can't get IP/port for endpointSpec.",
					endpointSpec);
			return -1;
		}
	}

	if (inetName.sin6_port == 0)
	{
		inetName.sin6_port = htons(BsspUdpDefaultPortNbr);
	}

	rtp.linkSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("BE-BSI can't open UDP socket", NULL);
		return -1;
	}

	nameLength = sizeof(inetName);
	if (reUseAddress(rtp.linkSocket)
	|| bind(rtp.linkSocket, (struct sockaddr *) &inetName, nameLength) < 0
	|| getsockname(rtp.linkSocket, (struct sockaddr *) &inetName, &nameLength) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("BE-BSI Can't initialize socket", NULL);
		return 1;
	}

	/*	Set up signal handling; SIGTERM is shutdown signal.	*/

	ionNoteMainThread("udpbsi6");
	isignal(SIGTERM, interruptThread);

	/*	Start the receiver thread.				*/

	rtp.running = 1;
	if (pthread_begin(&receiverThread, NULL, handleDatagrams, &rtp, "udpbsi6_receiver"))
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("udpbsi6 can't create receiver thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the link service.			*/

	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] udpbsi is running, spec=[%s:%d].", 
			endpointSpec, ntohs(inetName.sin6_port));
		writeMemo(txt);
	}

	ionPauseMainThread(-1);

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by opening a single-use
	 *	transmission socket and sending a 1-byte datagram
	 *	to the reception socket.				*/

	fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		if (isendto(fd, &quit, 1, 0, (struct sockaddr *) &inetName,
				sizeof(struct sockaddr_in6)) == 1)
		{
			pthread_join(receiverThread, NULL);
		}

		closesocket(fd);
	}

	closesocket(rtp.linkSocket);
	writeErrmsgMemos();
	writeMemo("[i] udpbsi6 has ended.");
	ionDetach();
	return 0;
}
