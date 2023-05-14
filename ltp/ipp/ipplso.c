/*
	ipplso.c:	Output daemon for the LTP link service based
			on GRO/GSO and IP parcels.  Each ipplso
			process is dedicated to UDP datagram
			transmission to a single remote LTP engine.

	Author: Fred Templin, Boeing Corp.

	Adapted from udplso.c.

	Copyright (c) 2023, Boeing Corp.  ALL RIGHTS RESERVED.
									*/
#include "ipplsa.h"

#if defined(linux)

#define IPHDR_SIZE	(sizeof(struct iphdr) + sizeof(struct udphdr))

#elif defined(mingw)

#define IPHDR_SIZE	(20 + 8)

#else /* defined(linux) */

#include "netinet/ip_var.h"
#include "netinet/udp_var.h"

#define IPHDR_SIZE	(sizeof(struct udpiphdr))

#endif /* defined(linux) */

static sm_SemId		ipplsoSemaphore(sm_SemId *semid)
{
	static sm_SemId	semaphore = -1;
	
	if (semid)
	{
		semaphore = *semid;
	}

	return semaphore;
}

static void	shutDownLso()	/*	Commands LSO termination.	*/
{
	sm_SemEnd(ipplsoSemaphore(NULL));
}

/*	*	*	Main thread functions	*	*	*	*/

static int	sendBatch(int linkSocket, struct mmsghdr *msgs,
			unsigned int batchLength)
{
	int	totalBytesSent = 0;
	int	bytesSent;
	int	i;

	if (sendmmsg(linkSocket, msgs, batchLength, 0) < 0)
	{
		putSysErrmsg("Failed in sendmmsg", itoa(batchLength));
		return -1;
	}

	for (i = 0; i < batchLength; i++)
	{
		bytesSent = msgs[i].msg_len;
		if (bytesSent > 0)
		{
			totalBytesSent += (IPHDR_SIZE + bytesSent);
		}
	}

	return totalBytesSent;
}

#ifdef LTPRATE
static unsigned long	getUsecTimestamp()
{
	struct timeval	tv;

	getCurrentTime(&tv);
	return ((tv.tv_sec * 1000000) + tv.tv_usec);
}

typedef struct
{
	unsigned long		startTimestamp;	/*	Billing cycle.	*/
	uvast			remoteEngineId;
	IonNeighbor		*neighbor;
	unsigned int		prevPaid;
} RateControlState;

static void	applyRateControl(RateControlState *rc, int bytesSent)
{
	/*	Rate control calculation is based on treating elapsed
	 *	time as a currency, the price you pay (by microsnooze)
	 *	for sending a given number of bytes.  All cost figures
	 *	are expressed in microseconds except the computed
	 *	totalCostSecs of the transmission.			*/

	unsigned long		totalPaid;	/*	Since last send.*/
	float			timeCostPerByte;/*	In seconds.	*/
	unsigned long		currentPaid;	/*	Sending seg.	*/
	PsmAddress		nextElt;
	float			totalCostSecs;	/*	For this seg.	*/
	unsigned long		totalCost;	/*	Microseconds.	*/
	unsigned long		balanceDue;	/*	Until next seg.	*/

	totalPaid = getUsecTimestamp() - rc->startTimestamp;

	/*	Start clock for next bill.				*/

	rc->startTimestamp = getUsecTimestamp();

	/*	Compute time balance due.				*/

	if (totalPaid >= rc->prevPaid)
	{
	/*	This should always be true provided that
	 *	clock_gettime() is supported by the O/S.		*/

		currentPaid = totalPaid - rc->prevPaid;
	}
	else
	{
		currentPaid = 0;
	}

	/*	Get current time cost, in seconds, per byte.		*/

	if (rc->neighbor == NULL)
	{
		rc->neighbor = findNeighbor(getIonVdb(), rc->remoteEngineId,
				&nextElt);
	}

	if (rc->neighbor && rc->neighbor->xmitRate > 0)
	{
		timeCostPerByte = 1.0 / (rc->neighbor->xmitRate);
	}
	else	/*	No link service rate control.			*/ 
	{
		timeCostPerByte = 0.0;
	}

	totalCostSecs = timeCostPerByte * bytesSent;
	totalCost = totalCostSecs * 1000000.0;		/*	usec.	*/
	if (totalCost > currentPaid)
	{
		balanceDue = totalCost - currentPaid;
	}
	else
	{
		balanceDue = 1;
	}

	microsnooze(balanceDue);
	rc->prevPaid = balanceDue;
}
#endif /* LTPRATE */

#if defined (ION_LWT)
int	ipplso(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
	       saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*endpointSpec = (char *) a1;
	uvast		txbps = (a2 != 0 ?  strtoul((char *) a2, NULL, 0) : 0);
	uvast		remoteEngineId = a3 != 0 ?  strtouvast((char *) a3) : 0;
#else /* defined (ION_LWT) */
int	main(int argc, char *argv[])
{
	char		*endpointSpec = argc > 1 ? argv[1] : NULL;
	uvast		txbps = (argc > 2 ?  strtoul(argv[2], NULL, 0) : 0);
	uvast		remoteEngineId = argc > 3 ? strtouvast(argv[3]) : 0;
#endif /* defined (ION_LWT) */
	Sdr			sdr;
	LtpVspan		*vspan;
	PsmAddress		vspanElt;
	unsigned short		portNbr = 0;
	unsigned int		ipAddress = 0;
	struct sockaddr		peerSockName;
	struct sockaddr_in	*peerInetName;
	char			ownHostName[MAXHOSTNAMELEN];
	struct sockaddr		ownSockName;
	struct sockaddr_in	*ownInetName;
	socklen_t		nameLength;
	ReceiverThreadParms	rtp;
	pthread_t		receiverThread;
	int			segmentLength;
	char			*segment;
	int			bytesSent;
	int			fd;
	char			quit = '\0';
	Object			spanObj;
	LtpSpan			spanBuf;
	unsigned int		batchLimit;
	unsigned int		gsoLimit;
	char			*buffers;
	char			*buffer;
	struct iovec		*iovecs;
	struct iovec		*iovec;
	struct mmsghdr		*msgs;
	struct mmsghdr		*msg;
	unsigned int		batchLength;
	struct cmsghdr		*cmsgs;
	struct cmsghdr		*cmsg;
	unsigned int		firstSeg;
	unsigned int		msgSegs;
	unsigned int		msgBytes;
	unsigned int		batchSegments;
	unsigned int		pmtudisc = IP_PMTUDISC_INTERFACE;
	/* alternate values: IP_PMTUDISC_{DONT,WANT,DO,PROBE,OMIT} */

#ifdef LTPPARCEL
	struct {
		unsigned char code;
		unsigned char len;
		unsigned int payload;
	} jumbo = {0x0b, 0x06, 0x00};
	/*
	 * is_parcel is set globally for now. Goal is to have it as a
	 * runtime variable tested on a per-message basis, with some
	 * messages sent as parcels and others sent as simple GSO.
	 */
	unsigned int		is_parcel = 1;
#ifdef LTPPARCEL_CSUM_TX
	unsigned int		checkTx = 1001;	/* Turn off kernel checkTx */
#endif /* LTPPARCEL_CSUM_TX */
	char			*checksums;
	char			*checksum;
	struct iovec		*cvec;
	uint16_t		*csum;
#endif /* LTPPARCEL */
#ifdef LTPGSO_NOTDEF
	unsigned int		gsoSize = 1472;
#endif /* LTPGSO_NOTDEF */
#ifdef LTPRATE
	RateControlState	rc;
#endif /* LTPRATE */

	if (txbps != 0 && remoteEngineId == 0)	/*	Now nominal.	*/
	{
		remoteEngineId = txbps;
		txbps = 0;
	}

	if (remoteEngineId == 0 || endpointSpec == NULL)
	{
		PUTS("Usage: ipplso {<remote engine's host name> | @}\
[:<its port number>] <remote engine ID>");
		return 0;
	}

	if (txbps != 0)
	{
		PUTS("NOTE: ipplso now gets transmission data rate from \
the contact plan.  txbps is still accepted on the command line, for backward \
compatibility, but it is ignored.");
	}

	/*	Note that ltpadmin must be run before the first
	 *	invocation of ltplso, to initialize the LTP database
	 *	(as necessary) and dynamic database.			*/

	if (ltpInit(0) < 0)
	{
		putErrmsg("ipplso can't initialize LTP.", NULL);
		return 1;
	}

	sdr = getIonsdr();
	CHKZERO(sdr_begin_xn(sdr));	/*	Just to lock memory.	*/
	findSpan(remoteEngineId, &vspan, &vspanElt);
	if (vspanElt == 0)
	{
		sdr_exit_xn(sdr);
		putErrmsg("No such engine in database.", itoa(remoteEngineId));
		return 1;
	}

	if (vspan->lsoPid != ERROR && vspan->lsoPid != sm_TaskIdSelf())
	{
		sdr_exit_xn(sdr);
		putErrmsg("LSO task is already started for this span.",
				itoa(vspan->lsoPid));
		return 1;
	}

	sdr_exit_xn(sdr);

	/*	All command-line arguments are now validated.  First
	 *	compute the peer's socket address.			*/

	parseSocketSpec(endpointSpec, &portNbr, &ipAddress);
	if (portNbr == 0)
	{
		portNbr = LtpIppDefaultPortNbr;
	}

	if (ipAddress == 0)	/*	Default to own IP address.	*/
	{
		getNameOfHost(ownHostName, sizeof ownHostName);
		ipAddress = getInternetAddress(ownHostName);
	}

	portNbr = htons(portNbr);
	ipAddress = htonl(ipAddress);
	memset((char *) &peerSockName, 0, sizeof peerSockName);
	peerInetName = (struct sockaddr_in *) &peerSockName;
	peerInetName->sin_family = AF_INET;
	peerInetName->sin_port = portNbr;
	memcpy((char *) &(peerInetName->sin_addr.s_addr),
			(char *) &ipAddress, 4);

	/*	Now compute own socket address, used when the peer
	 *	responds to the link service output socket rather
	 *	than to the advertised link service inpud socket.	*/

	ipAddress = INADDR_ANY;
	portNbr = 0;	/*	Let O/S choose it.			*/

	/*	This socket needs to be bound to the local socket
	 *	address (just as in ipplsi), so that the ipplso
	 *	main thread can send a 1-byte datagram to that
	 *	socket to shut down the datagram handling thread.	*/

	portNbr = htons(portNbr);
	ipAddress = htonl(ipAddress);
	memset((char *) &ownSockName, 0, sizeof ownSockName);
	ownInetName = (struct sockaddr_in *) &ownSockName;
	ownInetName->sin_family = AF_INET;
	ownInetName->sin_port = portNbr;
	memcpy((char *) &(ownInetName->sin_addr.s_addr),
			(char *) &ipAddress, 4);

	/*	Now create the socket that will be used for sending
	 *	datagrams to the peer LTP engine and possibly for
	 *	receiving datagrams from the peer LTP engine.		*/

	rtp.linkSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.linkSocket < 0)
	{
		putSysErrmsg("LSO can't open UDP socket", NULL);
		return 1;
	}

	/*	Bind the socket to own socket address so that we
	 *	can send a 1-byte datagram to that address to shut
	 *	down the datagram handling thread.			*/

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(rtp.linkSocket)
	|| bind(rtp.linkSocket, &ownSockName, nameLength) < 0
	|| getsockname(rtp.linkSocket, &ownSockName, &nameLength) < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("LSO can't initialize UDP socket", NULL);
		return 1;
	}

	/* PMTUDISC should default to MTU of outgoing interface (i.e., OMNI)
	 * and leverage link adaptation w/o fragmenting payload packet. */

	if (setsockopt (rtp.linkSocket, IPPROTO_IP, IP_MTU_DISCOVER,
			&pmtudisc, sizeof(pmtudisc)) < 0)
	{
		putSysErrmsg("LSO can't set pmtudisc", NULL);
	}
#ifdef LTPGSO_NOTDEF
	/* No longer use this as a runtime test for GSO support; GSO now
	 * enabled strictly as a compile-time option and applied on a
	 * per-message basis. */

	if (setsockopt (rtp.linkSocket, SOL_UDP, UDP_SEGMENT,
			&gsoSize, sizeof(gsoSize)) < 0)
	{
		putSysErrmsg("LSO can't enable GSO", NULL);
		gsoSize = 0;
	}
#endif /* LTPGSO_NOTDEF */
#ifdef LTPPARCEL
	/*
	 * Create the socket to use for sending parcels. No bind since
	 * this socket is send-only.
	 */
	rtp.parcelSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (rtp.parcelSocket < 0)
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("LSO can't initialize Parcel socket", NULL);
		return 1;
	}

	/* PMTUDISC should default to MTU of outgoing interface (i.e., OMNI)
	 * and leverage link adaptation w/o fragmenting payload packet. */

	if (setsockopt (rtp.parcelSocket, IPPROTO_IP, IP_MTU_DISCOVER,
			&pmtudisc, sizeof(pmtudisc)) < 0)
	{
		putSysErrmsg("LSO can't set pmtudisc", NULL);
	}

	/* Set socket to include Jumbo Payload option for Parcels.
	 * Note it would be nice if this could be set or cleared on a
	 * per-packet basis instead of on the socket as a whole. */

	if (setsockopt (rtp.parcelSocket, IPPROTO_IP, IP_OPTIONS,
			(void *)&jumbo, sizeof(jumbo)) < 0)
	{
		putSysErrmsg("LSO can't set jumbos", NULL);
	}

#ifdef LTPPARCEL_CSUM_TX
	/* Set sk_no_check_tx. Means that checksums are calculated
	 * here at the application layer instead of kernel. */

	if (setsockopt (rtp.parcelSocket, SOL_SOCKET, SO_NO_CHECK,
                        &checkTx, sizeof(checkTx)) < 0)
        {
		putSysErrmsg("LSO can't set checkTx", NULL);
        }
#endif /* LTPPARCEL_CSUM_TX */
#endif /* LTPPARCEL */
	/*	Set up signal handling.  SIGTERM is shutdown signal.	*/

	oK(ipplsoSemaphore(&(vspan->segSemaphore)));
	signal(SIGTERM, shutDownLso);

	/*	Start the receiver thread.				*/

	rtp.running = 1;
#ifdef LTPSTAT
	rtp.sendSegs = 0;
	rtp.recvSegs = 0;
#endif /* LTPSTAT */
	if (pthread_begin(&receiverThread, NULL, ipplsa_handle_datagrams,
			&rtp, "ipplso_receiver"))
	{
		closesocket(rtp.linkSocket);
		putSysErrmsg("ipplso can't create receiver thread", NULL);
		return 1;
	}

	/*	Can now begin transmitting to remote engine.		*/

	{
		char	memoBuf[1024];

		isprintf(memoBuf, sizeof(memoBuf),
			"[i] ipplso is running, spec=[%s:%d], rengine=%d.",
			(char *) inet_ntoa(peerInetName->sin_addr),
			ntohs(portNbr), (int) remoteEngineId);
		writeMemo(memoBuf);
	}

	spanObj = sdr_list_data(sdr, vspan->spanElt);
	sdr_read(sdr, (char *) &spanBuf, spanObj, sizeof(LtpSpan));

	/*	For multi-send, we normally send about one LTP block
	 *	per system call.  But this can be overridden.		*/

#ifdef MULTISEND_BATCH_LIMIT
	batchLimit = MULTISEND_BATCH_LIMIT;
#else /* MULTISEND_BATCH_LIMIT */
	batchLimit = spanBuf.aggrSizeLimit / spanBuf.maxSegmentSize;
#endif /* MULTISEND_BATCH_LIMIT */
	if (batchLimit < 1)
	{
		batchLimit = 1;
	}

	if ((gsoLimit = LTPGSO_LIMIT) < 1)
	{
		gsoLimit = 1;
	}

	/* (batchLimit+1) in case we get a short segment immediately
	 * followed by a long which would cause a "double message" */

	buffers = MTAKE((IPPLSA_BUFSZ + 1) * (batchLimit + 1));
	if (buffers == NULL)
	{
		closesocket(rtp.linkSocket);
		putErrmsg("No space for segment buffer array.", NULL);
		return 1;
	}

#ifdef LTPPARCEL
	/* (gsoLimit+1) to leave space for checksums. */
	iovecs = MTAKE(sizeof(struct iovec) * (gsoLimit+1) * (batchLimit + 1));
#else /* LTPPARCEL */
	iovecs = MTAKE(sizeof(struct iovec) * gsoLimit * (batchLimit + 1));
#endif /* LTPPARCEL */
	if (iovecs == NULL)
	{
		MRELEASE(buffers);
		closesocket(rtp.linkSocket);
		putErrmsg("No space for iovec array.", NULL);
		return 1;
	}

	msgs = MTAKE(sizeof(struct mmsghdr) * (batchLimit + 1));
	if (msgs == NULL)
	{
		MRELEASE(iovecs);
		MRELEASE(buffers);
		closesocket(rtp.linkSocket);
		putErrmsg("No space for mmsghdr array.", NULL);
		return 1;
	}

	memset(msgs, 0, sizeof(struct mmsghdr) * (batchLimit + 1));
	batchLength = 0;
	buffer = buffers;
	iovec = iovecs;

	/* Allocate cmsg block, but DO NOT set up individual entries here
	 * at startup time since the values would get blitzed during runtime.
	 * Instead, initialize each individual cmsg in conjunction with the
	 * corresponding msg header at runtime so that good data is given
	 * to sendmmsg().  */

	/* Parcels differentiated from GSO by cmsg arg value. */
	cmsgs = MTAKE(CMSG_LEN(sizeof (unsigned int)) * (batchLimit + 1));
	if (cmsgs == NULL)
	{
		MRELEASE(iovecs);
		MRELEASE(buffers);
		MRELEASE(msgs);
		closesocket(rtp.linkSocket);
		putErrmsg("No space for cmsghdr array.", NULL);
		return 1;
	}

	memset(cmsgs, 0, (CMSG_LEN(sizeof (unsigned int)) * (batchLimit + 1)));
	firstSeg = msgSegs = batchSegments = 0;
	msgBytes = IPHDR_SIZE;

#ifdef LTPPARCEL
	/*
	 * Allocate checksum block. Checksums are included before actual
	 * message segments and build with each segment added to the current
	 * message. Checksum block therefore contains between 1-64 two
	 * octet checksums.
	 */
	checksums = MTAKE(128 * (batchLimit + 1));
	if (checksums == NULL)
	{
		MRELEASE(iovecs);
		MRELEASE(buffers);
		MRELEASE(msgs);
		MRELEASE(cmsgs);
		closesocket(rtp.linkSocket);
		closesocket(rtp.parcelSocket);
		putErrmsg("No space for checsksum array.", NULL);
		return 1;
	}

	memset(checksums, 0, 128 * (batchLimit + 1));
	checksum = checksums;
	cvec = 0;
#endif /* LTPPARCEL */

#ifdef LTPRATE
	rc.startTimestamp = getUsecTimestamp();
	rc.prevPaid = 0;
	rc.remoteEngineId = remoteEngineId;
	rc.neighbor = NULL;
#endif /* LTPRATE */
	while (rtp.running && !(sm_SemEnded(vspan->segSemaphore)))
	{
		if (sdr_list_length(sdr, spanBuf.segments) == 0)
		{
			/*	No segments ready to append to batch.	*/

			microsnooze(100000);	/*	Wait .1 sec.	*/
			if (sdr_list_length(sdr, spanBuf.segments) == 0)
			{
				/*	Still nothing read to add to
				 *	batch.  Send partial batch,
				 *	if any.				*/

				if (batchLength > 0)
				{
#ifdef LTPPARCEL
					if (is_parcel) 
					bytesSent = sendBatch(rtp.parcelSocket,
							msgs, batchLength);
					else
#endif /* LTPPARCEL */
					bytesSent = sendBatch(rtp.linkSocket,
							msgs, batchLength);
					if (bytesSent < 0)
					{
						putErrmsg("Failed sending \
segment batch.", NULL);
						rtp.running = 0;
						continue;
					}

#ifdef LTPRATE
					applyRateControl(&rc, bytesSent);
#endif /* LTPRATE */
#ifdef LTPPARCEL
					checksum = checksums;
					csum = 0; cvec = 0;
#endif /* LTPPARCEL */
					firstSeg = msgSegs = batchSegments = 0;
					msgBytes = IPHDR_SIZE;
					batchLength = 0;
					buffer = buffers;
					iovec = iovecs;
					/*	Let other tasks run.	*/

					sm_TaskYield();
				}
				else
				{
					snooze(1);
				}
			}

			/*	Now see if a segment is waiting.	*/

			continue;
		}

		/*	A segment is waiting to be appended to batch.	*/

		segmentLength = ltpDequeueOutboundSegment(vspan, &segment);
		if (segmentLength < 0)
		{
			rtp.running = 0;	/*	Terminate LSO.	*/
			continue;
		}

		if (segmentLength == 0)		/*	Interrupted.	*/
		{
			continue;
		}

		/*
		 * Copy this segment into current batch buffers and set iovec
		 * params. Physically copy data since multiple segments will
		 * be processed per syscall (memcpy required)
		 *
		 * TBD: determine whether LTP segments can be mapped into
		 * the message directly without data copies. This may or may
		 * not be possible in the multi-threaded ION architecture.
		 */

#ifdef LTPPARCEL
		if (is_parcel) {

			/* If this will be the first message segment, insert
			 * checksum block as first iovec.
			 */
			if ((!msgSegs) || (segmentLength > firstSeg) ||
			     (gsoLimit <= 1) ||
			     ((msgBytes + segmentLength) > IPPLSA_BUFSZ)) {
				cvec = iovec++;
				cvec->iov_base = checksum;
				cvec->iov_len = 0; /* no csum yet - see below */
				csum = (uint16_t *)checksum;
			}
#ifdef LTPPARCEL_CSUM_0
			*csum = 0;
#else /* LTPPARCEL_CSUM_0 */
#ifdef LTPPARCEL_CSUM_TX
			/* insert current segment checksum */
			*csum = in_csum(segment, segmentLength);
			*csum = *csum ? : 0xffff;
#else /* LTPPARCEL_CSUM_TX */
			/* set any non-zero val to turn on kernel checksums */
			*csum = 0x01;
#endif /* LTPPARCEL_CSUM_TX */
#endif /* LTPPARCEL_CSUM_0 */
			/* checksum inserted; count it and set up for next */
			cvec->iov_len += 2;
			msgBytes += 2;
			csum++;
		}
#endif /* LTPPARCEL */

		memcpy(buffer, segment, segmentLength);
		iovec->iov_base = buffer;
		iovec->iov_len = segmentLength;

		/* count this segment */
		batchSegments++;
#ifdef LTPSTAT
		rtp.sendSegs++;
#endif /* LTPSTAT */

		/*
		 * Advance buffer pointer. Increasing by maxSegmentSize will
		 * leave an unused "gap" between the end of a short segment
		 * and the beginning of the next segment but that is OK due
		 * to the use of iovecs.
		 */
		buffer += spanBuf.maxSegmentSize;
#ifdef LTPPARCEL
		/* Advance checksum block pointer */
		checksum += 128;
#endif /* LTPPARCEL */

		/*
		 * Determine whether to start a new mmsghdr. Linux 5.10.67
		 * returns EMSGSIZE when first messasge of a multi-message
		 * includes GSO segments totalling more than 64KB
		 *
		 * TODO: there is a bug when a short segment is followed
		 * immediately by a long segment, and MULTISEND_BATCH_LIMIT
		 * is set to some small number like 1. It will consume a
		 * message and cause sendmmsg() to be called when that is
		 * not the desired behavior. This will also cause failures
		 * when both MULTISEND_BATCH_LIMIT and LTPGSO_LIMIT are
		 * both small.
		 */
		if ((!msgSegs) || (segmentLength > firstSeg) ||
		     (gsoLimit <= 1) ||
		     ((msgBytes + segmentLength) > IPPLSA_BUFSZ))
		{

			/* record first segment length */
			firstSeg = segmentLength;

			/* init cmsg */
			cmsg = (struct cmsghdr *)((void *)cmsgs +
			  (batchLength * CMSG_LEN(sizeof(unsigned int))));
			cmsg->cmsg_len = CMSG_LEN(sizeof(unsigned int));
			cmsg->cmsg_level = SOL_UDP;
			cmsg->cmsg_type = UDP_SEGMENT;

			/* init msg */
			msg = msgs + batchLength;
			msg->msg_hdr.msg_name = (struct sockaddr *)peerInetName;
			msg->msg_hdr.msg_namelen = sizeof(struct sockaddr);
			msg->msg_hdr.msg_control = (void *)cmsg;
			msg->msg_hdr.msg_controllen = cmsg->cmsg_len;
			msg->msg_hdr.msg_iov = iovec;

			/* append current message first segment */
			msgBytes += firstSeg;
			msg->msg_hdr.msg_iovlen = msgSegs = 1;

			/* get ready for next segment */
			iovec++;

#ifdef LTPPARCEL
			if (is_parcel) {

				/* Jam in the checksum iovec and count it.
				 * Note that the iovecs are in consecutive
				 * buffers so that the first data iovec will
				 * appear immediately after this one */
				msg->msg_hdr.msg_iov = cvec;
				msg->msg_hdr.msg_iovlen++;

				/* Keep cmsg API up to date */
				*((unsigned int *)CMSG_DATA(cmsg)) =
				  (unsigned int)(((msgSegs & 0xffff) << 16) |
						 (firstSeg & 0xffff));
			}
			else /* else, regular GSO */
#endif /* LTPPARCEL */
			*((unsigned int *)CMSG_DATA(cmsg)) =
				(unsigned int)(firstSeg & 0xffff);

			/* count message */
			batchLength++;
		}
		else
		{
			/* apppend current message non-first segment */
			msgBytes += segmentLength;
			msg->msg_hdr.msg_iovlen++;
			msgSegs++;
			iovec++;
#ifdef LTPPARCEL
			/* Update nsegs for API */
			if (is_parcel) {
				*((unsigned int *)CMSG_DATA(cmsg)) =
				  (unsigned int)(((msgSegs & 0xffff) << 16) |
						 (firstSeg & 0xffff));
			}
#endif /* LTPPARCEL */

			/* Stop on short segment or when segment aggregation
			 * limit reached (max 64 segments). */
			if ((segmentLength < firstSeg) ||
			    (msgSegs == gsoLimit)) {
#ifdef LTPSTAT_NOTDEF
				char	txt[500];

#ifdef LTPPARCEL
				if (is_parcel)
				isprintf(txt, sizeof(txt),
				"[i] ipplso: GSO (1): (%d / %d / %x / %d)",
				firstSeg, segmentLength,
				*((unsigned int *)CMSG_DATA(cmsg)), batchLength);
				else
#endif /* LTPPARCEL */
				isprintf(txt, sizeof(txt),
				"[i] ipplso: GSO (1): (%d / %d / %d / %d)",
				firstSeg, segmentLength, msgSegs, batchLength);
				writeMemo(txt);
#endif /* LTPSTAT_NOTDEF */
				msgSegs = 0;
			}
		}

		if (!msgSegs && (batchLength >= batchLimit))
		{
#ifdef LTPSTAT_NOTDEF
			{
				char	txt[500];

				isprintf(txt, sizeof(txt),
				"[i] ipplso: GSO (2): (%d / %d / %d / %d)",
				firstSeg, segmentLength, msgSegs, batchLength);
				writeMemo(txt);
			}
#endif /* LTPSTAT_NOTDEF */
#ifdef LTPPARCEL
			if (is_parcel) 
				bytesSent = sendBatch(rtp.parcelSocket, msgs,
						      batchLength);
			else
#endif /* LTPPARCEL */
			bytesSent = sendBatch(rtp.linkSocket, msgs,
					batchLength);

			if (bytesSent < 0)
			{
				putErrmsg("Failed sending segment batch.",
						NULL);
				rtp.running = 0;
				continue;
			}

#ifdef LTPRATE
			applyRateControl(&rc, bytesSent);
#endif /* LTPRATE */

			firstSeg = msgSegs = 0;
			msgBytes = IPHDR_SIZE;
			batchSegments = 0;
			batchLength = 0;
			buffer = buffers;
			iovec = iovecs;
#ifdef LTPPARCEL
			checksum = checksums;
			csum = 0; cvec = 0;
#endif /* LTPPARCEL */

			/*	Let other tasks run.			*/

			sm_TaskYield();
		}
	}

	MRELEASE(msgs);
	MRELEASE(iovecs);
	MRELEASE(buffers);
	MRELEASE(cmsgs);
#ifdef LTPPARCEL
	MRELEASE(checksums);
#endif /* LTPPARCEL */
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
	writeMemo("[i] ipplso has ended.");
#ifdef LTPSTAT
	{
		char	txt[500];

		isprintf(txt, sizeof(txt),
			"[i] ipplso sent %d segments", rtp.sendSegs);
		writeMemo(txt);
	}
#endif /* LTPSTAT */
	ionDetach();
	return 0;
}
