/*
	bundlemailer.c:	reads dtpc bundles and sends to MTA.
	adapted from bpsink and dtpcreceive by Scott Burleigh		*/
/*									*/
/*	Copyright (c) 2025, Scott Johnson, Spacely Packets, LLC.			*/
/*									*/

#include <bp.h>
#include <dtpc.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>

static DtpcSAP  _dtpcsap(DtpcSAP *newSAP)
{
        static DtpcSAP  sap = NULL;

        if (newSAP)
        {
                sap = *newSAP;
                sm_TaskVar((void **) &sap);
        }

        return sap;
}

void zerr(int ret)
{
    switch (ret) {
    case Z_STREAM_ERROR:
        putErrmsg("invalid compression level", NULL);
        break;
    case Z_DATA_ERROR:
        putErrmsg("invalid or incomplete deflate data", NULL);
        break;
    case Z_MEM_ERROR:
        putErrmsg("out of memory", NULL);
        break;

    }
}

#if defined (ION_LWT)
int	bundlemailer(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	unsigned int             topicID = a1;

#else
int	main(int argc, char **argv)
{
	unsigned int             topicID = (argc > 1 ? atoi(argv[1]) : 0);

#endif
	DtpcSAP         sap;
	Sdr 		sdr;
	DtpcDelivery	dlv;
	int		state = 0;
	char 		textBuffer[1000000];/*dynamically allocate?*/
	/*int		comprSize;
	unsigned long 	*unComprSize = 0;
	int		ret;
	unsigned char	*decompr = 0;*/
#ifndef mingw
	setlinebuf(stdout);
#endif
	ionAttach();
	sdr = getIonsdr();

	if (dtpc_attach() < 0)
	{
		putErrmsg("Can't attach to DTPC.", NULL);
		return 0;
	}

	if (dtpc_open(topicID, NULL, &sap) < 0)
	{
		putErrmsg("Can't open own dtpc endpoint.", NULL);
		return 0;
	}

	oK(_dtpcsap(&sap));
	while (state == 0)
	{
		writeMemo("[i] Waiting to RX ADU.");

		if (dtpc_receive(sap, &dlv, DTPC_BLOCKING) < 0)
		{
			putErrmsg("bundlemailer reception failed.", NULL);
			continue;
		}

		if (dlv.result == ReceptionInterrupted)
		{
			putErrmsg("bundlemailer reception interrupted.", NULL);
			continue;
		}

		if (dlv.result == DtpcServiceStopped)
		{
			putErrmsg("dtpc service stopped.", NULL);
			continue;
		}

		if (dlv.result == PayloadPresent)
		{
			/*read dtpc adu from sdr into payload buffer*/
			CHKZERO(sdr_begin_xn(sdr));
			sdr_read(sdr, textBuffer, dlv.item, dlv.length);
			sdr_exit_xn(sdr);
			/*parse size from last 8 bytes(inflated size), then*/
                        /*memcpy(unComprSize, textBuffer + dlv.length - 8, 8);
			comprSize = dlv.length - 8;*/
			dtpc_release_delivery(&dlv);

			/*DEBUG print actual bytes in compressed adu
			for (size_t i = 0; i < unComprSize; i++) {
			printf("%02x ",[i]);
			}*/

			/*decompress parsed adu payload*/
			/*ret = uncompress((unsigned char *) decompr, unComprSize, (unsigned char *)textBuffer, comprSize);*/
			
			/*test decompression result*/
			
			/*if (ret != Z_OK)
		        {
		            zerr(ret);
		        }*/

			/*write payload to stdout*/
			/*iputs(fileno(stdout), (char *) decompr);*/
			iputs(fileno(stdout), (char *) textBuffer);
			fflush(NULL);
               		PUTS("QUIT");
               		fflush(NULL);
                        dtpc_close(sap);
                        dtpc_detach();
			return 0;
		}

	}

	dtpc_release_delivery(&dlv);
	dtpc_close(sap);
	writeMemo("[i] Stopping bundlemailer.");
	ionDetach();
	return -1;
}
