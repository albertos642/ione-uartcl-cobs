/*	libipv6.c:	ipv6 related functions		*/
/*	Author: Scott Johnson, solarnetone.org		*/
/*							*/
#include "platform.h"
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

int     parseSocketSpecSix(char *socketSpec, struct sockaddr_in6 *ip6Address)
{
	char            *delimiter;
        char            *hostname;
	

	CHKERR(ip6Address);
	if (socketSpec == NULL || *socketSpec == '\0')
        {
                return 0;               /*      Use defaults.           */
        }

	ip6Address->sin6_family = AF_INET6;
	ip6Address->sin6_addr = in6addr_any;
	ip6Address->sin6_port = htons(0);
	ip6Address->sin6_flowinfo = 0;
	
	delimiter = strchr(socketSpec, '!');	
	if (delimiter)
	{
		*delimiter ='\0';	/*      Delimit host name.      */
	}

	/* Extract IPv6 address, convert to network byte order, and store in value in struct returned to calling daemon*/

	hostname = socketSpec;
	if (strlen(hostname) != 0)
	{
		inet_pton(AF_INET6, hostname, &ip6Address->sin6_addr);
	}	

	if (delimiter == NULL)
		{
			return 0;
		}
	/* Extract port number, convert to network byte order, and store in value in struct returned to calling daemon*/

	delimiter = strchr(socketSpec, '!');	
	if (delimiter)
	{
		ip6Address->sin6_port = (atoi(delimiter + 1)); 
		if (ip6Address->sin6_port != 0) 
		{
			if (ip6Address->sin6_port < 1024 || ip6Address->sin6_port > 65535)
			{
				writeMemoNote("[?] Invalid port number.", utoa(ip6Address->sin6_port));	
				return -1;
			}
			else
			{
				ip6Address->sin6_port = htons(ip6Address->sin6_port);
			}
		}
	}
	return 0;
}

int     itcp_connect6(char *socketSpec, unsigned short defaultPort, int *sock)
{
        struct sockaddr_in6      inetName;

        CHKERR(socketSpec);
        CHKERR(sock);
        *sock = -1;             /*      Default value.                  */
        if (*socketSpec == '\0')
        {
                return 0;       /*      Don't try to connect.           */
        }

        /*      Construct socket name.                                  */

        parseSocketSpecSix(socketSpec, &inetName);

        if (inetName.sin6_port == 0)
        {
                inetName.sin6_port = htons(4556);
        }

        *sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (*sock < 0)
        {
                putSysErrmsg("Can't open TCP socket", socketSpec);
                return -1;
        }

        if (connect(*sock, (struct sockaddr *) &inetName, sizeof(inetName)) < 0)
        {
                if (errno == ECONNREFUSED)
                {
                        writeMemoNote("[i] Can't connect to TCP socket (refused)", socketSpec);
                }
                else
                {
                        putSysErrmsg("Can't connect to TCP socket", socketSpec);
                }

                closesocket(*sock);
                *sock = -1;
                return 0;
        }

        writeMemoNote("[i] Connected to TCP socket", socketSpec);
        return 1;       /*      Connected to remote socket.             */
}
