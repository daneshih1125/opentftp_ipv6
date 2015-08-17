/**************************************************************************
*   Copyright (C) 2005 by Achal Dhir                                      *
*   achaldhir@gmail.com                                                   *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
***************************************************************************/

// tftpserver.cpp
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <memory.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include "opentftpd.h"

//Global Variables
bool kRunning = true;
bool verbatim = false;
char iniFile[256] = "";
char logFile[256] = "";
char fileSep = '/';
char notFileSep = '\\';
MYDWORD blksize = 65464;
MYWORD timeout = 3;
data1 network;
data1 newNetwork;
data2 cfig;
struct ifconf Ifc;
struct ifreq IfcBuf[MAX_SERVERS];
char logBuff[256];
char tempbuff[256];
char extbuff[256];
MYWORD loggingDay;
char sVersion[] = "TFTP Server MultiThreaded Version 1.65 Unix Built 2002";

//Thread Global Variables
pthread_t threadId;
MYWORD minThreads = 1;
MYWORD totalThreads = 0;
MYWORD activeThreads = 0;
int currentServer = UCHAR_MAX;
pthread_mutex_t mutThread = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutSocket = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutCount = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutLog = PTHREAD_MUTEX_INITIALIZER;
//pthread_cond_t cond1 = PTHREAD_COND_INITIALIZER;

int main(int argc, char **argv)
{
    signal(SIGINT, catch_int);
    signal(SIGABRT, catch_int);
    signal(SIGTERM, catch_int);
    signal(SIGQUIT, catch_int);
    signal(SIGTSTP, catch_int);
    signal(SIGHUP, catch_int);

    logBuff[0] = 0;

    for (int i = 1; i < argc; i++)
    {
        if (!strcasecmp(argv[i], "-v"))
            verbatim = true;
        else if (!strcmp(argv[i], "-i") && argc > i + 1 && argv[i + 1][0] != '-' )
        {
            myTrim(iniFile, argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "-l") && argc > i + 1 && argv[i + 1][0] != '-' )
        {
            myTrim(logFile, argv[i + 1]);
            i++;
        }
        else if (!strncasecmp(argv[i], "-i", 2))
            myTrim(iniFile, argv[i] + 2);
        else if (!strncasecmp(argv[i], "-l", 2))
            myTrim(logFile, argv[i] + 2);
        else
        {
            sprintf(logBuff, "Error: Invalid Argument %s", argv[i]);
            break;
		}
    }

	//printf("%s\n", logBuff);

	if (!iniFile[0])
		strcpy(iniFile,"/etc/opentftpd.ini");

	if (verbatim)
	{
		if (logBuff[0])
		{
			printf("%s\n", logBuff);
			exit(EXIT_FAILURE);
		}

		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
		int errcode = pthread_create(&threadId, &attr, init, NULL);
		pthread_attr_destroy(&attr);

		if(errcode)
		{
			sprintf(logBuff, "Error: Main Thread Creation Failed with error %s", strerror(errcode));
			logMess(logBuff, 1);
			exit(EXIT_FAILURE);
		}

		while (kRunning && !network.ready)
			sleep(1);

		setuid(cfig.pw_uid);
		setgid(cfig.pw_gid);

		//printf("user set to %s\n", cfig.username);

		timeval tv;
		fd_set readfds;

		while (kRunning)
		{
			network.busy = false;

			if (!network.ready)
			{
				sleep(1);
				continue;
			}

			FD_ZERO(&readfds);
			tv.tv_sec = 20;
			tv.tv_usec = 0;

			//printf("total=%d active=%d\n", totalThreads, activeThreads);

			for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].ready; i++)
				FD_SET(network.tftpConn[i].sock, &readfds);

			pthread_mutex_lock( &mutSocket );
			int fdsReady = select(network.maxFD, &readfds, NULL, NULL, &tv);
			pthread_mutex_unlock( &mutSocket );

			printf("ready=%i\n", fdsReady);

			//if (errno)
			//	printf("%s\n", strerror(errno));

			for (int i = 0; fdsReady > 0 && i < MAX_SERVERS && network.tftpConn[i].ready; i++)
			{
				if (network.ready)
				{
					network.busy = true;

					if (FD_ISSET(network.tftpConn[i].sock, &readfds))
					{
						pthread_mutex_lock( &mutSocket );
						//printf("Locked\n");
						currentServer = i;

						//printf("Total Threads %d Active Threads %d\n", totalThreads, activeThreads);

						if (!totalThreads || activeThreads >= totalThreads)
						{
							//Thanks erez for suggenting this
							pthread_attr_t attr;
							pthread_attr_init(&attr);
							pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
							int errcode = pthread_create(&threadId, &attr, processRequest, NULL);
							pthread_attr_destroy(&attr);
							//if (pthread_create(&threadId, 0, processRequest, NULL))
							if(errcode)
							{
								printf("Thread Creation Error %i\n", errcode);
								pthread_mutex_unlock( &mutSocket );
								continue;
							}
						}
						pthread_mutex_unlock( &mutThread );

						pthread_mutex_lock( &mutSocket );
						fdsReady--;
						pthread_mutex_unlock( &mutSocket );
					}
				}
			}
		}
	}
	else
	{
		if(logBuff[0])
		{
			syslog(LOG_MAKEPRI(LOG_LOCAL1, LOG_CRIT), logBuff);
			exit(EXIT_FAILURE);
		}

		/* Our process ID and Session ID */
		pid_t pid, sid;

		/* Fork off the parent process */
		pid = fork();
		if (pid < 0)
		{
			exit(EXIT_FAILURE);
		}

		/* If we got a good PID, then
		we can exit the parent process. */
		if (pid > 0)
		{
			exit(EXIT_SUCCESS);
		}

		/* Change the file mode mask */
		umask(0);

		/* Open any logs here */

		/* Create a new SID for the child process */
		sid = setsid();
		if (sid < 0)
		{
			/* Log the failure */
			exit(EXIT_FAILURE);
		}

		/* Change the current working directory */
		if ((chdir("/")) < 0)
		{
			/* Log the failure */
			exit(EXIT_FAILURE);
		}

		/* Close out the standard file descriptors */
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);

		/* Daemon-specific initialization goes here */
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
		int errcode = pthread_create(&threadId, &attr, init, NULL);
		pthread_attr_destroy(&attr);

		if(errcode)
		{
			sprintf(logBuff, "Error: Main Thread Creation Failed with error %s", strerror(errcode));
			logMess(logBuff, 1);
			exit(EXIT_FAILURE);
		}

		while (kRunning && !network.ready)
			sleep(1);

		setuid(cfig.pw_uid);
		setgid(cfig.pw_gid);

		timeval tv;
		fd_set readfds;

		while (kRunning)
		{
			network.busy = false;

			if (!network.tftpConn[0].ready || !network.ready)
			{
				sleep(2);
				continue;
			}

			FD_ZERO(&readfds);
			tv.tv_sec = 20;
			tv.tv_usec = 0;

			//printf("total=%d active=%d\n", totalThreads, activeThreads);

			for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].ready; i++)
				FD_SET(network.tftpConn[i].sock, &readfds);

			pthread_mutex_lock( &mutSocket );
			int fdsReady = select(network.maxFD, &readfds, NULL, NULL, &tv);
			pthread_mutex_unlock( &mutSocket );
			//printf("ready=%i\n", fdsReady);

			//if (errno)
			//	printf("%s\n", strerror(errno));

			for (int i = 0; fdsReady > 0 && i < MAX_SERVERS && network.tftpConn[i].ready; i++)
			{
				if (network.ready)
				{
					network.busy = true;

					if (FD_ISSET(network.tftpConn[i].sock, &readfds))
					{
						pthread_mutex_lock( &mutSocket );
						//printf("Locked\n");
						currentServer = i;

						if (!totalThreads || activeThreads >= totalThreads)
						{
							//Thanks erez for suggenting this
							pthread_attr_t attr;
							pthread_attr_init(&attr);
							pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
							int errcode = pthread_create(&threadId, &attr, processRequest, NULL);
							pthread_attr_destroy(&attr);
							//if (pthread_create(&threadId, 0, processRequest, NULL))
							if(errcode)
							{
								sprintf(logBuff, "Thread Creation Error %i", errcode);
								logMess(logBuff, 1);
								pthread_mutex_unlock( &mutSocket );
								continue;
							}
						}
						pthread_mutex_unlock( &mutThread );

						pthread_mutex_lock( &mutSocket );
						fdsReady--;
						pthread_mutex_unlock( &mutSocket );
					}
				}
			}
		}
	}

	sprintf(logBuff, "Closing Network Connections...");
	logMess(logBuff, 1);
	closeConn();
	close(cfig.fixedSocket);
	sprintf(logBuff, "TFTP Server Stopped !");
	logMess(logBuff, 1);

	if (cfig.logfile)
		fclose(cfig.logfile);

	exit(EXIT_SUCCESS);
}

void closeConn()
{
	for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].loaded; i++)
	{
		if (network.tftpConn[i].ready)
		{
			close(network.tftpConn[i].sock);
			//printf("%d, %s clsoed\n", i, IP2String(tempbuff, network.tftpConn[i].server));
		}
	}
}

void catch_int(int sig_num)
{
	if (cfig.pppid == getpid())
	{
		kRunning = false;
	}
}

void *processRequest(void *lpParam)
{
	//printf("New Thread %u\n", GetCurrentThreadId());

	request req;

	pthread_mutex_lock( &mutCount );
	totalThreads++;
	pthread_mutex_unlock( &mutCount );

	do
	{
		pthread_mutex_lock( &mutThread );

		//printf("In Thread %d, Socket=%u\n", getpid(), currentServer);

		pthread_mutex_lock( &mutCount );
		activeThreads++;
		pthread_mutex_unlock( &mutCount );

		if (currentServer >= MAX_SERVERS || !network.tftpConn[currentServer].port)
		{
			pthread_mutex_unlock( &mutSocket );
			req.attempt = UCHAR_MAX;
			continue;
		}

		memset(&req, 0, sizeof(request));
		req.sock = -1;

		req.clientsize = sizeof(req.client);
		req.sockInd = currentServer;
		currentServer = UCHAR_MAX;
		req.knock = network.tftpConn[req.sockInd].sock;
		bool isIPv6 = network.listenIPv6[req.sockInd];

		if (req.knock < 0)
		{
			pthread_mutex_unlock( &mutSocket );
			req.attempt = UCHAR_MAX;
			continue;
		}

		errno = 0;
		req.bytesRecd = recvfrom(req.knock, (char*)&req.mesin, sizeof(message), 0, (sockaddr*)&req.client, &req.clientsize);
		////errno = WSAGetLastError();

		//printf("socket Signalled=%u\n",SetEvent(sEvent));
		pthread_mutex_unlock( &mutSocket );

		if (!errno && req.bytesRecd > 0)
		{
			if (cfig.hostRanges[0].rangeStart)
			{
				MYDWORD iip = ntohl(req.client.v4.sin_addr.s_addr);
				bool allowed = false;

				for (int j = 0; j <= 32 && cfig.hostRanges[j].rangeStart; j++)
				{
					if (iip >= cfig.hostRanges[j].rangeStart && iip <= cfig.hostRanges[j].rangeEnd)
					{
						allowed = true;
						break;
					}
				}

				if (!allowed)
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					strcpy(req.serverError.errormessage, "Access Denied");
					logMess(&req, 1);
					sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v6, req.clientsize);
					req.attempt = UCHAR_MAX;
					continue;
				}
			}

			if ((htons(req.mesin.opcode) == 5))
			{
				sprintf(req.serverError.errormessage, "Error Code %i at Client, %s", ntohs(req.clientError.errorcode), req.clientError.errormessage);
				logMess(&req, 2);
				req.attempt = UCHAR_MAX;
				continue;
			}
			else if (htons(req.mesin.opcode) != 1 && htons(req.mesin.opcode) != 2)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(5);
				sprintf(req.serverError.errormessage, "Unknown Transfer Id");
				logMess(&req, 2);
				sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v6, req.clientsize);
				req.attempt = UCHAR_MAX;
				continue;
			}
		}
		else
		{
			sprintf(req.serverError.errormessage, "Communication Error");
			logMess(&req, 1);
			req.attempt = UCHAR_MAX;
			continue;
		}

		req.blksize = 512;
		req.timeout = timeout;
		req.expiry = time(NULL) + req.timeout;
		bool fetchAck = false;

		//req.sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (isIPv6)
			req.sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		else
			req.sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

		if (req.sock == INVALID_SOCKET)
		{
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			strcpy(req.serverError.errormessage, "Thread Socket Creation Error");
			sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v6, req.clientsize);
			logMess(&req, 1);
			req.attempt = UCHAR_MAX;
			continue;
		}

		//IPv6 mark
		sockaddr_in service;
		service.sin_family = AF_INET;
		service.sin_addr.s_addr = network.tftpConn[req.sockInd].server;
		struct sockaddr_in6 service6;
		service6.sin6_family = AF_INET6;
		if (isIPv6)
                	inet_pton(AF_INET6, network.listenAddress[req.sockInd], (void *)&service6.sin6_addr.s6_addr);
                	//service6.sin6_addr = in6addr_any;
		//service.sin6_addr.s6_addr = network.tftpConn[req.sockInd].addr.v6.sin6_addr.s6_addr;
		

		if (cfig.minport)
		{
			for (MYWORD comport = cfig.minport; ; comport++)
			{
				service.sin_port = htons(comport);

				if (comport > cfig.maxport)
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);
					strcpy(req.serverError.errormessage, "No port is free");
					sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client, req.clientsize);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
					break;
				}
				else if (bind(req.sock, (sockaddr*) &service, sizeof(service)) == -1)
					continue;
				else
					break;
				;
			}
		}
		else
		{
			if (isIPv6)
			{
				service6.sin6_port = 0;

				if (bind(req.sock, (sockaddr*) &service6, sizeof(service6)) == -1)
				{
					strcpy(req.serverError.errormessage, "Thread failed to bind");
					sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v6, req.clientsize);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
				}
			}
			else
			{
				service.sin_port = 0;

				if (bind(req.sock, (sockaddr*) &service, sizeof(service)) == -1)
				{
					strcpy(req.serverError.errormessage, "Thread failed to bind");
					sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v4, req.clientsize);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
				}
			}
		}

		if (req.attempt >= 3)
			continue;

		if (connect(req.sock, (sockaddr*)&req.client.v6, req.clientsize) == -1)
		{
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			strcpy(req.serverError.errormessage, "Connect Failed");
			sendto(req.knock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0, (sockaddr*)&req.client.v6, req.clientsize);
			logMess(&req, 1);
			req.attempt = UCHAR_MAX;
			continue;
		}

		//sprintf(req.serverError.errormessage, "In Temp, Socket");
		//logMess(&req, 1);

		char *inPtr = req.mesin.buffer;
		*(inPtr + (req.bytesRecd - 3)) = 0;
		req.filename = inPtr;

		if (!strlen(req.filename) || strlen(req.filename) > UCHAR_MAX)
		{
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			strcpy(req.serverError.errormessage, "Malformed Request, Invalid/Missing Filename");
			send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
			req.attempt = UCHAR_MAX;
			logMess(&req, 1);
			continue;
		}

		inPtr += strlen(inPtr) + 1;
		req.mode = inPtr;

		if (!strlen(req.mode) || strlen(req.mode) > 25)
		{
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(0);
			strcpy(req.serverError.errormessage, "Malformed Request, Invalid/Missing Mode");
			send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
			req.attempt = UCHAR_MAX;
			logMess(&req, 1);
			continue;
		}

		inPtr += strlen(inPtr) + 1;

		for (MYDWORD i = 0; i < strlen(req.filename); i++)
			if (req.filename[i] == notFileSep)
				req.filename[i] = fileSep;

		tempbuff[0] = '.';
		tempbuff[1] = '.';
		tempbuff[2] = fileSep;
		tempbuff[3] = 0;

		if (strstr(req.filename, tempbuff))
		{
			req.serverError.opcode = htons(5);
			req.serverError.errorcode = htons(2);
			strcpy(req.serverError.errormessage, "Access violation");
			send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
			logMess(&req, 1);
			req.attempt = UCHAR_MAX;
			continue;
		}

		if (req.filename[0] == fileSep)
			req.filename++;

		if (!cfig.homes[0].alias[0])
		{
			if (strlen(cfig.homes[0].target) + strlen(req.filename) >= sizeof(req.path))
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				sprintf(req.serverError.errormessage, "Filename too large");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			strcpy(req.path, cfig.homes[0].target);
			strcat(req.path, req.filename);
		}
		else
		{
			char *bname = strchr(req.filename, fileSep);

			if (bname)
			{
				*bname = 0;
				bname++;
			}
			else
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				sprintf(req.serverError.errormessage, "Missing directory/alias");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			for (int i = 0; i < 8; i++)
			{
				//printf("%s=%i\n", req.filename, cfig.homes[i].alias[0]);
				if (cfig.homes[i].alias[0] && !strcasecmp(req.filename, cfig.homes[i].alias))
				{
					if (strlen(cfig.homes[i].target) + strlen(bname) >= sizeof(req.path))
					{
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(2);
						sprintf(req.serverError.errormessage, "Filename too large");
						send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}

					strcpy(req.path, cfig.homes[i].target);
					strcat(req.path, bname);
					break;
				}
				else if (i == 7 || !cfig.homes[i].alias[0])
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					sprintf(req.serverError.errormessage, "No such directory/alias %s", req.filename);
					send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
					break;
				}
			}
		}

		if (req.attempt >= 3)
			continue;

		if (ntohs(req.mesin.opcode) == 1)
		{
			if (!cfig.fileRead)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "GET Access Denied");
				logMess(&req, 1);
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}

			if (*inPtr)
			{
				char *tmp = inPtr;

				while (*tmp)
				{
					if (!strcasecmp(tmp, "blksize"))
					{
						tmp += strlen(tmp) + 1;
						MYDWORD val = atol(tmp);

						if (val < 512)
							val = 512;
						else if (val > blksize)
							val = blksize;

						req.blksize = val;
						break;
					}

					tmp += strlen(tmp) + 1;
				}
			}

			errno = 0;

			if (!strcasecmp(req.mode, "netascii") || !strcasecmp(req.mode, "ascii"))
				req.file = fopen(req.path, "rt");
			else
				req.file = fopen(req.path, "rb");

			if (errno || !req.file)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(1);
				strcpy(req.serverError.errormessage, "File not found or No Access");
				logMess(&req, 1);
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				req.attempt = UCHAR_MAX;
				continue;
			}
		}
		else
		{
			if (!cfig.fileWrite && !cfig.fileOverwrite)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "PUT Access Denied");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			req.file = fopen(req.path, "rb");

			if (req.file)
			{
				fclose(req.file);
				req.file = NULL;

				if (!cfig.fileOverwrite)
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(6);
					strcpy(req.serverError.errormessage, "File already exists");
					send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
					continue;
				}
			}
			else if (!cfig.fileWrite)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "Create File Access Denied");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			errno = 0;

			if (!strcasecmp(req.mode, "netascii") || !strcasecmp(req.mode, "ascii"))
				req.file = fopen(req.path, "wt");
			else
				req.file = fopen(req.path, "wb");

			if (errno || !req.file)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}
		}

		setvbuf(req.file, NULL, _IOFBF, 5 * req.blksize);

		if (*inPtr)
		{
			fetchAck = true;
			char *outPtr = req.mesout.buffer;
			req.mesout.opcode = htons(6);
			MYDWORD val;
			while (*inPtr)
			{
				//printf("%s\n", inPtr);
				if (!strcasecmp(inPtr, "blksize"))
				{
					strcpy(outPtr, inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;
					val = atol(inPtr);

					if (val < 512)
						val = 512;
					else if (val > blksize)
						val = blksize;

					req.blksize = val;
					sprintf(outPtr, "%u", val);
					outPtr += strlen(outPtr) + 1;
				}
				else if (!strcasecmp(inPtr, "tsize"))
				{
					strcpy(outPtr, inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;

					if (ntohs(req.mesin.opcode) == 1)
					{
						if (!fseek(req.file, 0, SEEK_END))
						{
							if (ftell(req.file) >= 0)
							{
								req.tsize = ftell(req.file);
								sprintf(outPtr, "%u", req.tsize);
								outPtr += strlen(outPtr) + 1;
							}
							else
							{
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(2);
								strcpy(req.serverError.errormessage, "Invalid Path or No Access");
								send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
								logMess(&req, 1);
								req.attempt = UCHAR_MAX;
								break;
							}
						}
						else
						{
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(2);
							strcpy(req.serverError.errormessage, "Invalid Path or No Access");
							send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
							logMess(&req, 1);
							req.attempt = UCHAR_MAX;
							break;
						}
					}
					else
					{
						req.tsize = 0;
						sprintf(outPtr, "%u", req.tsize);
						outPtr += strlen(outPtr) + 1;
					}
				}
				else if (!strcasecmp(inPtr, "timeout"))
				{
					strcpy(outPtr, inPtr);
					outPtr += strlen(outPtr) + 1;
					inPtr += strlen(inPtr) + 1;
					val = atoi(inPtr);

					if (val < 1)
						val = 1;
					else if (val > UCHAR_MAX)
						val = UCHAR_MAX;

					req.timeout = val;
					req.expiry = time(NULL) + req.timeout;
					sprintf(outPtr, "%u", val);
					outPtr += strlen(outPtr) + 1;
				}

				inPtr += strlen(inPtr) + 1;
				//printf("=%u\n", val);
			}

			if (req.attempt >= 3)
				continue;

			errno = 0;
			req.bytesReady = (MYLWORD)outPtr - (MYLWORD)&(req.mesout);
			//printf("Bytes Ready=%u\n", req.bytesReady);
			send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
			//errno = WSAGetLastError();
		}
		else if (htons(req.mesin.opcode) == 2)
		{
			req.acout.opcode = htons(4);
			req.acout.block = htons(0);
			errno = 0;
			req.bytesReady = 4;
			send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
			//errno = WSAGetLastError();
		}

		if (errno)
		{
			sprintf(req.serverError.errormessage, "Communication Error");
			logMess(&req, 1);
			req.attempt = UCHAR_MAX;
			continue;
		}
		else if (ntohs(req.mesin.opcode) == 1)
		{
			errno = 0;
			req.pkt[0] = (packet*)calloc(1, req.blksize + 4);
			req.pkt[1] = (packet*)calloc(1, req.blksize + 4);

			if (errno || !req.pkt[0] || !req.pkt[1])
			{
				sprintf(req.serverError.errormessage, "Memory Error");
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			long ftellLoc = ftell(req.file);

			if (ftellLoc > 0)
			{
				if (fseek(req.file, 0, SEEK_SET))
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(2);
					strcpy(req.serverError.errormessage, "File Access Error");
					send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
					logMess(&req, 1);
					req.attempt = UCHAR_MAX;
					continue;
				}
			}
			else if (ftellLoc < 0)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "File Access Error");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			errno = 0;
			req.pkt[0]->opcode = htons(3);
			req.pkt[0]->block = htons(1);
			req.bytesRead[0] = fread(&req.pkt[0]->buffer, 1, req.blksize, req.file);

			if (errno)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(2);
				strcpy(req.serverError.errormessage, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			if (req.bytesRead[0] == req.blksize)
			{
				req.pkt[1]->opcode = htons(3);
				req.pkt[1]->block = htons(2);
				req.bytesRead[1] = fread(&req.pkt[1]->buffer, 1, req.blksize, req.file);
				if (req.bytesRead[1] < req.blksize)
				{
					fclose(req.file);
					req.file = 0;
				}
			}
			else
			{
				fclose(req.file);
				req.file = 0;
			}

			if (errno)
			{
				req.serverError.opcode = htons(5);
				req.serverError.errorcode = htons(0);
				strcpy(req.serverError.errormessage, "Invalid Path or No Access");
				send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			while (req.attempt <= 3)
			{
				if (fetchAck)
				{
					FD_ZERO(&req.readfds);
					req.tv.tv_sec = 1;
					req.tv.tv_usec = 0;
					FD_SET(req.sock, &req.readfds);
					select(req.sock + 1, &req.readfds, NULL, NULL, &req.tv);

					if (FD_ISSET(req.sock, &req.readfds))
					{
						errno = 0;
						req.bytesRecd = recv(req.sock, (char*)&req.mesin, sizeof(message), 0);
						//errno = WSAGetLastError();
						if (req.bytesRecd <= 0 || errno)
						{
							sprintf(req.serverError.errormessage, "Communication Error");
							logMess(&req, 1);
							req.attempt = UCHAR_MAX;
							break;
						}
						else if(req.bytesRecd >= 4 && ntohs(req.mesin.opcode) == 4)
						{
							if (ntohs(req.acin.block) == req.block)
							{
								req.block++;
								req.fblock++;
								req.attempt = 0;
							}
							else if (req.expiry > time(NULL))
								continue;
							else
								req.attempt++;
						}
						else if (ntohs(req.mesin.opcode) == 5)
						{
							sprintf(req.serverError.errormessage, "Client %s:%u, Error Code %i at Client, %s", inet_ntoa(req.client.v4.sin_addr), ntohs(req.client.v4.sin_port), ntohs(req.clientError.errorcode), req.clientError.errormessage);
							logMess(&req, 1);
							req.attempt = UCHAR_MAX;
							break;
						}
						else
						{
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(0);
							sprintf(req.serverError.errormessage, "Unexpected Option Code %i", ntohs(req.mesin.opcode));
							send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
							logMess(&req, 1);
							req.attempt = UCHAR_MAX;
							break;
						}
					}
					else if (req.expiry > time(NULL))
						continue;
					else
						req.attempt++;
				}
				else
				{
					fetchAck = true;
					req.acin.block = 1;
					req.block = 1;
					req.fblock = 1;
				}

				if (req.attempt >= 3)
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);

					if (req.fblock && !req.block)
						strcpy(req.serverError.errormessage, "Large File, Block# Rollover not supported by Client");
					else
						strcpy(req.serverError.errormessage, "Timeout");

					logMess(&req, 1);
					send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					break;
				}
				else if (!req.fblock)
				{
					errno = 0;
					send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
					//errno = WSAGetLastError();
					if (errno)
					{
						sprintf(req.serverError.errormessage, "Communication Error");
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
					req.expiry = time(NULL) + req.timeout;
				}
				else if (ntohs(req.pkt[0]->block) == req.block)
				{
					errno = 0;
					send(req.sock, (const char*)req.pkt[0], req.bytesRead[0] + 4, 0);
					//errno = WSAGetLastError();
					if (errno)
					{
						sprintf(req.serverError.errormessage, "Communication Error");
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
					req.expiry = time(NULL) + req.timeout;

					if (req.file)
					{
						req.tblock = ntohs(req.pkt[1]->block) + 1;
						if (req.tblock == req.block)
						{
							req.pkt[1]->block = htons(++req.tblock);
							req.bytesRead[1] = fread(&req.pkt[1]->buffer, 1, req.blksize, req.file);

							if (errno)
							{
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(4);
								sprintf(req.serverError.errormessage, strerror(errno));
								send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
								logMess(&req, 1);
								req.attempt = UCHAR_MAX;
								break;
							}
							else if (req.bytesRead[1] < req.blksize)
							{
								fclose(req.file);
								req.file = 0;
							}
						}
					}
				}
				else if (ntohs(req.pkt[1]->block) == req.block)
				{
					errno = 0;
					send(req.sock, (const char*)req.pkt[1], req.bytesRead[1] + 4, 0);
					//errno = WSAGetLastError();
					if (errno)
					{
						sprintf(req.serverError.errormessage, "Communication Error");
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}

					req.expiry = time(NULL) + req.timeout;

					if (req.file)
					{
						req.tblock = ntohs(req.pkt[0]->block) + 1;
						if (req.tblock == req.block)
						{
							req.pkt[0]->block = htons(++req.tblock);
							req.bytesRead[0] = fread(&req.pkt[0]->buffer, 1, req.blksize, req.file);
							if (errno)
							{
								req.serverError.opcode = htons(5);
								req.serverError.errorcode = htons(4);
								sprintf(req.serverError.errormessage, strerror(errno));
								send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
								logMess(&req, 1);
								req.attempt = UCHAR_MAX;
								break;
							}
							else if (req.bytesRead[0] < req.blksize)
							{
								fclose(req.file);
								req.file = 0;
							}
						}
					}
				}
				else
				{
					sprintf(req.serverError.errormessage, "%u Blocks Served", req.fblock - 1);
					logMess(&req, 2);
					req.attempt = UCHAR_MAX;
					break;
				}
			}
		}
		else if (ntohs(req.mesin.opcode) == 2)
		{
			printf("Enter PUT\n");
			errno = 0;
			req.pkt[0] = (packet*)calloc(1, req.blksize + 4);

			if (errno || !req.pkt[0])
			{
				sprintf(req.serverError.errormessage, "Memory Error");
				logMess(&req, 1);
				req.attempt = UCHAR_MAX;
				continue;
			}

			while (req.attempt <= 3)
			{
				FD_ZERO(&req.readfds);
				req.tv.tv_sec = 1;
				req.tv.tv_usec = 0;
				FD_SET(req.sock, &req.readfds);
				select(req.sock + 1, &req.readfds, NULL, NULL, &req.tv);

				if (FD_ISSET(req.sock, &req.readfds))
				{
					errno = 0;
					req.bytesRecd = recv(req.sock, (char*)req.pkt[0], req.blksize + 4, 0);
					//errno = WSAGetLastError();

					if (errno)
					{
						sprintf(req.serverError.errormessage, "Communication Error");
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
				}
				else 
					req.bytesRecd = 0;

				if (req.bytesRecd >= 4)
				{
					if (ntohs(req.pkt[0]->opcode) == 3)
					{
						req.tblock = req.block + 1;

						if (ntohs(req.pkt[0]->block) == req.tblock)
						{
							req.acout.opcode = htons(4);
							req.acout.block = req.pkt[0]->block;
							req.block++;
							req.fblock++;
							req.bytesReady = 4;
							req.expiry = time(NULL) + req.timeout;

							errno = 0;
							send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
							//errno = WSAGetLastError();

							if (errno)
							{
								sprintf(req.serverError.errormessage, "Communication Error");
								logMess(&req, 1);
								req.attempt = UCHAR_MAX;
								break;
							}

							if (req.bytesRecd > 4)
							{
								errno = 0;
								if (fwrite(&req.pkt[0]->buffer, req.bytesRecd - 4, 1, req.file) != 1 || errno)
								{
									req.serverError.opcode = htons(5);
									req.serverError.errorcode = htons(3);
									strcpy(req.serverError.errormessage, "Disk full or allocation exceeded");
									send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
									logMess(&req, 1);
									req.attempt = UCHAR_MAX;
									break;
								}
								else
									req.attempt = 0;
							}
							else
								req.attempt = 0;

							if ((MYWORD)req.bytesRecd < req.blksize + 4)
							{
								fclose(req.file);
								req.file = 0;
								sprintf(req.serverError.errormessage, "%u Blocks Received", req.fblock);
								logMess(&req, 2);
								req.attempt = UCHAR_MAX;
								break;
							}
						}
						else if (req.expiry > time(NULL))
							continue;
						else if (req.attempt >= 3)
						{
							req.serverError.opcode = htons(5);
							req.serverError.errorcode = htons(0);

							if (req.fblock && !req.block)
								strcpy(req.serverError.errormessage, "Large File, Block# Rollover not supported by Client");
							else
								strcpy(req.serverError.errormessage, "Timeout");

							logMess(&req, 1);
							send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
							req.attempt = UCHAR_MAX;
							break;
						}
						else
						{
							req.expiry = time(NULL) + req.timeout;
							errno = 0;
							send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
							//errno = WSAGetLastError();
							req.attempt++;

							if (errno)
							{
								sprintf(req.serverError.errormessage, "Communication Error");
								logMess(&req, 1);
								req.attempt = UCHAR_MAX;
								break;
							}
						}
					}
					else if (req.bytesRecd > (int)sizeof(message))
					{
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(0);
						sprintf(req.serverError.errormessage, "Error: Incoming Packet too large");
						send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
					else if (ntohs(req.pkt[0]->opcode) == 5)
					{
						sprintf(req.serverError.errormessage, "Error Code %i at Client, %s", ntohs(req.pkt[0]->block), &req.pkt[0]->buffer);
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
					else
					{
						req.serverError.opcode = htons(5);
						req.serverError.errorcode = htons(4);
						sprintf(req.serverError.errormessage, "Unexpected Option Code %i", ntohs(req.pkt[0]->opcode));
						send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
				}
				else if (req.expiry > time(NULL))
					continue;
				else if (req.attempt >= 3)
				{
					req.serverError.opcode = htons(5);
					req.serverError.errorcode = htons(0);

					if (req.fblock && !req.block)
						strcpy(req.serverError.errormessage, "Large File, Block# Rollover not supported by Client");
					else
						strcpy(req.serverError.errormessage, "Timeout");

					logMess(&req, 1);
					send(req.sock, (const char*)&req.serverError, strlen(req.serverError.errormessage) + 5, 0);
					req.attempt = UCHAR_MAX;
					break;
				}
				else
				{
					req.expiry = time(NULL) + req.timeout;
					errno = 0;
					send(req.sock, (const char*)&req.mesout, req.bytesReady, 0);
					//errno = WSAGetLastError();
					req.attempt++;

					if (errno)
					{
						sprintf(req.serverError.errormessage, "Communication Error");
						logMess(&req, 1);
						req.attempt = UCHAR_MAX;
						break;
					}
				}
			}
		}
	}
	while (cleanReq(&req));

	pthread_mutex_lock( &mutCount );
	totalThreads--;
	pthread_mutex_unlock( &mutCount );
	//printf("Thread Killed\n");
	pthread_exit(NULL);
}

bool cleanReq(request* req)
{
	//printf("cleaning\n");

	if (req->file)
		fclose(req->file);

	if (req->sock >= 0)
		close(req->sock);

	if (req->pkt[0])
		free(req->pkt[0]);

	if (req->pkt[1])
		free(req->pkt[1]);

	pthread_mutex_lock( &mutCount );
	activeThreads--;
	pthread_mutex_unlock( &mutCount );

	//printf("cleaned\n");

	return (totalThreads <= minThreads);
}

bool getSection(const char *sectionName, char *buffer, MYBYTE serial, char *fileName)
{
	//printf("%s=%s\n",fileName,sectionName);
	char section[128];
	sprintf(section, "[%s]", sectionName);
	myUpper(section);
	FILE *f = fopen(fileName, "rt");
	char buff[512];
	MYBYTE found = 0;

	if (f)
	{
		while (fgets(buff, 511, f))
		{
			myUpper(buff);
			myTrim(buff, buff);

			if (strstr(buff, section) == buff)
			{
				found++;
				if (found == serial)
				{
					//printf("%s=%s\n",fileName,sectionName);
					while (fgets(buff, 511, f))
					{
						myTrim(buff, buff);

						if (strstr(buff, "[") == buff)
							break;

						if ((*buff) >= '0' && (*buff) <= '9' || (*buff) >= 'A' && (*buff) <= 'Z' || (*buff) >= 'a' && (*buff) <= 'z' || ((*buff) && strchr("/\\?*", (*buff))))
						{
							buffer += sprintf(buffer, "%s", buff);
							buffer++;
						}
					}
					break;
				}
			}
		}
		fclose(f);
	}

	*buffer = 0;
	*(buffer + 1) = 0;
	return (found == serial);
}

FILE *openSection(const char *sectionName, MYBYTE serial, char *fileName)
{
	//printf("%s=%s\n",fileName,sectionName);
	char section[128];
	sprintf(section, "[%s]", sectionName);
	myUpper(section);
	FILE *f = fopen(fileName, "rt");
	char buff[512];
	MYBYTE found = 0;

	if (f)
	{
		while (fgets(buff, 511, f))
		{
			myUpper(buff);
			myTrim(buff, buff);

			if (strstr(buff, section) == buff)
			{
				found++;

				if (found == serial)
					return f;
			}
		}
		fclose(f);
	}
	return NULL;
}

char *readSection(char* buff, FILE *f)
{
	while (fgets(buff, 511, f))
	{
		myTrim(buff, buff);

		if (*buff == '[')
			break;

		if ((*buff) >= '0' && (*buff) <= '9' || (*buff) >= 'A' && (*buff) <= 'Z' || (*buff) >= 'a' && (*buff) <= 'z' || ((*buff) && strchr("/\\?*", (*buff))))
			return buff;
	}

	fclose(f);
	return NULL;
}

char* myGetToken(char* buff, MYBYTE index)
{
	while (*buff)
	{
		if (index)
			index--;
		else
			break;

		buff += strlen(buff) + 1;
	}

	return buff;
}

MYWORD myTokenize(char *target, char *source, char *sep, bool whiteSep)
{
	bool found = true;
	char *dp = target;
	MYWORD kount = 0;

	while (*source)
	{
		if (sep && sep[0] && strchr(sep, (*source)))
		{
			found = true;
			source++;
			continue;
		}
		else if (whiteSep && (*source) <= 32)
		{
			found = true;
			source++;
			continue;
		}

		if (found)
		{
			if (target != dp)
			{
				*dp = 0;
				dp++;
			}
			kount++;
		}

		found = false;
		*dp = *source;
		dp++;
		source++;
	}

	*dp = 0;
	dp++;
	*dp = 0;

	//printf("%s\n", target);

	return kount;
}

char* myTrim(char *target, char *source)
{
	while ((*source) && (*source) <= 32)
		source++;

	int i = 0;

	for (; i < 511 && source[i]; i++)
		target[i] = source[i];

	target[i] = source[i];
	i--;

	for (; i >= 0 && target[i] <= 32; i--)
		target[i] = 0;

	return target;
}

/*
void mySplit(char *name, char *value, char *source, char splitChar)
{
	char *dp = strchr(source, splitChar);

	if (dp)
	{
		strncpy(name, source, (dp - source));
		name[dp - source] = 0;
		strcpy(value, dp + 1);
		myTrim(name, name);
		myTrim(value, value);
	}
	else
	{
 		strcpy(name, source);
		myTrim(name, name);
 		*value = 0;
	}
}
*/

void mySplit(char *name, char *value, char *source, char splitChar)
{
	int i = 0;
	int j = 0;
	int k = 0;

	for (; source[i] && j <= 510 && source[i] != splitChar; i++, j++)
	{
		name[j] = source[i];
	}

	if (source[i])
	{
		i++;
		for (; k <= 510 && source[i]; i++, k++)
		{
			value[k] = source[i];
		}
	}

	name[j] = 0;
	value[k] = 0;

	myTrim(name, name);
	myTrim(value, value);
	//printf("%s %s\n", name, value);
}

char *IP2String(char *target, MYDWORD ip)
{
	data15 inaddr;
	inaddr.ip = (unsigned int)ip;
	sprintf(target, "%u.%u.%u.%u", inaddr.octate[0], inaddr.octate[1], inaddr.octate[2], inaddr.octate[3]);
	return target;
}

char *myUpper(char *string)
{
	char diff = 'a' - 'A';
	MYWORD len = strlen(string);
	for (int i = 0; i < len; i++)
		if (string[i] >= 'a' && string[i] <= 'z')
			string[i] -= diff;
	return string;
}

char *myLower(char *string)
{
	char diff = 'a' - 'A';
	MYWORD len = strlen(string);
	for (int i = 0; i < len; i++)
		if (string[i] >= 'A' && string[i] <= 'Z')
			string[i] += diff;
	return string;
}

bool isIP(char *string)
{
	int j = 0;

	for (; *string; string++)
	{
		if (*string == '.' && *(string + 1) != '.')
			j++;
		else if (*string < '0' || *string > '9')
			return 0;
	}

	if (j == 3)
		return 1;
	else
		return 0;
}

void *init(void *lpParam)
{
	//printf("In Init\n");
	memset(&cfig, 0, sizeof(cfig));
	cfig.pppid = getpid();

	char raw[512];
	char name[512];
	char value[512];
	char temp[512];
	FILE *f = NULL;

	if (verbatim)
	{
		cfig.logLevel = 2;
		printf("%s\n\n", sVersion);
	}
	else if (f = openSection("LOGGING", 1, iniFile))
	{
		cfig.logLevel = 1;
		tempbuff[0] = 0;

		while (readSection(raw, f))
		{
			if (!strcasecmp(raw, "None"))
				cfig.logLevel = 0;
			else if (!strcasecmp(raw, "Errors"))
				cfig.logLevel = 1;
			else if (!strcasecmp(raw, "All"))
				cfig.logLevel = 2;
			else
				sprintf(tempbuff, "Section [LOGGING], Invalid LogLevel: %s", raw);
		}
	}

	if (!verbatim && cfig.logLevel && logFile[0])
	{
		time_t t = time(NULL);
		tm *ttm = localtime(&t);
		loggingDay = ttm->tm_yday;
		strftime(extbuff, sizeof(extbuff), logFile, ttm);

		cfig.logfile = fopen(extbuff, "at");

		if (cfig.logfile)
		{
			sprintf(logBuff, "%s Starting..", sVersion);
			logMess(logBuff, 1);

			if (tempbuff[0])
				logMess(tempbuff, 0);
		}
    }

	if ((f = fopen(iniFile, "rt")))
	{
		fclose(f);
	}
	else
	{
		sprintf(logBuff, "Warning: file %s not found, defaults will be used", iniFile);
		logMess(logBuff, 1);
	}

	if (f = openSection("HOME", 1, iniFile))
	{
		while (readSection(raw, f))
		{
			mySplit(name, value, raw, '=');

			if (strlen(value))
			{
				if (!cfig.homes[0].alias[0] && cfig.homes[0].target[0])
				{
					sprintf(logBuff, "Section [HOME], alias and bare path mixup, entry %s ignored", raw);
					logMess(logBuff, 1);
				}
				else if (strchr(name, notFileSep) || strchr(name, fileSep) || strchr(name, '>') || strchr(name, '<') || strchr(name, '.'))
				{
					sprintf(logBuff, "Section [HOME], invalid chars in alias %s, entry ignored", name);
					logMess(logBuff, 1);
				}
				else if (name[0] && strlen(name) < 64 && value[0])
				{
					for (int i = 0; i < 8; i++)
					{
						if (cfig.homes[i].alias[0] && !strcasecmp(name, cfig.homes[i].alias))
						{
							sprintf(logBuff, "Section [HOME], Duplicate Entry: %s ignored", raw);
							logMess(logBuff, 1);
							break;
						}
						else if (!cfig.homes[i].alias[0])
						{
							strcpy(cfig.homes[i].alias, name);
							strcpy(cfig.homes[i].target, value);

							if (cfig.homes[i].target[strlen(cfig.homes[i].target) - 1] != fileSep)
							{
								tempbuff[0] = fileSep;
								tempbuff[1] = 0;
								strcat(cfig.homes[i].target, tempbuff);
							}

							break;
						}
					}
				}
				else
				{
					sprintf(logBuff, "Section [HOME], alias name %s too large", name);
					logMess(logBuff, 1);
				}
			}
			else if (!cfig.homes[0].alias[0] && !cfig.homes[0].target[0])
			{
				strcpy(cfig.homes[0].target, name);

				if (cfig.homes[0].target[strlen(cfig.homes[0].target) - 1] != fileSep)
				{
					tempbuff[0] = fileSep;
					tempbuff[1] = 0;
					strcat(cfig.homes[0].target, tempbuff);
				}
			}
			else if (cfig.homes[0].alias[0])
			{
				sprintf(logBuff, "Section [HOME], alias and bare path mixup, entry %s ignored", raw);
				logMess(logBuff, 1);
			}
			else if (cfig.homes[0].target[0])
			{
				sprintf(logBuff, "Section [HOME], Duplicate Path: %s ignored", raw);
				logMess(logBuff, 1);
			}
			else
			{
				sprintf(logBuff, "Section [HOME], missing = sign, Invalid Entry: %s ignored", raw);
				logMess(logBuff, 1);
			}
		}
	}

	cfig.fileRead = true;

	if (f = openSection("TFTP-OPTIONS", 1, iniFile))
	{
		while (readSection(raw, f))
		{
			mySplit(name, value, raw, '=');

			if (strlen(value))
			{
				if (!strcasecmp(name, "UserName"))
				{
					if (strlen(value) < 128)
					{
						passwd *pwd = getpwnam(value);

						if (pwd)
						{
							cfig.pw_uid = pwd->pw_uid;
							cfig.pw_gid = pwd->pw_gid;
							strcpy(cfig.username, value);

							if (!cfig.homes[0].target[0])
							{
								if (cfig.pw_uid)
									sprintf(cfig.homes[0].target, "%s/", pwd->pw_dir);
								else
									strcpy(cfig.homes[0].target, "/home/");
							}
						}
						else
						{
							sprintf(logBuff, "Section [TFTP-OPTIONS], unknown username: %s, stopping", value);
							logMess(logBuff, 1);
							exit(EXIT_FAILURE);
						}
					}
					else
					{
						sprintf(logBuff, "Section [TFTP-OPTIONS], invalid username: %s, stopping", value);
						logMess(logBuff, 1);
						exit(EXIT_FAILURE);
					}
				}
				else if (!strcasecmp(name, "blksize"))
				{
					MYDWORD tblksize = atol(value);

					if (tblksize < 512)
						blksize = 512;
					else if (tblksize > 65464)
						blksize = 65464;
					else
						blksize = tblksize;
				}
				else if (!strcasecmp(name, "threadpoolsize"))
				{
					minThreads = atol(value);
					if (minThreads < 1)
						minThreads = 0;
					else if (minThreads > 100)
						minThreads = 100;
				}
				else if (!strcasecmp(name, "timeout"))
				{
					timeout = atol(value);
					if (timeout < 1)
						timeout = 1;
					else if (timeout > UCHAR_MAX)
						timeout = UCHAR_MAX;
				}
				else if (!strcasecmp(name, "Read"))
				{
					if (strchr("Yy", *value))
						cfig.fileRead = true;
					else
						cfig.fileRead = false;
				}
				else if (!strcasecmp(name, "Write"))
				{
					if (strchr("Yy", *value))
						cfig.fileWrite = true;
					else
						cfig.fileWrite = false;
				}
				else if (!strcasecmp(name, "Overwrite"))
				{
					if (strchr("Yy", *value))
						cfig.fileOverwrite = true;
					else
						cfig.fileOverwrite = false;
				}
				else if (!strcasecmp(name, "port-range"))
				{
					char *ptr = strchr(value, '-');
					if (ptr)
					{
						*ptr = 0;
						cfig.minport = atol(value);
						cfig.maxport = atol(++ptr);

						if (cfig.minport < 1024 || cfig.minport >= USHRT_MAX || cfig.maxport < 1024 || cfig.maxport >= USHRT_MAX || cfig.minport > cfig.maxport)
						{
							cfig.minport = 0;
							cfig.maxport = 0;

							sprintf(logBuff, "Invalid port range %s", value);
							logMess(logBuff, 1);
						}
					}
					else
					{
						sprintf(logBuff, "Invalid port range %s", value);
						logMess(logBuff, 1);
					}
				}
				else
				{
					sprintf(logBuff, "Warning: unknown option %s, ignored", name);
					logMess(logBuff, 1);
				}
			}
		}
	}

	if (f = openSection("ALLOWED-CLIENTS", 1, iniFile))
	{
		int i = 0;

		while (readSection(raw, f))
		{
			if (i < 32)
			{
				MYDWORD rs = 0;
				MYDWORD re = 0;
				mySplit(name, value, raw, '-');
				rs = htonl(my_inet_addr(name));

				if (strlen(value))
					re = htonl(my_inet_addr(value));
				else
					re = rs;

				if (rs && rs != INADDR_NONE && re && re != INADDR_NONE && rs <= re)
				{
					cfig.hostRanges[i].rangeStart = rs;
					cfig.hostRanges[i].rangeEnd = re;
					i++;
				}
				else
				{
					sprintf(logBuff, "Section [ALLOWED-CLIENTS] Invalid entry %s in ini file, ignored", raw);
					logMess(logBuff, 1);
				}
			}
		}
	}

	if (!cfig.username[0])
	{
		passwd *pwd = getpwuid(getuid());
		strcpy(cfig.username, pwd->pw_name);

		if (!cfig.homes[0].target[0])
		{
			if (pwd->pw_uid)
				strcpy(cfig.homes[0].target, pwd->pw_dir);
			else
				strcpy(cfig.homes[0].target, "/home/");
		}
	}

	sprintf(logBuff, "username: %s", cfig.username);
	logMess(logBuff, 1);

	for (int i = 0; i < MAX_SERVERS; i++)
		if (cfig.homes[i].target[0])
		{
			sprintf(logBuff, "alias /%s is mapped to %s", cfig.homes[i].alias, cfig.homes[i].target);
			logMess(logBuff, 1);
		}

	if (cfig.hostRanges[0].rangeStart)
	{
		char temp[128];

		for (MYWORD i = 0; i <= sizeof(cfig.hostRanges) && cfig.hostRanges[i].rangeStart; i++)
		{
			sprintf(logBuff, "%s", "permitted clients: ");
			sprintf(temp, "%s-", IP2String(tempbuff, htonl(cfig.hostRanges[i].rangeStart)));
			strcat(logBuff, temp);
			sprintf(temp, "%s", IP2String(tempbuff, htonl(cfig.hostRanges[i].rangeEnd)));
			strcat(logBuff, temp);
			logMess(logBuff, 1);
		}
	}
	else
	{
		sprintf(logBuff, "%s", "permitted clients: all");
		logMess(logBuff, 1);
	}

	if (cfig.minport)
	{
		sprintf(logBuff, "server port range: %u-%u", cfig.minport, cfig.maxport);
		logMess(logBuff, 1);
	}
	else
	{
		sprintf(logBuff, "server port range: all");
		logMess(logBuff, 1);
	}

	sprintf(logBuff, "max blksize: %u", blksize);
	logMess(logBuff, 1);
	sprintf(logBuff, "default blksize: %u", 512);
	logMess(logBuff, 1);
	sprintf(logBuff, "default timeout: %u", timeout);
	logMess(logBuff, 1);
	sprintf(logBuff, "file read allowed: %s", cfig.fileRead ? "Yes" : "No");
	logMess(logBuff, 1);
	sprintf(logBuff, "file create allowed: %s", cfig.fileWrite ? "Yes" : "No");
	logMess(logBuff, 1);
	sprintf(logBuff, "file overwrite allowed: %s", cfig.fileOverwrite ? "Yes" : "No");
	logMess(logBuff, 1);

	if (!verbatim)
	{
		sprintf(logBuff, "logging: %s", cfig.logLevel > 1 ? "all" : "errors");
		logMess(logBuff, 1);
	}

	pthread_mutex_lock( &mutThread );

	if (minThreads)
	{
		int j = 0;

		for (int i=0; i<minThreads; i++)
		{
			//Thanks erez for suggenting this
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
			int errcode = pthread_create(&threadId, &attr, processRequest, NULL);
			pthread_attr_destroy(&attr);

			if(errcode)
			//if (pthread_create(&threadId, 0, processRequest, NULL))
			{
				sprintf(logBuff, "Warning Thread# %i Creation Failed with error %i", i, errcode);
				logMess(logBuff, 1);
			}
			else
				j++;
		}

		sprintf(logBuff, "thread pool size: %u", j);
		logMess(logBuff, 1);
	}

	cfig.fixedSocket = socket(AF_INET, SOCK_DGRAM, 0);

	if (cfig.fixedSocket < 0)
	{
		sprintf(logBuff, "Failed to Create Socket");
		logMess(logBuff, 1);
		exit(EXIT_FAILURE);
	}

	do
	{
		bool bindfailed = false;

		if (!cfig.ifspecified && (f = openSection("LISTEN-ON", 1, iniFile)))
		{
			MYBYTE i = 0;
			MYWORD temp_port = 69;

			while (readSection(raw, f))
			{
				MYWORD port = 69;
				bool isIPv6 = false;

				cfig.ifspecified = true;
				if (!strncmp("IPv6", raw, 4)) 
				{
					//printf("ip address %s\n", raw);
					mySplit(temp, name, raw, '=');
					memset(value, 0, sizeof(value));	
					isIPv6 = true;
				}
				else
				{
					mySplit(name, value, raw, ':');
				}
				printf("ip address %s\n", name);

				if (value[0]) 
				{
					port = atoi(value);
					temp_port = port;
				}

				if(i < MAX_SERVERS)
				{
					MYDWORD addr = my_inet_addr(name);
					if (isIPv6)  
					{
						strcpy(newNetwork.listenAddress[i], name);
						newNetwork.listenPorts[i] = temp_port;
						newNetwork.listenIPv6[i] = true;
						memset(&newNetwork.listenServers[i], 0, sizeof(newNetwork.listenServers[i]));
						newNetwork.listenServers[i] = addr;
						i++;
					}
					else if (isIP(name))
					{
						if (!addr)
						{
							newNetwork.listenServers[0] = 0;
							newNetwork.listenPorts[0] = port;
							fclose(f);
							break;
						}
						else if (!findServer(newNetwork.listenServers, addr))
						{
							newNetwork.listenServers[i] = addr;
							newNetwork.listenPorts[i] = port;
							newNetwork.listenIPv6[i] = false;
							i++;
						}
					}
					else
					{
						sprintf(logBuff, "Warning: Section [LISTEN-ON], Invalid Interface Address %s, ignored", raw);
						logMess(logBuff, 1);
					}
				}
			}
		}

		if (!cfig.ifspecified)
		{
			getInterfaces(&newNetwork);
			memcpy(cfig.oldservers, newNetwork.staticServers, (MAX_SERVERS * sizeof(MYDWORD)));

			for (MYBYTE n = 0; n < MAX_SERVERS && newNetwork.staticServers[n]; n++)
			{
				newNetwork.listenServers[n] = newNetwork.staticServers[n];
				newNetwork.listenPorts[n] = 69;
			}
		}

		MYBYTE i = 0;

		for (int j = 0; j < MAX_SERVERS && newNetwork.listenPorts[j]; j++)
		{
			int k = 0;

			for (; k < MAX_SERVERS && network.tftpConn[k].loaded; k++)
			{
				if (network.tftpConn[k].ready && network.tftpConn[k].server == newNetwork.listenServers[j] && network.tftpConn[k].port == newNetwork.listenPorts[j])
					break;
			}

			if (network.tftpConn[k].ready && network.tftpConn[k].server == newNetwork.listenServers[j] && network.tftpConn[k].port == newNetwork.listenPorts[j])
			{
				memcpy(&(newNetwork.tftpConn[i]), &(network.tftpConn[k]), sizeof(tftpConnType));

				if (newNetwork.maxFD < newNetwork.tftpConn[i].sock)
					newNetwork.maxFD = newNetwork.tftpConn[i].sock;

				network.tftpConn[k].ready = false;
				//printf("%d, %s found\n", i, IP2String(tempbuff, newNetwork.tftpConn[i].server));
				i++;
				continue;
			}
			else
			{
				if (newNetwork.listenIPv6[j])
				{
					int on = 1;
                			memset(&newNetwork.tftpConn[i].addr, 0, sizeof(newNetwork.tftpConn[i].addr));
					//inet_pton(AF_INET6, "2000:0:0::1", (void *)&newNetwork.tftpConn[i].addr.v6.sin6_addr.s6_addr);
					inet_pton(AF_INET6, newNetwork.listenAddress[j], (void *)&newNetwork.tftpConn[i].addr.v6.sin6_addr.s6_addr);
					newNetwork.tftpConn[i].sock = socket(AF_INET6, SOCK_DGRAM, 0);
					newNetwork.tftpConn[i].addr.v6.sin6_family = AF_INET6;
					newNetwork.tftpConn[i].addr.v6.sin6_port = htons(newNetwork.listenPorts[j]);
					newNetwork.tftpConn[i].server = newNetwork.listenServers[j];
					//memcpy(&newNetwork.tftpConn[i].server, &newNetwork.tftpConn[i].addr.v6.sin6_addr.s6_addr, sizeof(newNetwork.tftpConn[i].server));
					//memcpy(&newNetwork.tftpConn[i].server, &in6addr_any, sizeof(newNetwork.tftpConn[i].server));
					printf("IIII %s\n", newNetwork.listenAddress[j]);
					//newNetwork.maxFD = newNetwork.tftpConn[i].sock;
					if (setsockopt(newNetwork.tftpConn[i].sock, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&on,sizeof(on)))
						printf("ipv6 setsockopt error\n");
					//if (bind(newNetwork.tftpConn[i].sock, (struct sockaddr *)&in6addr_any, sizeof(newNetwork.tftpConn[i].addr)) < 0) 
					if (bind(newNetwork.tftpConn[i].sock, (struct sockaddr *)&newNetwork.tftpConn[i].addr, sizeof(newNetwork.tftpConn[i].addr)) < 0)
						printf("ipv6 bind error\n");
					//ipv6 = 1;
				}
				else
				{
					newNetwork.tftpConn[i].sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

					if (newNetwork.tftpConn[i].sock == INVALID_SOCKET)
					{
						bindfailed = true;
						sprintf(logBuff, "Failed to Create Socket");
						logMess(logBuff, 1);
						continue;
					}

					//printf("Socket %u\n", newNetwork.tftpConn[i].sock);

					errno = 0;
					newNetwork.tftpConn[i].addr.v4.sin_family = AF_INET;
					newNetwork.tftpConn[i].addr.v4.sin_addr.s_addr = newNetwork.listenServers[j];
					newNetwork.tftpConn[i].addr.v4.sin_port = htons(newNetwork.listenPorts[j]);
					int nRet = bind(newNetwork.tftpConn[i].sock, (sockaddr*)&newNetwork.tftpConn[i].addr, sizeof(struct sockaddr_in));

					if (nRet == SOCKET_ERROR || errno)
					{
						bindfailed = true;
						close(newNetwork.tftpConn[i].sock);
						sprintf(logBuff, "%s Port %i bind failed, %s", IP2String(tempbuff, newNetwork.listenServers[j]), newNetwork.listenPorts[j], strerror(errno));
						logMess(logBuff, 1);
						continue;
					}
				}
				

				newNetwork.tftpConn[i].loaded = true;
				newNetwork.tftpConn[i].ready = true;
				newNetwork.tftpConn[i].server = newNetwork.listenServers[j];
				newNetwork.tftpConn[i].port = newNetwork.listenPorts[j];

				//printf("%d, %s created\n", i, IP2String(tempbuff, newNetwork.tftpConn[i].server));

				if (newNetwork.maxFD < newNetwork.tftpConn[i].sock)
					newNetwork.maxFD = newNetwork.tftpConn[i].sock;

				if (!newNetwork.listenServers[j])
					break;

				i++;
			}
		}
		newNetwork.maxFD++;

		if (bindfailed)
			cfig.failureCount++;
		else
			cfig.failureCount = 0;

		closeConn();
		memcpy(&network, &newNetwork, sizeof(data1));

		//printf("%i %i %i\n", network.tftpConn[0].ready, network.dnsUdpConn[0].ready, network.dnsTcpConn[0].ready);

		if (!network.tftpConn[0].ready)
		{
			sprintf(logBuff, "No Static Interface ready, Waiting...");
			logMess(logBuff, 1);
			continue;
		}

		for (int i = 0; i < MAX_SERVERS && network.tftpConn[i].port; i++)
		{
			sprintf(logBuff, "listening on: %s:%i", IP2String(tempbuff, network.tftpConn[i].server), network.tftpConn[i].port);
			logMess(logBuff, 1);
		}

		network.ready = true;

	} while (detectChange());

	//printf("Exiting init\n");

	pthread_exit(NULL);
}

bool detectChange()
{
	//printf("Entering detectchange\n");

	if (!cfig.failureCount)
	{
		if (cfig.ifspecified || cfig.pw_uid)
			return false;
	}

    while (true)
    {
		sleep(20);
		//printf("Checking Networks (failue count=%u, failue cycle=%u)..\n", cfig.failureCount, cfig.failureCycle);

		if (!cfig.ifspecified)
			getInterfaces(&newNetwork);
		else
		{
			memcpy(newNetwork.listenServers, newNetwork.listenServers, (MAX_SERVERS * sizeof(MYDWORD)));
			memcpy(newNetwork.listenPorts, newNetwork.listenPorts, (MAX_SERVERS * sizeof(MYWORD)));
		}

		if (!cfig.ifspecified && memcmp(cfig.oldservers, newNetwork.staticServers, (MAX_SERVERS * sizeof(MYDWORD))))
		{
			memcpy(cfig.oldservers, newNetwork.staticServers, (MAX_SERVERS * sizeof(MYDWORD)));
			sprintf(logBuff, "Network changed, re-detecting Listening Interfaces..");
			logMess(logBuff, 1);
			break;
		}
		else if (cfig.failureCount)
		{
			cfig.failureCycle++;

			if (cfig.failureCycle == (MYDWORD)pow(2, cfig.failureCount))
			{
				sprintf(logBuff, "Retrying failed Listening Interfaces..");
				logMess(logBuff, 1);
				break;
			}
		}
		else
			cfig.failureCycle = 0;
	}

	network.ready = false;

	while (network.busy)
		sleep(1);

	//printf("Returning from detectchange\n");

	return true;
}

void getInterfaces(data1 *network)
{
	memset(network, 0, sizeof(data1));

	Ifc.ifc_len = sizeof(IfcBuf);
	Ifc.ifc_buf = (char*)IfcBuf;

	if (ioctl(cfig.fixedSocket, SIOCGIFCONF, &Ifc) >= 0)
	{

		MYDWORD addr, mask;
		short flags;
		struct ifreq pIfr;
		MYBYTE numInterfaces = Ifc.ifc_len / sizeof(ifreq);

		for (MYBYTE i = 0 ; i < numInterfaces; i++)
		{
			memcpy(&pIfr, &(IfcBuf[i]), sizeof(ifreq));

			if (!ioctl(cfig.fixedSocket, SIOCGIFADDR, &pIfr))
				addr = ((struct sockaddr_in*)&pIfr.ifr_addr)->sin_addr.s_addr;
			else
				addr = 0;

			if (!ioctl(cfig.fixedSocket, SIOCGIFNETMASK, &pIfr))
				mask = ((struct sockaddr_in*)&pIfr.ifr_addr)->sin_addr.s_addr;
			else
				mask = 0;

			if (!ioctl(cfig.fixedSocket, SIOCGIFFLAGS, &pIfr))
				flags = pIfr.ifr_flags;
			else
				flags = 0;


			//printf("%s IFF_RUNNING %d IFF_UP %d\n", (flags & IFF_RUNNING), (flags & IFF_UP));

			if (addr)
				addServer(network->allServers, addr);

			//if (addr && mask && !(flags & IFF_POINTOPOINT) && !(flags & IFF_DYNAMIC))
			//if (addr && mask && !(flags & IFF_POINTOPOINT) && !(flags & IFF_DYNAMIC) && (flags & IFF_RUNNING) && (flags & IFF_UP))
			//if (addr && mask && !(flags & IFF_POINTOPOINT) && !(flags & IFF_LOOPBACK) && !(flags & IFF_DYNAMIC))
			//if (addr && mask && !(flags & IFF_POINTOPOINT) && !(flags & IFF_LOOPBACK))
			if (addr && mask && !(flags & IFF_POINTOPOINT) && (flags & IFF_RUNNING) && (flags & IFF_UP))
			{
				addServer(network->staticServers, addr);
			}
		}
    }
}

bool addServer(MYDWORD *array, MYDWORD ip)
{
	for (MYBYTE i = 0; i < MAX_SERVERS; i++)
	{
		if (!ip || array[i] == ip)
			return 0;
		else if (!array[i])
		{
			array[i] = ip;
			return 1;
		}
	}
	return 0;
}

MYDWORD *findServer(MYDWORD *array, MYDWORD ip)
{
	if (ip)
	{
		for (MYBYTE i = 0; i < MAX_SERVERS && array[i]; i++)
		{
			if (array[i] == ip)
				return &(array[i]);
		}
	}
	return 0;
}

MYDWORD my_inet_addr(char *str)
{
    if (str == NULL || !str[0])
        return INADDR_ANY;
    else
    	return inet_addr(str);
}

void logMess(char *logBuff, MYBYTE logLevel)
{
	pthread_mutex_lock( &mutLog );

	if (verbatim)
		printf("%s\n", logBuff);
	else if (cfig.logfile && logLevel <= cfig.logLevel)
	{
		time_t t = time(NULL);
		tm *ttm = localtime(&t);

		if (ttm->tm_yday != loggingDay)
		{
			loggingDay = ttm->tm_yday;
			strftime(extbuff, sizeof(extbuff), logFile, ttm);
			fprintf(cfig.logfile, "Logging Continued on file %s\n", extbuff);
			fclose(cfig.logfile);
			cfig.logfile = fopen(extbuff, "at");

			if (cfig.logfile)
				fprintf(cfig.logfile, "%s\n\n", sVersion);
			else
				return;
		}

		strftime(extbuff, sizeof(extbuff), "%d-%b-%y %X", ttm);
		fprintf(cfig.logfile, "[%s] %s\n", extbuff, logBuff);
		fflush(cfig.logfile);
	}
	else if (logLevel <= cfig.logLevel)
		syslog(LOG_MAKEPRI(LOG_LOCAL1, LOG_CRIT), logBuff);

	pthread_mutex_unlock( &mutLog );
}

void logMess(request *req, MYBYTE logLevel)
{
	pthread_mutex_lock( &mutLog );

	if (verbatim)
	{
		if (!req->serverError.errormessage[0])
			printf(req->serverError.errormessage, strerror(errno));

		if (req->path[0])
			printf("Client %s:%u %s, %s\n", IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->path, req->serverError.errormessage);
		else
			printf("Client %s:%u, %s\n", IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->serverError.errormessage);

	}
	else if (cfig.logfile && logLevel <= cfig.logLevel)
	{
		time_t t = time(NULL);
		tm *ttm = localtime(&t);

		if (ttm->tm_yday != loggingDay)
		{
			loggingDay = ttm->tm_yday;
			strftime(extbuff, sizeof(extbuff), logFile, ttm);
			fprintf(cfig.logfile, "Logging Continued on file %s\n", extbuff);
			fclose(cfig.logfile);
			cfig.logfile = fopen(extbuff, "at");

			if (cfig.logfile)
				fprintf(cfig.logfile, "%s\n\n", sVersion);
			else
				return;
		}

		strftime(extbuff, sizeof(extbuff), "%d-%b-%y %X", ttm);

		if (req->path[0])
			fprintf(cfig.logfile, "[%s] Client %s:%u %s, %s\n", extbuff, IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->path, req->serverError.errormessage);
		else
			fprintf(cfig.logfile, "[%s] Client %s:%u, %s\n", extbuff, IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->serverError.errormessage);

		fflush(cfig.logfile);
	}
	else if (logLevel <= cfig.logLevel)
	{
		char logBuff[512];

		if (!req->serverError.errormessage[0])
			sprintf(req->serverError.errormessage, strerror(errno));

		if (req->path[0])
			sprintf(logBuff, "Client %s:%u %s, %s\n", IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->path, req->serverError.errormessage);
		else
			sprintf(logBuff, "Client %s:%u, %s\n", IP2String(tempbuff, req->client.v4.sin_addr.s_addr), ntohs(req->client.v4.sin_port), req->serverError.errormessage);

		syslog(LOG_MAKEPRI(LOG_LOCAL1, LOG_CRIT), logBuff);
	}

	pthread_mutex_unlock( &mutLog );
}

