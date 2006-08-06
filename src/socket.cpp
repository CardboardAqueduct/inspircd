/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include <string>
#include "configreader.h"
#include "socket.h"
#include "inspircd.h"
#include "inspstring.h"
#include "helperfuncs.h"
#include "socketengine.h"
#include "message.h"

extern InspIRCd* ServerInstance;
extern ServerConfig* Config;
extern time_t TIME;

/* Used when comparing CIDR masks for the modulus bits left over */

char inverted_bits[8] = { 0x80, /* 10000000 - 1 bits */
			  0xC0, /* 11000000 - 2 bits */
			  0xE0, /* 11100000 - 3 bits */
			  0xF0, /* 11110000 - 4 bits */
			  0xF8, /* 11111000 - 5 bits */
			  0xFC, /* 11111100 - 6 bits */
			  0xFE, /* 11111110 - 7 bits */
			  0xFF, /* 11111111 - 8 bits */
};

bool MatchCIDRBits(unsigned char* address, unsigned char* mask, unsigned int mask_bits)
{
	unsigned int modulus = mask_bits % 8;
	unsigned int divisor = mask_bits / 8;

	if (memcmp(address, mask, divisor))
			return false;
	if (modulus)
		if ((address[divisor] & inverted_bits[modulus]) != address[divisor])
			return false;

	return true;
}

/** This will bind a socket to a port. It works for UDP/TCP.
 * It can only bind to IP addresses, if you wish to bind to hostnames
 * you should first resolve them using class 'Resolver'.
 */ 
bool BindSocket(int sockfd, insp_sockaddr client, insp_sockaddr server, int port, char* addr)
{
	memset(&server,0,sizeof(server));
	insp_inaddr addy;

	if (*addr == '*')
		*addr = 0;

	if ((*addr) && (insp_aton(addr,&addy) < 1))
	{
		log(DEBUG,"Invalid IP '%s' given to BindSocket()", addr);
		return false;;
	}

#ifdef IPV6
	server.sin6_family = AF_FAMILY;
#else
	server.sin_family = AF_FAMILY;
#endif
	if (!*addr)
	{
#ifdef IPV6
		memcpy(&addy, &server.sin6_addr, sizeof(in6_addr));
#else
		server.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
	}
	else
	{
#ifdef IPV6
		memcpy(&addy, &server.sin6_addr, sizeof(in6_addr));
#else
		server.sin_addr = addy;
#endif
	}
#ifdef IPV6
	server.sin6_port = htons(port);
#else
	server.sin_port = htons(port);
#endif
	if (bind(sockfd,(struct sockaddr*)&server,sizeof(server)) < 0)
	{
		return false;
	}
	else
	{
		log(DEBUG,"Bound port %s:%d",*addr ? addr : "*",port);
		if (listen(sockfd, Config->MaxConn) == -1)
		{
			log(DEFAULT,"ERROR in listen(): %s",strerror(errno));
			return false;
		}
		else
		{
			NonBlocking(sockfd);
			return true;
		}
	}
}


// Open a TCP Socket
int OpenTCPSocket()
{
	int sockfd;
	int on = 1;
	struct linger linger = { 0 };
  
	if ((sockfd = socket (AF_FAMILY, SOCK_STREAM, 0)) < 0)
	{
		log(DEFAULT,"Error creating TCP socket: %s",strerror(errno));
		return (ERROR);
	}
	else
	{
		setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		/* This is BSD compatible, setting l_onoff to 0 is *NOT* http://web.irc.org/mla/ircd-dev/msg02259.html */
		linger.l_onoff = 1;
		linger.l_linger = 1;
		setsockopt(sockfd, SOL_SOCKET, SO_LINGER, &linger,sizeof(linger));
		return (sockfd);
	}
}

bool HasPort(int port, char* addr)
{
	for (unsigned long count = 0; count < ServerInstance->stats->BoundPortCount; count++)
	{
		if ((port == Config->ports[count]) && (!strcasecmp(Config->addrs[count],addr)))
		{
			return true;
		}
	}
	return false;
}

int BindPorts(bool bail)
{
	char configToken[MAXBUF], Addr[MAXBUF], Type[MAXBUF];
	insp_sockaddr client, server;
	int clientportcount = 0;
	int BoundPortCount = 0;

	if (!bail)
	{
		int InitialPortCount = ServerInstance->stats->BoundPortCount;
		log(DEBUG,"Initial port count: %d",InitialPortCount);

		for (int count = 0; count < Config->ConfValueEnum(Config->config_data, "bind"); count++)
		{
			Config->ConfValue(Config->config_data, "bind", "port", count, configToken, MAXBUF);
			Config->ConfValue(Config->config_data, "bind", "address", count, Addr, MAXBUF);
			Config->ConfValue(Config->config_data, "bind", "type", count, Type, MAXBUF);

			if (((!*Type) || (!strcmp(Type,"clients"))) && (!HasPort(atoi(configToken),Addr)))
			{
				// modules handle server bind types now
				Config->ports[clientportcount+InitialPortCount] = atoi(configToken);
				if (*Addr == '*')
					*Addr = 0;

				strlcpy(Config->addrs[clientportcount+InitialPortCount],Addr,256);
				clientportcount++;
				log(DEBUG,"NEW binding %s:%s [%s] from config",Addr,configToken, Type);
			}
		}
		int PortCount = clientportcount;
		if (PortCount)
		{
			for (int count = InitialPortCount; count < InitialPortCount + PortCount; count++)
			{
				if ((Config->openSockfd[count] = OpenTCPSocket()) == ERROR)
				{
					log(DEBUG,"Bad fd %d binding port [%s:%d]",Config->openSockfd[count],Config->addrs[count],Config->ports[count]);
					return ERROR;
				}
				if (!BindSocket(Config->openSockfd[count],client,server,Config->ports[count],Config->addrs[count]))
				{
					log(DEFAULT,"Failed to bind port [%s:%d]: %s",Config->addrs[count],Config->ports[count],strerror(errno));
				}
				else
				{
					/* Associate the new open port with a slot in the socket engine */
					if (Config->openSockfd[count] > -1)
					{
						ServerInstance->SE->AddFd(Config->openSockfd[count],true,X_LISTEN);
						BoundPortCount++;
					}
				}
			}
			return InitialPortCount + BoundPortCount;
		}
		else
		{
			log(DEBUG,"There is nothing new to bind!");
		}
		return InitialPortCount;
	}

	for (int count = 0; count < Config->ConfValueEnum(Config->config_data, "bind"); count++)
	{
		Config->ConfValue(Config->config_data, "bind", "port", count, configToken, MAXBUF);
		Config->ConfValue(Config->config_data, "bind", "address", count, Addr, MAXBUF);
		Config->ConfValue(Config->config_data, "bind", "type", count, Type, MAXBUF);

		if ((!*Type) || (!strcmp(Type,"clients")))
		{
			// modules handle server bind types now
			Config->ports[clientportcount] = atoi(configToken);

			// If the client put bind "*", this is an unrealism.
			// We don't actually support this as documented, but
			// i got fed up of people trying it, so now it converts
			// it to an empty string meaning the same 'bind to all'.
			if (*Addr == '*')
				*Addr = 0;

			strlcpy(Config->addrs[clientportcount],Addr,256);
			clientportcount++;
			log(DEBUG,"Binding %s:%s [%s] from config",Addr,configToken, Type);
		}
	}

	int PortCount = clientportcount;

	for (int count = 0; count < PortCount; count++)
	{
		if ((Config->openSockfd[BoundPortCount] = OpenTCPSocket()) == ERROR)
		{
			log(DEBUG,"Bad fd %d binding port [%s:%d]",Config->openSockfd[BoundPortCount],Config->addrs[count],Config->ports[count]);
			return ERROR;
		}

		if (!BindSocket(Config->openSockfd[BoundPortCount],client,server,Config->ports[count],Config->addrs[count]))
		{
			log(DEFAULT,"Failed to bind port [%s:%d]: %s",Config->addrs[count],Config->ports[count],strerror(errno));
		}
		else
		{
			/* well we at least bound to one socket so we'll continue */
			BoundPortCount++;
		}
	}

	/* if we didn't bind to anything then abort */
	if (!BoundPortCount)
	{
		log(DEFAULT,"No ports bound, bailing!");
		printf("\nERROR: Could not bind any of %d ports! Please check your configuration.\n\n", PortCount);
		return ERROR;
	}

	return BoundPortCount;
}

const char* insp_ntoa(insp_inaddr n)
{
	static char buf[1024];
	inet_ntop(AF_FAMILY, &n, buf, sizeof(buf));
	return buf;
}

int insp_aton(const char* a, insp_inaddr* n)
{
	return inet_pton(AF_FAMILY, a, n);
}

