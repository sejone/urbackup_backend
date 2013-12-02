/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011  Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "../../Interface/Server.h"

#include "FileClient.h"

#include "../../common/data.h"
#include "../../stringtools.h"

#include "../../md5.h"

#include <iostream>
#include <memory.h>
#include <algorithm>

extern std::string server_identity;

namespace
{
	const std::string str_tmpdir="C:\\Windows\\Temp";
	const _u64 c_checkpoint_dist=512*1024;
#ifndef _DEBUG
	const unsigned int DISCOVERY_TIMEOUT=30000; //30sec
#else
	const unsigned int DISCOVERY_TIMEOUT=1000; //1sec
#endif
}

void Log(std::string str)
{
	Server->Log(str);
}

int curr_fnum=0;

bool setSockP(SOCKET sock)
{
#ifdef _WIN32
		DWORD dwBytesReturned = 0;
		BOOL bNewBehavior = FALSE;
		int status;

		// disable  new behavior using
		// IOCTL: SIO_UDP_CONNRESET
		#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
		status = WSAIoctl(sock, SIO_UDP_CONNRESET,
						&bNewBehavior, sizeof(bNewBehavior),
					NULL, 0, &dwBytesReturned,
					NULL, NULL);
		if (SOCKET_ERROR == status)
		{
			return false;
		}
#endif
        return true;
}    

FileClient::FileClient(bool enable_find_servers, int protocol_version, bool internet_connection,
	FileClient::ReconnectionCallback *reconnection_callback, FileClient::NoFreeSpaceCallback *nofreespace_callback)
	: tcpsock(NULL), starttime(0), connect_starttime(0), socket_open(false), connected(false),
	num_games(0), num_games_get(0), num_games_res(false), res_name(false), serveraddr(), local_ip(),
	max_version(), server_addr(), connection_id(), 
	protocol_version(protocol_version), internet_connection(internet_connection),
	transferred_bytes(0), reconnection_callback(reconnection_callback),
	nofreespace_callback(nofreespace_callback), reconnection_timeout(300000)
{
	memset(buffer, 0, BUFFERSIZE_UDP);

	if(enable_find_servers)
	{
#ifndef _WIN32
		std::string bcast_interfaces=Server->getServerParameter("broadcast_interfaces", "");

		std::vector<std::string> bcast_filter;
		if(!bcast_interfaces.empty())
		{
			Tokenize(bcast_interfaces, bcast_filter, ";,");
		}

		ifaddrs *ifap;
		int rc=getifaddrs(&ifap);
		if(rc==0)
		{
			for(;ifap!=NULL;ifap=ifap->ifa_next)
			{
				bool found_name = bcast_filter.empty() || std::find(bcast_filter.begin(), bcast_filter.end(), ifap->ifa_name)!=bcast_filter.end();

				if(found_name &&
					!(ifap->ifa_flags & IFF_LOOPBACK) 
					&& !(ifap->ifa_flags & IFF_POINTOPOINT) 
					&&  (ifap->ifa_flags & IFF_BROADCAST)
					&&  ifap->ifa_addr->sa_family == AF_INET )
				{			
					SOCKET udpsock=socket(AF_INET,SOCK_DGRAM,0);
					if(udpsock==-1)
					{
						Server->Log(std::string("Error creating socket for interface ")+std::string(ifap->ifa_name), LL_ERROR);
						continue;
					}

					BOOL val=TRUE;
					int rc = setsockopt(udpsock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(BOOL));
					if(rc<0)
					{
						Server->Log(std::string("Setting SO_REUSEADDR failed for interface ")+std::string(ifap->ifa_name), LL_ERROR);
					}

					sockaddr_in source_addr;
					memset(&source_addr, 0, sizeof(source_addr));
					source_addr.sin_addr=((struct sockaddr_in *)ifap->ifa_addr)->sin_addr;
					source_addr.sin_family = AF_INET;
					source_addr.sin_port = htons(UDP_SOURCE_PORT);

					broadcast_addrs.push_back(*((struct sockaddr_in *)ifap->ifa_broadaddr));

					Server->Log(std::string("Binding to interface ")+std::string(ifap->ifa_name)+" for broadcasting...", LL_DEBUG);

					rc = bind(udpsock, (struct sockaddr *)&source_addr, sizeof(source_addr));
					if(rc<0)
					{
						Server->Log(std::string("Binding UDP socket failed for interface ")+std::string(ifap->ifa_name), LL_ERROR);
					}

				
					rc = setsockopt(udpsock, SOL_SOCKET, SO_BROADCAST, (char*)&val, sizeof(BOOL) );
					if(rc<0)
					{
						Server->Log(std::string("Enabling SO_BROADCAST for UDP socket failed for interface ")+std::string(ifap->ifa_name), LL_ERROR);
						closesocket(udpsock);
						continue;
					}

					#if defined(__FreeBSD__)
					int optval=1;
					if(setsockopt(udpsock, IPPROTO_IP, IP_ONESBCAST, &optval, sizeof(int))==-1)
					{
						Server->Log(std::string("Error setting IP_ONESBCAST for interface " )+std::string(ifap->ifa_name), LL_ERROR);
					}
					#endif

					udpsocks.push_back(udpsock);
				}
			}
			freeifaddrs(ifap);
		}
		else
		{
			Server->Log("Getting interface ips failed", LL_ERROR);
		}
#else
		char hostname[MAX_PATH];
        struct    hostent* h;

        _i32 rc=gethostname(hostname, MAX_PATH);
        if(rc==SOCKET_ERROR)
		{
			Server->Log("Error getting Hostname", LL_ERROR);
            return;
		}
		std::vector<_u32> addresses;

        if(NULL != (h = gethostbyname(hostname)))
        {
			if(h->h_addrtype!=AF_INET)
			{
				Server->Log("Hostname hostent is not AF_INET (ipv4)", LL_ERROR);
			}
			else
			{
				for(_u32 x = 0; (h->h_addr_list[x]); x++)
				{
					SOCKET udpsock=socket(AF_INET,SOCK_DGRAM,0);	

					int optval=1;
					int rc=setsockopt(udpsock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(int));
					if(rc==SOCKET_ERROR)
					{
						Server->Log("Failed setting SO_REUSEADDR in FileClient", LL_ERROR);
					}

					sockaddr_in source_addr;
					memset(&source_addr, 0, sizeof(source_addr));
					source_addr.sin_family = AF_INET;
					source_addr.sin_addr.s_addr = *((_u32*)h->h_addr_list[x]);
					source_addr.sin_port = htons(UDP_SOURCE_PORT);

					rc = bind(udpsock, (struct sockaddr *)&source_addr, sizeof(source_addr));
					if(rc<0)
					{
						Server->Log("Binding UDP socket failed", LL_ERROR);
					}

					setSockP(udpsock);

					BOOL val=TRUE;
					rc=setsockopt(udpsock, SOL_SOCKET, SO_BROADCAST, (char*)&val, sizeof(BOOL) );      
					if(rc<0)
					{
						Server->Log("Failed setting SO_BROADCAST in FileClient", LL_ERROR);
					}

					udpsocks.push_back(udpsock);
				}
			}				
        }
#endif
	}

	socket_open=false;
	stack.setAddChecksum(internet_connection);
}

FileClient::~FileClient(void)
{
	if(socket_open && tcpsock!=NULL)
	{
		Server->destroy(tcpsock);
	}
	for(size_t i=0;i<udpsocks.size();++i)
	{
		closesocket(udpsocks[i]);
	}
}

std::vector<sockaddr_in> FileClient::getServers(void)
{
        return servers;
}

std::vector<std::wstring> FileClient::getServerNames(void)
{
        return servernames;
}

std::vector<sockaddr_in> FileClient::getWrongVersionServers(void)
{
        return wvservers;
}

_u32 FileClient::getLocalIP(void)
{
        return local_ip;
}

_u32 FileClient::GetServers(bool start, const std::vector<in_addr> &addr_hints)
{
        if(start==true)
        {
			max_version=0;
#if defined(_WIN32)

			SOCKET udpsock;
			if(udpsocks.empty())
			{
				Server->Log("No udp socket present", LL_ERROR);
				return ERR_ERROR;
			}

			udpsock=udpsocks[0];

            //get local ip address
            char hostname[MAX_PATH];
            struct    hostent* h;
            _u32     address;

            _i32 rc=gethostname(hostname, MAX_PATH);
            if(rc==SOCKET_ERROR)
                    return 0;

				std::vector<_u32> addresses;

            if(NULL != (h = gethostbyname(hostname)))
            {
				for(_u32 x = 0; (h->h_addr_list[x]); x++)
				{
			//		  Server->Log("Found address for hostname", LL_DEBUG);
						((uchar*)(&address))[0] = h->h_addr_list[x][0];
						((uchar*)(&address))[1] = h->h_addr_list[x][1];
						((uchar*)(&address))[2] = h->h_addr_list[x][2];
						((uchar*)(&address))[3] = h->h_addr_list[x][3];
						((uchar*)(&address))[3]=255;
						addresses.push_back(address);
						local_ip=address;
				}
            }


			sockaddr_in addr_udp;
			addr_udp.sin_family=AF_INET;
			addr_udp.sin_port=htons(UDP_PORT);
			addr_udp.sin_addr.s_addr=INADDR_BROADCAST;
                
			char ch=ID_PING;
			sendto(udpsock, &ch, 1, 0, (sockaddr*)&addr_udp, sizeof(sockaddr_in) );

			for(size_t i=0;i<addresses.size();++i)
			{
					char *ip=(char*)&addresses[i];
					//Server->Log("Sending broadcast to "+nconvert((unsigned char)ip[0])+"."+nconvert((unsigned char)ip[1])+"."+nconvert((unsigned char)ip[2])+"."+nconvert((unsigned char)ip[3]), LL_DEBUG);
					addr_udp.sin_addr.s_addr=addresses[i];
					sendto(udpsock, &ch, 1, 0, (sockaddr*)&addr_udp, sizeof(sockaddr_in) );
			}


			for(size_t i=0;i<addr_hints.size();++i)
			{
				addr_udp.sin_addr.s_addr=addr_hints[i].s_addr;
				sendto(udpsock, &ch, 1, 0, (sockaddr*)&addr_udp, sizeof(sockaddr_in) );
			}
				
#else //Linux		

			for(size_t i=0;i<udpsocks.size();++i)
			{
				sockaddr_in addr_udp;
				addr_udp.sin_family=AF_INET;
				addr_udp.sin_port=htons(UDP_PORT);
#ifdef __FreeBSD__
				addr_udp.sin_addr.s_addr=broadcast_addrs[i].sin_addr.s_addr;
#else
				addr_udp.sin_addr.s_addr=INADDR_BROADCAST;
#endif
				memset(addr_udp.sin_zero,0, sizeof(addr_udp.sin_zero));

				char ch=ID_PING;
				int rc=sendto(udpsocks[0], &ch, 1, 0, (sockaddr*)&addr_udp, sizeof(sockaddr_in));
				if(rc==-1)
				{
					Server->Log("Sending broadcast failed!", LL_ERROR);
				}
			}

			if(!addr_hints.empty())
			{
				for(size_t i=0;i<udpsocks.size();++i)
				{
					int broadcast=0;
					if(setsockopt(udpsocks[i], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(int))==-1)
					{
						Server->Log("Error setting socket to not broadcast", LL_ERROR);
					}

					#if defined(__FreeBSD__)
					int optval=0;
					if(setsockopt(udpsocks[i], IPPROTO_IP, IP_ONESBCAST, &optval, sizeof(int))==-1)
					{
						Server->Log(std::string("Error setting IP_ONESBCAST" ), LL_ERROR);
					}
					#endif

					for(size_t j=0;j<addr_hints.size();++j)
					{
						char ch=ID_PING;
						addr_udp.sin_addr.s_addr=addr_hints[j].s_addr;
						sendto(udpsocks[i], &ch, 1, 0, (sockaddr*)&addr_udp, sizeof(sockaddr_in) );
					}

					broadcast=1;
					if(setsockopt(udpsocks[i], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(int))==-1)
					{
						Server->Log("Error setting socket to broadcast", LL_ERROR);
					}

					#if defined(__FreeBSD__)
					optval=1;
					if(setsockopt(udpsocks[i], IPPROTO_IP, IP_ONESBCAST, &optval, sizeof(int))==-1)
					{
						Server->Log(std::string("Error setting IP_ONESBCAST" ), LL_ERROR);
					}
					#endif
				}
			}
#endif

			starttime=Server->getTimeMS();

			servers.clear();
			servernames.clear();
			wvservers.clear(); 


			return ERR_CONTINUE;
        }
        else
        {
#ifdef _WIN32
			fd_set fdset;
			FD_ZERO(&fdset);

			SOCKET max_socket;

			if(!udpsocks.empty())
			{
				max_socket=udpsocks[0];
			}

			for(size_t i=0;i<udpsocks.size();++i)
			{
				FD_SET(udpsocks[i], &fdset);
				max_socket=(std::max)(max_socket, udpsocks[i]);
			}

			timeval lon;
			lon.tv_sec=0;
			lon.tv_usec=1000*1000;
			_i32 rc = select((int)max_socket+1, &fdset, 0, 0, &lon);
#else
			std::vector<pollfd> conn;
			conn.resize(udpsocks.size());
			for(size_t i=0;i<udpsocks.size();++i)
			{
				conn[i].fd=udpsocks[i];
				conn[i].events=POLLIN;
				conn[i].revents=0;
			}
			int rc = poll(&conn[0], conn.size(), 1000);
#endif
        	if(rc>0)
	        {

				for(size_t i=0;i<udpsocks.size();++i)
				{
#ifdef _WIN32
					if(FD_ISSET(udpsocks[i], &fdset))
#else
					if(conn[i].revents!=0)
#endif
					{
						socklen_t addrsize=sizeof(sockaddr_in);
						sockaddr_in sender;
						_i32 err = recvfrom(udpsocks[i], buffer, BUFFERSIZE_UDP, 0, (sockaddr*)&sender, &addrsize);
						if(err==SOCKET_ERROR)
						{
							continue;
        				}
						if(err>2&&buffer[0]==ID_PONG)
						{
							int version=(unsigned char)buffer[1];
							if(version==VERSION)
							{
								servers.push_back(sender);

								std::string sn;
								sn.resize(err-2);
								memcpy((char*)sn.c_str(), &buffer[2], err-2);

								servernames.push_back(Server->ConvertToUnicode(sn));
							}
							else
							{
								wvservers.push_back(sender);
							}
                                
							if( version>max_version )
							{
								max_version=version;
							}
						}
					}
				}
			}


            if(Server->getTimeMS()-starttime>DISCOVERY_TIMEOUT)
            {
                    return ERR_TIMEOUT;
            }
            else
                    return ERR_CONTINUE;

        }

        
}

int FileClient::getMaxVersion(void)
{
        return max_version;        
}

_u32 FileClient::Connect(sockaddr_in *addr)
{
	if( socket_open==true )
    {
            Server->destroy(tcpsock);
    }

	tcpsock=Server->ConnectStream(inet_ntoa(addr->sin_addr), TCP_PORT, 10000);

	if(tcpsock!=NULL)
	{
		socket_open=true;

		for(size_t i=0;i<throttlers.size();++i)
		{
			tcpsock->addThrottler(throttlers[i]);
		}
	}

	server_addr=*addr;

    if(tcpsock==NULL)
		return ERR_ERROR;
	else
		return ERR_CONNECTED;
}    

void FileClient::addThrottler(IPipeThrottler *throttler)
{
	throttlers.push_back(throttler);
	if(tcpsock!=NULL)
	{
		tcpsock->addThrottler(throttler);
	}
}

_u32 FileClient::Connect(IPipe *cp)
{
	if( socket_open==true )
    {
            Server->destroy(tcpsock);
    }

	tcpsock=cp;

	if(tcpsock!=NULL)
	{
		socket_open=true;
	}

	if(tcpsock==NULL)
		return ERR_ERROR;
	else
		return ERR_CONNECTED;
}

_u32 FileClient::GetGameList(void)
{

		CWData data;
		data.addUChar(ID_GET_GAMELIST);
		data.addString( server_identity );

		stack.reset();
		stack.Send(tcpsock, data.getDataPtr(), data.getDataSize() );

		starttime=Server->getTimeMS();
		num_games_res=false;
		num_games_get=0;

		while(true)
		{
			size_t rc=tcpsock->Read(buffer, BUFFERSIZE_UDP, 10000);

			if(rc==0)
				return ERR_ERROR;

			starttime=Server->getTimeMS();

            stack.AddData(buffer, rc);
                                
			char *packet;
			size_t packetsize;
			while( (packet=stack.getPacket(&packetsize) )!=NULL )
			{
					if( packetsize>1 && packet[0]==ID_GAMELIST && num_games_res==false)
					{
							CRData data(&packet[1], packetsize);

							if( !data.getUInt(&num_games) )
							{
									delete [] packet;
									return ERR_ERROR;
							}

							res_name=true;
							num_games_res=true;
							num_games_get=0;

							if( num_games==0 )
							{
									delete [] packet;
									return ERR_SUCCESS;
							}
					}
					else if( num_games_res==true )
					{
							if( res_name==true )
							{
									games.push_back(&packet[0]);
									res_name=false;
							}
							else
							{
									writestring(packet, (unsigned int)packetsize, str_tmpdir+conv_filename(mServerName+"-"+games[ games.size()-1]) );
									res_name=true;
									num_games_get++;
									if( num_games_get==num_games )
									{
											delete [] packet;
											return ERR_SUCCESS;
									}
							}
					}
					delete []packet;
			}
               
			if( Server->getTimeMS()-starttime>10000)
            {
                    return ERR_TIMEOUT;
            }
        }
}

bool FileClient::ListDownloaded(void)
{
        if( num_games_get==num_games )
        {
                return true;
        }
        else
                return false;
}

std::vector<std::string> FileClient::getGameList(void)
{
        return games;
}

void FileClient::setServerName(std::string pName)
{
        mServerName=pName;
}

std::string FileClient::getServerName(void)
{
        return mServerName;
}

bool FileClient::isConnected(void)
{
        return connected;
}

bool FileClient::Reconnect(void)
{
	transferred_bytes+=tcpsock->getTransferedBytes();
	Server->destroy(tcpsock);
	connect_starttime=Server->getTimeMS();

	while(Server->getTimeMS()-connect_starttime<reconnection_timeout)
	{
		if(reconnection_callback==NULL)
		{
			tcpsock=Server->ConnectStream(inet_ntoa(server_addr.sin_addr), TCP_PORT, 10000);
		}
		else
		{
			tcpsock=reconnection_callback->new_fileclient_connection();
		}
		if(tcpsock!=NULL)
		{
			for(size_t i=0;i<throttlers.size();++i)
			{
				tcpsock->addThrottler(throttlers[i]);
			}
			Server->Log("Reconnected successfully,", LL_DEBUG);
			socket_open=true;
			return true;
		}
		else
		{
			Server->wait(1000);
		}
	}
	Server->Log("Reconnecting failed.", LL_DEBUG);
	socket_open=false;
	return false;
}

 _u32 FileClient::GetFile(std::string remotefn, IFile *file, bool hashed)
{
	if(tcpsock==NULL)
		return ERR_ERROR;

	int tries=5000;

	if(!hashed && protocol_version>1)
	{
		//Disable hashed transfer (protocol_version>1)
		protocol_version=1;
	}

    CWData data;
    data.addUChar( protocol_version>1?ID_GET_FILE_RESUME_HASH:ID_GET_FILE );
    data.addString( remotefn );
	data.addString( server_identity );

    stack.Send( tcpsock, data.getDataPtr(), data.getDataSize() );

	_u64 filesize=0;
	_u64 received=0;
	_u64 next_checkpoint=c_checkpoint_dist;
	_u64 last_checkpoint=0;
	bool firstpacket=true;

	if(file==NULL)
		return ERR_ERROR;

    starttime=Server->getTimeMS();

	char buf[BUFFERSIZE];
	int state=0;
	char hash_buf[16];
	_u32 hash_r;
	MD5 hash_func;


	while(true)
	{        
		size_t rc=tcpsock->Read(buf, BUFFERSIZE, 120000);

        if( rc==0 )
        {
			Server->Log("Server timeout (2) in FileClient", LL_DEBUG);
			bool b=Reconnect();
			--tries;
			if(!b || tries<=0 )
			{
				Server->Log("FileClient: ERR_TIMEOUT", LL_INFO);
				return ERR_TIMEOUT;
			}
			else
			{
				CWData data;
				data.addUChar( protocol_version>1?ID_GET_FILE_RESUME_HASH:(protocol_version>0?ID_GET_FILE_RESUME:ID_GET_FILE) );
				data.addString( remotefn );
				data.addString( server_identity );

				if( protocol_version>1 )
				{
					received=last_checkpoint;
				}

				if( firstpacket==false )
					data.addInt64( received ); 

				file->Seek(received);

				rc=stack.Send( tcpsock, data.getDataPtr(), data.getDataSize() );
				if(rc==0)
				{
					Server->Log("FileClient: Error sending request", LL_INFO);
				}
				starttime=Server->getTimeMS();

				if(protocol_version>0)
					firstpacket=true;

				hash_func.init();
				state=0;
			}
		}
        else
        {
			starttime=Server->getTimeMS();

			_u32 off=0;
			uchar PID=buf[0];
                        
            if( firstpacket==true)
            {
                    if(PID==ID_COULDNT_OPEN)
                    {
                        return ERR_FILE_DOESNT_EXIST;
                    }
					else if(PID==ID_BASE_DIR_LOST)
					{
						return ERR_BASE_DIR_LOST;
					}
                    else if(PID==ID_FILESIZE && rc >= 1+sizeof(_u64))
                    {
                            memcpy(&filesize, buf+1, sizeof(_u64) );
                            off=1+sizeof(_u64);

                            if( filesize==0 )
                            {
                                    return ERR_SUCCESS;
                            }

							if(protocol_version>1)
							{
								if(filesize<next_checkpoint)
									next_checkpoint=filesize;
							}
							else
							{
								next_checkpoint=filesize;
							}
                    }
                    firstpacket=false;
            }

			if( state==1 && (_u32) rc > off )
			{
				_u32 tc=(std::min)((_u32)rc-off, hash_r);
				memcpy(&hash_buf[16-hash_r], &buf[off],  tc);
				off+=tc;
				hash_r-=tc;

				if(hash_r==0)
				{
					hash_func.finalize();
					if(memcmp(hash_func.raw_digest_int(), hash_buf, 16)!=0)
					{
						Server->Log("Error while downloading file: hash wrong -1", LL_ERROR);
						Reconnect();
						return ERR_HASH;
					}
					hash_func.init();
					state=0;
				}

				if(received >= filesize && state==0)
				{
					return ERR_SUCCESS;
				}
			}

            if( state==0 && (_u32) rc > off )
            {
				_u32 written=off;
				_u64 write_remaining=next_checkpoint-received;
				_u32 hash_off=0;
				bool c=true;
				while(c)
				{
					c=false;
					while(written<rc)
					{
						_u32 tw=(_u32)rc-written;
						if((_u64)tw>write_remaining)
							tw=(_u32)write_remaining;

						_u32 cw=file->Write(&buf[written], tw);
						hash_func.update((unsigned char*)&buf[written], cw);
						written+=cw;
						write_remaining-=cw;
						received+=cw;
						if(write_remaining==0)
							break;
						if(written<rc)
						{
							if(nofreespace_callback!=NULL
								&& !nofreespace_callback->handle_not_enough_space(file->getFilenameW()) )
							{
								Server->Log("Error while writing to file. No free space -2", LL_ERROR);
								Reconnect();
								return ERR_ERROR;
							}

							Server->Log("Failed to write to file... waiting...", LL_WARNING);
							Server->wait(10000);
							starttime=Server->getTimeMS();
						}
					}

					if(write_remaining==0 && protocol_version>1) 
					{
						if(next_checkpoint<filesize)
						{
							last_checkpoint=next_checkpoint;
						}
						next_checkpoint+=c_checkpoint_dist;
						if(next_checkpoint>filesize)
							next_checkpoint=filesize;

						hash_r=(_u32)rc-written;
						if(hash_r>0)
						{
							memcpy(hash_buf, &buf[written], (std::min)(hash_r, (_u32)16));

							if(hash_r>16)
							{
								hash_r=16;
								c=true;
								write_remaining=next_checkpoint-received;
								written+=16;
							}
						}
						else
						{
							int asfsf=3;
						}

						hash_off+=hash_r;

						if(hash_r<16)
						{
							hash_r=16-hash_r;
							state=1;
						}
						else
						{
							hash_func.finalize();
							if(memcmp(hash_func.raw_digest_int(), hash_buf, 16)!=0)
							{
								Server->Log("Error while downloading file: hash wrong -2", LL_ERROR);
								Reconnect();
								return ERR_HASH;
							}
							hash_func.init();
						}
					}
				}

				if( received >= filesize && state==0)
                {
					return ERR_SUCCESS;
				}
            }
		}
            
	    if( Server->getTimeMS()-starttime > SERVER_TIMEOUT )
		{
				Server->Log("Server timeout in FileClient. Trying to reconnect...", LL_INFO);
				bool b=Reconnect();
				--tries;
				if(!b || tries<=0 )
				{
					Server->Log("FileClient: ERR_TIMEOUT", LL_INFO);
					return ERR_TIMEOUT;
				}
				else
				{
					CWData data;
					data.addUChar( protocol_version>1?ID_GET_FILE_RESUME_HASH:(protocol_version>0?ID_GET_FILE_RESUME:ID_GET_FILE) );
					data.addString( remotefn );
					data.addString( server_identity );

					if( protocol_version>1 )
					{
						received=last_checkpoint;
					}

					if( firstpacket==false )
						data.addInt64( received ); 

					file->Seek(received);

					stack.Send( tcpsock, data.getDataPtr(), data.getDataSize() );
					starttime=Server->getTimeMS();

					if(protocol_version>0)
						firstpacket=true;

					hash_func.init();
					state=0;
				}
		}
	}
}
        
_i64 FileClient::getTransferredBytes(void)
{
	if(tcpsock!=NULL)
	{
		transferred_bytes+=tcpsock->getTransferedBytes();
		tcpsock->resetTransferedBytes();
	}
	return transferred_bytes;
}

std::string FileClient::getErrorString(_u32 ec)
{
#define DEFEC(x) case ERR_##x : return #x;
	switch(ec)
	{
	DEFEC(CONTINUE);
	DEFEC(SUCCESS);
	DEFEC(TIMEOUT);
	DEFEC(FILE_DOESNT_EXIST);
	DEFEC(SOCKET_ERROR);
	DEFEC(CONNECTED);
	DEFEC(ERROR);
	DEFEC(BASE_DIR_LOST);
	DEFEC(HASH);
	DEFEC(INT_ERROR);
	DEFEC(CONN_LOST);
	}
#undef DEFEC
	return "";
}

void FileClient::setReconnectionTimeout(unsigned int t)
{
	reconnection_timeout=t;
}


