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
#include <string.h>

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
	int             topicID = a1;

#else
int	main(int argc, char **argv)
{
	unsigned int             topicID = (argc > 1 ? atoi(argv[1]) : 0);

#endif
	DtpcSAP         sap;
	Sdr 	sdr;
	DtpcDelivery	dlv;
	int		state = 0;
	int		ret;
	struct comprPl {
                uLongf	 	uncomprSize;
                uLong		comprSize;
                const unsigned char	comprData[1000000];
        } comprPayload = {0, 0};
	unsigned char   decompr[1000000];

/*	int		contentLength;
	ZcoReader	reader;
	int		len;
	char		content[80];
	char		mail[84];
	int		msend;
	int		bline;*/

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
		putErrmsg("waiting to rx", NULL);

		if (dtpc_receive(sap, &dlv, DTPC_BLOCKING) < 0)
		{
			putErrmsg("bundlemailer reception failed.", NULL);
			continue;
		}
		/*putErrmsg("dtpc received", NULL);*/

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
			/*access dlv.item, write to comprPayload*/

			CHKZERO(sdr_begin_xn(sdr));
			putErrmsg("begin sdr xn", NULL);
			/*read dtpc adu from sdr into payload struct*/
			sdr_read(sdr, (char *) &comprPayload, dlv.item, dlv.length);
			putErrmsg("payload size from sdr_read", itoa(dlv.length));
			sdr_exit_xn(sdr);
			putErrmsg("sdr exit xn", NULL);
                        dtpc_release_delivery(&dlv);
			putErrmsg("release dlv", NULL);

			/*decompress, write to stdout and terminate*/

			/* allocate memory from ion working memory
			decompr = MTAKE(comprPayload.uncomprSize);*/

			/*get inflated size from struct element
			transmitted over network*/
			putErrmsg("get decompressed size", itoa(comprPayload.uncomprSize));
			/*get deflated size from struct element*/
			putErrmsg("get compressed size", itoa(comprPayload.comprSize));
			for (size_t i = 0; i < comprPayload.comprSize; i++) {
			printf("%02x ",comprPayload.comprData[i]);
			}

			
			/*decompress adu payload struct payload element*/
			ret = uncompress(decompr, &comprPayload.uncomprSize, comprPayload.comprData, comprPayload.comprSize);
			/*test decompression result*/
			if (ret != Z_OK)
		        {
		            zerr(ret);
		        }

			putErrmsg("uncompress return value", itoa(ret));

/* read line by line so the blank lines can be restored, 
or just sub in a newline before compression instead of -5tr1p- ?*/

/*				content[contentLength] = '\0';
				isprintf(mail, sizeof mail, "%s", content);
				bline = strcmp(mail, "-b-");

				if ((bline) == 0)
				{
					mail[0] = '\0';
				}*/
			PUTS((char *) decompr);
                        fflush(NULL);
                        PUTS("QUIT");
                        fflush(NULL);
			putErrmsg("write to stdout", NULL);
                                        dtpc_close(sap);
					putErrmsg("close sap", NULL);
                                        fflush(NULL);
                                        dtpc_detach();
					putErrmsg("dtpc detach", NULL);

                                        return 0;
                                /*}
				fflush(NULL);*/
			
		}

	}

	dtpc_release_delivery(&dlv);
	dtpc_close(sap);
	writeMemo("[i] Stopping bundlemailer.");
	ionDetach();
	return -1;
}
