/*
	imcadminep.c:	Administrative endpoint application process
			for "imc" scheme, handles IMC petitions.

	Author: Scott Burleigh, JPL

	Copyright (c) 2020, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
									*/
#include "imcfwP.h"

static int	handlePetition(BpDelivery *dlv, unsigned char *cursor,
			unsigned int unparsedBytes)
{
	uvast		uvtemp;
	ImcPetition	petition;
	MetaEid		metaEid;
	VScheme		*vscheme;
	PsmAddress	vschemeElt;
	uvast		petitioner;

	/*	Finish parsing the petition.				*/

	if (cbor_decode_integer(&uvtemp, CborAny, &cursor, &unparsedBytes) < 1)
	{
		writeMemo("[?] Can't decode IMC petition group number.");
		return 0;
	}

	petition.groupNbr = uvtemp;
	if (cbor_decode_integer(&uvtemp, CborAny, &cursor, &unparsedBytes) < 1)
	{
		writeMemo("[?] Can't decode IMC petition membership switch.");
		return 0;
	}

	petition.isMember = (uvtemp != 0);

	/*	Determine the node that sent the petition.		*/

	if (parseEidString(dlv->bundleSourceEid, &metaEid, &vscheme,
			&vschemeElt) == 0 || vscheme->codeNumber != ipn)
	{
		/*	Can't determine sending node number.		*/

		writeMemoNote("[?] Invalid sender of IMC petition",
				dlv->bundleSourceEid);
		return 0;
	}

	/*	Apply the petition to the local multicast database.	*/

	petitioner = metaEid.elementNbr;	/*	A node number.	*/
#if IMCDEBUG
writeMemoNote("Handling petition of Boolean value", itoa(petition.isMember));
writeMemoNote("...from node", itoa(petitioner));
writeMemoNote("...at node", itoa(getOwnNodeNbr()));
#endif
	if (imcUpdateGroup(petition.groupNbr, petitioner, petition.isMember)
			< 0)
	{
		putErrmsg("Failed handling petition.", NULL);
		return -1;
	}

	return 0;
}

static BpSAP	_petitionSap(BpSAP *newSap)
{
	void	*value;
	BpSAP	sap;

	if (newSap)			/*	Add task variable.	*/
	{
		value = (void *) (*newSap);
		sap = (BpSAP) sm_TaskVar(&value);
	}
	else				/*	Retrieve task variable.	*/
	{
		sap = (BpSAP) sm_TaskVar(NULL);
	}

	return sap;
}

static void	shutDownAdminApp(int signum)
{
	isignal(SIGTERM, shutDownAdminApp);
	sm_SemEnd((_petitionSap(NULL))->recvSemaphore);
}

static int	handlePetitions()
{
	Sdr		sdr = getIonsdr();
	char		receptionEid[] = "imc:0.0";
	int		running = 1;
	BpSAP		sap;
	BpDelivery	dlv;
	unsigned int	buflen;
	unsigned char	buffer[256];
	ZcoReader	reader;
	vast		bytesToParse;
	unsigned char	*cursor;
	unsigned int	unparsedBytes;
	uvast		arrayLength;

	if (bp_open(receptionEid, &sap) < 0)
	{
		putErrmsg("Can't open imcadmin endpoint 'imc:0.0'.", NULL);
		return 1;
	}

	oK(_petitionSap(&sap));
	isignal(SIGTERM, shutDownAdminApp);
	while (running && !(sm_SemEnded(sap->recvSemaphore)))
	{
		if (bp_receive(sap, &dlv, BP_BLOCKING) < 0)
		{
			putErrmsg("IMC petition reception failed.", NULL);
			running = 0;
			continue;
		}

		switch (dlv.result)
		{
		case BpPayloadPresent:
			break;

		case BpEndpointStopped:
			running = 0;

			/*	Intentional fall-through to default.	*/

		default:
			bp_release_delivery(&dlv, 1);
			continue;
		}

		/*	Process the petition.				*/

		CHKERR(sdr_begin_xn(sdr));
		buflen = zco_source_data_length(sdr, dlv.adu);
		if (buflen > sizeof buffer)
		{
			putErrmsg("Can't acquire petition.", itoa(buflen));
			oK(sdr_end_xn(sdr));
			bp_release_delivery(&dlv, 1);
			continue;
		}

		zco_start_receiving(dlv.adu, &reader);
		bytesToParse = zco_receive_source(sdr, &reader, buflen,
				(char *) buffer);
		if (bytesToParse < 0)
		{
			putErrmsg("Can't receive petition.", NULL);
			oK(sdr_end_xn(sdr));
			bp_release_delivery(&dlv, 1);
			running = 0;
			continue;
		}

		oK(sdr_end_xn(sdr));

		/*	Start parsing of petition.			*/

		cursor = buffer;
		unparsedBytes = bytesToParse;
		arrayLength = 0;
		if (cbor_decode_array_open(&arrayLength, &cursor,
				&unparsedBytes) < 1)
		{
			writeMemo("[?] Can't decode IMC petition array.");
			bp_release_delivery(&dlv, 1);
			continue;
		}

		if (arrayLength != 2)
		{
			writeMemoNote("[?] Bad IMC petition array length",
					itoa(arrayLength));
			bp_release_delivery(&dlv, 1);
			continue;
		}

		if (handlePetition(&dlv, cursor, unparsedBytes) < 0)
		{
			putErrmsg("Can't process IMC petition.", NULL);
			running = 0;
		}

		bp_release_delivery(&dlv, 1);

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	bp_close(sap);
	writeMemo("[i] Administrative endpoint terminated.");
	writeErrmsgMemos();
	return 0;
}

#if defined (ION_LWT)
int	imcadminep(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
#else
int	main(int argc, char *argv[])
{
#endif
	ImcPetition	petition = { 0, 1 };

	if (bpAttach() < 0)
	{
		putErrmsg("imcadminep can't attach to BP.", NULL);
		return 1;
	}

	if (imcInit() < 0)
	{
		putErrmsg("imcadminep can't load multicast database.", NULL);
		return 1;
	}

	writeMemo("[i] imcadminep is running.");

	/*	Join the multicast administration multicast group
	 *	(0) in order to ask for multicast group membership
	 *	briefings from all other nodes in the region.		*/

	oK(imcSendPetition(&petition, 0));

	/*	Handle petitions from other nodes.			*/

	if (handlePetitions() < 0)
	{
		putErrmsg("imcadminep crashed.", NULL);
	}

	writeErrmsgMemos();
	writeMemo("[i] imcadminep has ended.");
	ionDetach();
	return 0;
}
