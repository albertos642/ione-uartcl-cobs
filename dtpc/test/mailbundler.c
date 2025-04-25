/*      mailbundler.c: encapsulate batch mode emails into compressed
	bundles with dtpc reliability and elision features.

	Derived from bpsource.c by Scott Burleigh, with addition
	of dtpc reliability and zlib compression.
	Copyright (c) 2025 Scott Johnson, Spacely Packets, LLC.
	Released under the GNU GPLv3 License.
								*/

#include <bp.h>
#include <dtpc.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>

#define	DEFAULT_TTL 300

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


static int      checkElision(Object recordsList)
{
        Sdr             sdr = getIonsdr();
        Object          elt;
        Object          nextElt;
        Object          obj;
        PayloadRecord   item;
        uvast           firstLength;
        uvast           length;

        CHKZERO(sdr_begin_xn(sdr));
        firstLength = 0;
        for (elt = sdr_list_first(sdr, recordsList); elt; elt = nextElt)
        {
                nextElt = sdr_list_next(sdr, elt);
                obj = sdr_list_data(sdr, elt);
                sdr_read(sdr, (char *) &item, obj, sizeof(PayloadRecord));
                oK(decodeSdnv(&length, item.length.text));
                if (firstLength == 0)
                {
                        firstLength = length;
                        continue;
                }

                if (length == firstLength)      /*      Duplicate.      */
                {
                        sdr_list_delete(sdr, elt, NULL, NULL);
                        sdr_free(sdr, item.payload);
                        sdr_free(sdr, obj);
                }
        }

        return sdr_end_xn(sdr);
}

static int	_running(int *newState)
{
	int	state = 1;

	if (newState)
	{
		state = *newState;
	}

	return state;
}

/*static void	handleQuit(int signum)
{
	int	stop = 0;


	oK(_running(&stop));
}*/

void zerr(int ret)
{
    switch (ret) {
    case Z_STREAM_ERROR:
        putErrmsg("invalid compression level\n", NULL);
        break;
    case Z_DATA_ERROR:
        putErrmsg("invalid or incomplete deflate data\n", NULL);
        break;
    case Z_MEM_ERROR:
        putErrmsg("out of memory\n", NULL);
        break;

    }
}


#if defined (ION_LWT)
int	mailbundler(saddr a1, saddr a2, saddr a3, saddr a4, saddr a5,
		saddr a6, saddr a7, saddr a8, saddr a9, saddr a10)
{
	char		*destEid = (char *) a1;

#else
int	main(int argc, char **argv)
{
	char		*destEid = NULL;

	switch (argc)
	{
	case 1:
		destEid = argv[1];
	case 2:
		destEid = argv[1];
	default:
		break;
	}
#endif
	unsigned int	topicID = 26;
	/*int		ret = 12;*/ 
	int		stop;
	Sdr		sdr;
	/*unsigned char	*compr = 0;
	uLong		comprLen = 0;*/
	uLong 		textBufferLength = 0;
	int		textLength = 0;
	Object		extent;
	Address		addr;
	DtpcSAP         sap;
	DtpcElisionFn   elisionFn;
	unsigned int 	profileID = 25;
	int 		bufferLength = 0;
	int		fd;

	elisionFn = checkElision;

	if (destEid == NULL)
	{
		PUTS("Usage: mailbundler <destination endpoint ID> [\"<text>\"]");
		return 0;
	}

	if (dtpc_attach() < 0)
	{
		putErrmsg("Can't attach to DTPC.", NULL);
		return 0;
	}

	if (dtpc_open(topicID, elisionFn, &sap) < 0)
        {
                putErrmsg("Can't open mail topic.", itoa(topicID));
                return 0;
        }

	oK(_dtpcsap(&sap));
	sdr = getIonsdr();
	/*isignal(SIGINT, handleQuit);*/
	fd = fileno(stdin);
	char    	*text = MTAKE(4096);
	char		*textBuffer = MTAKE(1000000);
	/*loop to collect email from stdin line by line into buffer*/
	while (igets(fd, text, 4096, &textLength) != NULL)
	{
		/*"."denotes end of smtp message*/
		if ((strcmp (text, ".")) == 0)
		{
	                memcpy(textBuffer + bufferLength, text, textLength);
        	        textBuffer[bufferLength + textLength] = '\n';
                	bufferLength += textLength + 1;
			MRELEASE(text);
			close(fileno(stdin));
			break;
		}

		/*nono buffer overflow!*/
		if (bufferLength + textLength + 1 > 1000000)
		{
			MRELEASE(text);
			MRELEASE(textBuffer);
			break;
		}
		
		memcpy(textBuffer + bufferLength, text, textLength);
		textBuffer[bufferLength + textLength] = '\n';
		bufferLength += textLength + 1;

	}


	if (bufferLength == 0)
	{
		putErrmsg("[?] No mail for mailbundler to send.", NULL);
		dtpc_detach();
		dtpc_close(sap);
		ionDetach();
		return 0;
	}
	/*	compression  */

	textBufferLength = bufferLength;
	/*determine upper bound of compressed size*/
	/*comprLen = compressBound(textBufferLength);*/
	/*take enough for unsigned long too*/
	/*compr = MTAKE(comprLen + 8);*/
	/*ret = compress2(compr, &comprLen, (const unsigned char *) textBuffer, textBufferLength, 6);

        if (ret != Z_OK)
	{
       	    zerr(ret);
	}*/
	/*	 end of compression  */
	/*	 append uncompressed length to compressed data  */
	/*memcpy(compr + comprLen, (void *) textBufferLength, 8);*/
	/*	write to sdr*/
	CHKZERO(sdr_begin_xn(sdr));
	/*extent = sdr_malloc(sdr, comprLen + 8);*/
	extent = sdr_malloc(sdr, bufferLength);
	addr = extent;
	if (extent)
	{
		/*sdr_write(sdr, addr, (char *) compr, comprLen + 8);*/
		sdr_write(sdr, addr, textBuffer, textBufferLength);
	}
	MRELEASE(textBuffer);

	if (sdr_end_xn(sdr) < 0)
	{
		putErrmsg("No space for mail ADU.", NULL);
		dtpc_close(sap);
		dtpc_detach();
		ionDetach();
		return 0;
	}

	switch (dtpc_send(profileID, sap, destEid, 0, 0, 0, 0, NULL, 0, 0, NULL, 0, addr, textBufferLength))
	{
	case -1:
                putErrmsg("Can't send adu.", NULL);
                oK(_running(&stop));

        case 0:         /* This payload does not fit in an adu  */
 		if (sdr_begin_xn(sdr) == 0)
        	{
                	putErrmsg("Can't discard payload.", NULL);
                        oK(_running(&stop));
                }

                sdr_free(sdr, extent);

		if (sdr_end_xn(sdr) < 0)
                {
                        putErrmsg("Can't discard payload.", NULL);
                	oK(_running(&stop));
                }

                break;
	case 1:

		MRELEASE(textBuffer);
		/*MRELEASE(compr);*/
		/*CHKZERO(sdr_begin_xn(sdr));
		sdr_free(sdr, extent);
		sdr_end_xn(sdr);*/
	default:
		break;
	}
	/*CHKZERO(sdr_begin_xn(sdr));
	sdr_free(sdr, extent);
	sdr_end_xn(sdr);*/
	dtpc_close(sap);
	dtpc_detach();
	ionDetach();
	return 0;

}
