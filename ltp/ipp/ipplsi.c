/*
	ipplsi.c:	Input daemon for the LTP link service based
			on GRO/GSO and IP parcels.

	Author: Fred Templin, Boeing Corp.

	Adapted from udplsi.c.

	Copyright (c) 2023, Boeing Corp.  ALL RIGHTS RESERVED.
									*/
#include "ipplsa.h"

static void	interruptThread(int signum)
{
	isignal(SIGTERM, interruptThread);
	ionKillMainThread("ipplsi");
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (ION_LWT)
int	ipplsi(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char	*endpointSpec = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*endpointSpec = (argc > 1 ? argv[1] : NULL);
#endif
	Sdr			sdr;
	char			lsiCmd[256];
	LtpVseat		*vseat;
	PsmAddress		vseatElt;
	unsigned short		portNbr = 0;
	unsigned int		ipAddress = INADDR_ANY;
	struct sockaddr		ownSockName;
	struct sockaddr_in	*inetName;
	ReceiverThreadParms	rtp;
	socklen_t		nameLength;
	pthread_t		receiverThread;
	int			fd;
	char			quit = '\0';
	unsigned int		groEnable = 1;	/* boolean */
#ifdef LTPPARCEL_CSUM_RX
	unsigned int		checkRx = 1002;	/* turn of kernel checkRx */
#endif /* LTPPARCEL_CSUM_RX */

	/*	Note that ltpadmin must be run before the first
	 *	invocation of ltplsi, to initialize the LTP database
	 *	(as necessary) and dynamic database.			*/ 

	if (ltpInit(0) < 0)
	{
		putErrmsg("ipplsi can't initialize LTP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	isprintf(lsiCmd, sizeof lsiCmd, "ipplsi %s", endpointSpec);
	CHKERR(sdr_begin_xn(sdr));
	findSeat(lsiCmd, &vseat, &vseatElt);
	sdr_exit_xn(sdr);
	if (vseatElt == 0)
	{
		putErrmsg("Undefined LSI", lsiCmd);
		return 1;
	}

	if (vseat->lsiPid != ERROR && vseat->lsiPid != sm_TaskIdSelf())
	{
		putErrmsg("LSI task is already started.", itoa(vseat->lsiPid));
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	if (endpointSpec)
	{
		if(parseSocketSpec(endpointSpec, &portNbr, &ipAddress) != 0)
		{
			putErrmsg("Can't get IP/port for endpointSpec.",
					endpointSpec);
			return -1;
		}
	}

	if (portNbr == 0)
	{
		portNbr = LtpIppDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	ipAddress = htonl(ipAddress);
	memset((char *) &ownSockName, 0, sizeof ownSockName);
	inetName = (struct sockaddr_in *) &ownSockName;
	inetName->sin_family = AF_INET;
	inetName->sin_port = portNbr;
	memcpy((char *) &(inetName->sin_addr.s_addr), (char *) &ipAddress, 4);
	rtp.linkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("LSI can't open UDP socket", NULL);
		return -1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(rtp.linkSocket)
	|| bind(rtp.linkSocket, &ownSockName, nameLength) < 0
	|| getsockname(rtp.linkSocket, &ownSockName, &nameLength) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("LSI can't initialize UDP socket", NULL);
		return 1;
	}

	if (setsockopt (rtp.linkSocket, SOL_UDP, UDP_GRO,
			&groEnable, sizeof(groEnable)) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("LSI can't set GRO", NULL);
		return 1;
	}

#ifdef LTPPARCEL_CSUM_RX
	/* Set sk_no_check_rx */
	if (setsockopt (rtp.linkSocket, SOL_SOCKET, SO_NO_CHECK,
			&checkRx, sizeof(checkRx)) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("LSI can't set checkRx", NULL);
		return 1;
	}
#endif /* LTPPARCEL_CSUM_RX */

	/*	Set up signal handling; SIGTERM is shutdown signal.	*/

	ionNoteMainThread("ipplsi");
	isignal(SIGTERM, interruptThread);

	/*	Start the receiver thread.				*/

	rtp.running = 1;
#ifdef LTPSTAT
	rtp.sendSegs = 0;
	rtp.recvSegs = 0;
	rtp.recvGRO = 0;
	rtp.recvBigMsgs = 0;
	rtp.recvBigBytes = 0;
#endif /* LTPSTAT */
	if (pthread_begin(&receiverThread, NULL, ipplsa_handle_datagrams,
			&rtp, "ipplsi_receiver"))
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("ipplsi can't create receiver thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the link service.			*/

	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] ipplsi is running, spec=[%s:%d].", 
			inet_ntoa(inetName->sin_addr), ntohs(portNbr));
		writeMemo(txt);
	}

	ionPauseMainThread(-1);

	/*	Time to shut down.					*/

	rtp.running = 0;

	/*	Wake up the receiver thread by opening a single-use
	 *	transmission socket and sending a 1-byte datagram
	 *	to the reception socket.				*/

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd >= 0)
	{
		if (isendto(fd, &quit, 1, 0, &ownSockName,
				sizeof(struct sockaddr)) == 1)
		{
			pthread_join(receiverThread, NULL);
		}

		closesocket(fd);
	}

	closesocket(rtp.linkSocket);
	writeErrmsgMemos();
	writeMemo("[i] ipplsi has ended.");
	ionDetach();
	return 0;
}
