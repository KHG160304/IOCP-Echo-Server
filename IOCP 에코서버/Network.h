#pragma once
#ifndef __NETWORK_H__
#define	__NETWORK_H__
#include "SerializationBuffer.h"

#define WINAPI	__stdcall

typedef unsigned short		WORD;
typedef	void*				LPVOID;
typedef unsigned long long	UINT64;
typedef UINT64 SESSIONID;
typedef WORD	NETWORK_HEADER;

void RequestExitNetworkLibThread(void);
bool InitNetworkLib(WORD port);
bool InitNetworkIOThread(void);
void SetOnRecvEvent(void (*_OnRecv)(SESSIONID sessionID, SerializationBuffer& packet));
void SendPacket(SESSIONID sessionID, SerializationBuffer& sendPacket);
unsigned int WINAPI AcceptThread(LPVOID args);
unsigned int WINAPI	IOCPWorkerThread(LPVOID args);

#endif // !__NETWORK_H__