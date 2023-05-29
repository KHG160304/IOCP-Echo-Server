#pragma comment(lib, "ws2_32")
#include "Network.h"
#include "Log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include "RingBuffer.h"
#include "SerializationBuffer.h"
#include <map>
#include <locale.h>

#define EXIT_THREAD_CODE	

struct WsaOverlappedEX
{
	WSAOVERLAPPED overlapped;
	void* ptrSession;
};

struct Session
{
	SOCKET socket;
	SOCKADDR_IN clientAddr;
	SESSIONID sessionID;// 사용자가 socket에 직접 접근하지 못하게 하기 위해서,
	WsaOverlappedEX sendOverlapped;
	WsaOverlappedEX recvOverlapped;
	RingBuffer sendRingBuffer;
	RingBuffer recvRingBuffer;
	int overlappedIOCnt;
	int waitSend;
	char sendBuffer[11];

	Session(SOCKET sock, SOCKADDR_IN* addr, SESSIONID id)
		: socket(sock)
		, clientAddr(*addr)
		, sessionID(id)
		, sendRingBuffer(1048576)
		, recvRingBuffer(1048576)
		, overlappedIOCnt(0)
		, waitSend(false)
		, sendBuffer { 0, }
	{
		sendOverlapped.ptrSession = this;
		recvOverlapped.ptrSession = this;
	}
};

SESSIONID gSessionID = 0;
SOCKET gListenSock;
SOCKADDR_IN gServerAddr;
SYSTEM_INFO gSystemInfo;
int numberOfConcurrentThread;
int numberOfCreateIOCPWorkerThread;
HANDLE hIOCP;
HANDLE hThreadAccept;
HANDLE* hThreadIOCPWorker;
std::map<SESSIONID, Session*> sessionMap;
SRWLOCK srwlock = RTL_SRWLOCK_INIT;
void (*OnRecv)(SESSIONID sessionID, SerializationBuffer& packet) = nullptr;

void ReleaseSession(Session* ptrSession);
void PostRecv(Session* ptrSession);
void PostSend(Session* ptrSession);

#define _WSALog(logLvl, wsaErrorCode, fmt, ...)									\
do {																			\
	if (wsaErrorCode != 10054)													\
	{																			\
		_Log(logLvl, "[WSAError Code:%d] " fmt, wsaErrorCode, ##__VA_ARGS__);	\
	}																			\
} while (0)																		\

void ReleaseServerResource()
{
	_Log(dfLOG_LEVEL_SYSTEM, "서버 리소스 해제 시작");
	Session* ptrSession;
	std::map<SESSIONID, Session*>::iterator iter = sessionMap.begin();
	while (iter != sessionMap.end())
	{
		ptrSession = iter->second;
		closesocket(ptrSession->socket);
		delete ptrSession;
		++iter;
	}
	sessionMap.clear();
	_Log(dfLOG_LEVEL_SYSTEM, "서버 리소스 해제 완료");
}

void RequestExitNetworkLibThread(void)
{
	_Log(dfLOG_LEVEL_SYSTEM, "서버 종료 처리 시작");
	closesocket(gListenSock); //Accept Thread 종료를 위해서 리슨 소켓 종료;
	for (int i = 0; i < numberOfCreateIOCPWorkerThread; ++i)
	{
		PostQueuedCompletionStatus(hIOCP, 0, 0, nullptr);
	}

	HANDLE* hThreadAll = new HANDLE[numberOfCreateIOCPWorkerThread + 1];
	hThreadAll[0] = hThreadAccept;
	for (int i = 0; i < numberOfCreateIOCPWorkerThread; ++i)
	{
		hThreadAll[i + 1] = hThreadIOCPWorker[i];
	}
	if (WaitForMultipleObjects(numberOfCreateIOCPWorkerThread + 1, hThreadAll, true, INFINITE) == WAIT_FAILED)
	{
		printf("RequestExitProcess - WaitForMultipleObjects error code: %d\n", GetLastError());
	}
	_Log(dfLOG_LEVEL_SYSTEM, "스레드 종료 완료");
	ReleaseServerResource();
	_Log(dfLOG_LEVEL_SYSTEM, "서버 종료 처리 완료");
}

bool InitNetworkLib(WORD port)
{
	_wsetlocale(LC_ALL, L"korean");

	WSADATA wsa;
	WCHAR wstrIp[16];
	if (OnRecv == nullptr)
	{
		_Log(dfLOG_LEVEL_SYSTEM, L"OnRecv 이벤트를 등록하세요");
		return false;
	}

	if (WSAStartup(MAKEWORD(2, 2), &wsa) != NO_ERROR)
	{
		_Log(dfLOG_LEVEL_SYSTEM, L"WSAStartup() errorcode: %d", WSAGetLastError());
		return false;
	}

	gListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (gListenSock == INVALID_SOCKET)
	{
		_Log(dfLOG_LEVEL_SYSTEM, L"listen socket() errorcode: %d", WSAGetLastError());
		return false;
	}

	linger optLinger;
	optLinger.l_onoff = 1;
	optLinger.l_linger = 0;
	setsockopt(gListenSock, SOL_SOCKET, SO_LINGER, (char*)&optLinger, sizeof(linger));

	DWORD soSndBUfSize = 0;
	setsockopt(gListenSock, SOL_SOCKET, SO_SNDBUF, (char*)&soSndBUfSize, sizeof(DWORD));

	gServerAddr.sin_family = AF_INET;
	gServerAddr.sin_port = htons(port);
	gServerAddr.sin_addr.s_addr = INADDR_ANY;
	if (bind(gListenSock, (SOCKADDR*)&gServerAddr, sizeof(gServerAddr)) == SOCKET_ERROR)
	{
		_Log(dfLOG_LEVEL_SYSTEM, L"listen socket() errorcode: %d", WSAGetLastError());
		goto InitError;
	}

	if (listen(gListenSock, SOMAXCONN) == SOCKET_ERROR)
	{
		_Log(dfLOG_LEVEL_SYSTEM, L"listen() errorcode: %d", WSAGetLastError());
		goto InitError;
	}

	if (InitNetworkIOThread() == false)
	{
		goto InitError;
	}

	InetNtop(AF_INET, &gServerAddr.sin_addr, wstrIp, 16);
	_Log(dfLOG_LEVEL_SYSTEM, L"Server Init OK [%s/%d]", wstrIp, ntohs(gServerAddr.sin_port));

	return true;

InitError:
	closesocket(gListenSock);
	return false;
}

bool InitNetworkIOThread(void)
{
	_int64 idx;
	GetSystemInfo(&gSystemInfo);
	numberOfConcurrentThread = gSystemInfo.dwNumberOfProcessors / 2;
	hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, numberOfConcurrentThread);
	if (hIOCP == nullptr)
	{
		_Log(dfLOG_LEVEL_SYSTEM, "IOCP HANDLE CreateIoCompletionPort() error code: %d", GetLastError());
		return false;
	}
	
	numberOfCreateIOCPWorkerThread = numberOfConcurrentThread - 1;
	hThreadIOCPWorker = new HANDLE[numberOfCreateIOCPWorkerThread];
	for (idx = 0; idx < numberOfCreateIOCPWorkerThread; ++idx)
	{
		hThreadIOCPWorker[idx] = (HANDLE)_beginthreadex(nullptr, 0, IOCPWorkerThread, (void*)idx, 0, nullptr);
		if (hThreadIOCPWorker == nullptr)
		{
			closesocket(gListenSock);
			_Log(dfLOG_LEVEL_SYSTEM, "_beginthreadex(IOCPWorkerThread) error code: %d", GetLastError());
			for (DWORD j = 0; j < idx; ++j)
			{
				PostQueuedCompletionStatus(hIOCP, 0, 0, nullptr);
			}
			return false;
		}
	}

	hThreadAccept = (HANDLE)_beginthreadex(nullptr, 0, AcceptThread, nullptr, 0, nullptr);
	if (hThreadAccept == nullptr)
	{
		closesocket(gListenSock);
		_Log(dfLOG_LEVEL_SYSTEM, "_beginthreadex(AcceptThread) error code: %d", GetLastError());
		for (idx = 0; idx < numberOfCreateIOCPWorkerThread; ++idx)
		{
			PostQueuedCompletionStatus(hIOCP, 0, 0, nullptr);
		}
		return false;
	}

	return true;
}

void SetOnRecvEvent(void (*_OnRecv)(SESSIONID sessionID, SerializationBuffer& packet))
{
	OnRecv = _OnRecv;
}

void ReleaseSession(Session* ptrSession)
{
	//세션 삭제
	closesocket(ptrSession->socket);
	AcquireSRWLockExclusive(&srwlock);
	sessionMap.erase(ptrSession->sessionID);
	delete ptrSession;
	ReleaseSRWLockExclusive(&srwlock);
}

unsigned WINAPI AcceptThread(LPVOID args)
{
	int retval;
	int errorCode;
	int acceptErrorCode;
	DWORD flags = 0;
	SOCKET clientSock;
	SOCKADDR_IN clientAddr;
	int addrlen = sizeof(clientAddr);
	_Log(dfLOG_LEVEL_SYSTEM, "AcceptThread");
	for (;;)
	{
		clientSock = accept(gListenSock, (SOCKADDR*)&clientAddr, &addrlen);
		if (clientSock == INVALID_SOCKET)
		{
			acceptErrorCode = WSAGetLastError();
			if (acceptErrorCode == WSAENOTSOCK || acceptErrorCode == WSAEINTR)
			{
				_Log(dfLOG_LEVEL_SYSTEM, "AcceptThread Exit");
				return 0;
			}

			_Log(dfLOG_LEVEL_SYSTEM, "accept() error code: %d", acceptErrorCode);
			closesocket(clientSock);
			continue;
		}

		CreateIoCompletionPort((HANDLE)clientSock, hIOCP, gSessionID, 0);
		AcquireSRWLockExclusive(&srwlock);
		Session* ptrNewSession = new Session(clientSock, &clientAddr, gSessionID);
		sessionMap.insert({ gSessionID, ptrNewSession });
		ReleaseSRWLockExclusive(&srwlock);
		gSessionID += 1;

		WSABUF recvWsaBuf;
		recvWsaBuf.buf = ptrNewSession->recvRingBuffer.GetRearBufferPtr();
		recvWsaBuf.len = ptrNewSession->recvRingBuffer.GetDirectEnqueueSize();
		ZeroMemory(&ptrNewSession->recvOverlapped, sizeof(WSAOVERLAPPED));
		InterlockedIncrement((LONG*)&(ptrNewSession->overlappedIOCnt));
		retval = WSARecv(ptrNewSession->socket, &recvWsaBuf, 1, nullptr, &flags, (LPWSAOVERLAPPED)&ptrNewSession->recvOverlapped, nullptr);
		if (retval == SOCKET_ERROR)
		{
			errorCode = WSAGetLastError();
			if (errorCode != WSA_IO_PENDING)
			{
				ReleaseSession(ptrNewSession);
			}
		}
	}
}

unsigned WINAPI IOCPWorkerThread(LPVOID args)
{
	DWORD numberOfBytesTransferred;
	SESSIONID sessionID;
	WsaOverlappedEX* overlapped;
	Session* ptrSession;
	RingBuffer* ptrRecvRingBuffer;
	RingBuffer* ptrSendRingBuffer;
	SerializationBuffer recvPacket;
	NETWORK_HEADER packetHeader;
	int recvSize;
	int retvalGQCS;
	_Log(dfLOG_LEVEL_SYSTEM, "[No.%lld] IOCPWorkerThread", (_int64)args);
	for (;;)
	{
		numberOfBytesTransferred = 0;
		sessionID = 0;
		overlapped = 0;
		retvalGQCS = GetQueuedCompletionStatus(hIOCP, &numberOfBytesTransferred, &sessionID, (LPOVERLAPPED*)&overlapped, INFINITE);
		if (numberOfBytesTransferred == 0 && sessionID == 0 && overlapped == nullptr)
		{
			_Log(dfLOG_LEVEL_SYSTEM, "[No.%lld] IOCPWorkerThread Exit", (_int64)args);
			return 0;
		}

		ptrSession = (Session*)overlapped->ptrSession;
		if (retvalGQCS == TRUE && numberOfBytesTransferred != 0)
		{
			if (&(ptrSession->recvOverlapped) == overlapped)
			{
				ptrRecvRingBuffer = &ptrSession->recvRingBuffer;
				ptrRecvRingBuffer->MoveRear(numberOfBytesTransferred);
				
				recvPacket.ClearBuffer();
				for (;;)
				{
					recvSize = ptrRecvRingBuffer->GetUseSize();
					if (recvSize < sizeof(NETWORK_HEADER))
					{
						break;
					}
					ptrRecvRingBuffer->Peek((char*)&packetHeader, sizeof(NETWORK_HEADER));
					if (recvSize < sizeof(NETWORK_HEADER) + packetHeader)
					{
						break;
					}

					ptrRecvRingBuffer->Peek((char*)&ptrSession->sendBuffer, sizeof(NETWORK_HEADER) + packetHeader);
					ptrRecvRingBuffer->MoveFront(sizeof(NETWORK_HEADER));
					recvPacket.MoveRear(ptrRecvRingBuffer->Dequeue(recvPacket.GetFrontBufferPtr(), packetHeader));

					OnRecv(sessionID, recvPacket);
					//recvSize -= (sizeof(NETWORK_HEADER) + packetHeader);
				}
				
				PostRecv(ptrSession);

				//ptrSendRingBuffer->Enqueue(ptrRecvRingBuffer->GetFrontBufferPtr(), numberOfBytesTransferred);
				//ptrRecvRingBuffer->MoveFront(numberOfBytesTransferred);

				/*wsaBuf[0].buf = ptrSendRingBuffer->GetFrontBufferPtr();
				wsaBuf[0].len = ptrSendRingBuffer->GetDirectDequeueSize();
				if ((restSendBufferLen = ptrSession->sendRingBuffer.GetUseSize() - wsaBuf[0].len) > 0)
				{
					wsaBuf[1].buf = ptrSendRingBuffer->GetInternalBufferPtr();
					wsaBuf[1].len = restSendBufferLen;
					++sendWsaBufCnt;
				}
				ZeroMemory(&ptrSession->sendOverlapped, sizeof(WSAOVERLAPPED));
				InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
				int retvalWSASend = WSASend(ptrSession->socket, wsaBuf, sendWsaBufCnt, nullptr, 0, (LPWSAOVERLAPPED)&ptrSession->sendOverlapped, nullptr);
				if (retvalWSASend == SOCKET_ERROR
					&& (wsaSendErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
				{
					_WSALog(dfLOG_LEVEL_SYSTEM, wsaSendErrorCode, "WSASend Error");
					InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt);
				}
				else
				{
					flag = 0;
					wsaBuf[0].buf = ptrRecvRingBuffer->GetRearBufferPtr();
					wsaBuf[0].len = ptrRecvRingBuffer->GetDirectEnqueueSize();
					if ((restRecvBufferLen = ptrRecvRingBuffer->GetFreeSize() - wsaBuf[0].len) > 0)
					{
						wsaBuf[1].buf = ptrRecvRingBuffer->GetInternalBufferPtr();
						wsaBuf[1].len = restRecvBufferLen;
						++recvWsaBufCnt;
					}
					ZeroMemory(&ptrSession->recvOverlapped, sizeof(WSAOVERLAPPED));
					InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
					if (WSARecv(ptrSession->socket, wsaBuf, recvWsaBufCnt, nullptr, &flag, (LPWSAOVERLAPPED)&ptrSession->recvOverlapped, nullptr) == SOCKET_ERROR
						&& (wsaRecvErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
					{
						_WSALog(dfLOG_LEVEL_SYSTEM, wsaRecvErrorCode, "WSARecv Error");
						InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt);
					}
				}*/
			}
			else if (&(ptrSession->sendOverlapped) == overlapped)
			{
				ptrSendRingBuffer = &ptrSession->sendRingBuffer;
				// 여기서 샌드큐를 감소 시킴
				ptrSendRingBuffer->MoveFront(numberOfBytesTransferred);
				// 인터락 호출 이전의 것들은 모두 캐시 메모리에 반영을 완료 해준다.
				InterlockedExchange((LONG*)&ptrSession->waitSend, false);
				if (ptrSendRingBuffer->GetUseSize() > 0)
				{
					PostSend(ptrSession);
				}
			}
		}

		if (InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt) == 0)
		{
			ReleaseSession(ptrSession);
		}
	}
}

void PostRecv(Session* ptrSession)
{
	RingBuffer* ptrRecvRingBuffer = &ptrSession->recvRingBuffer;
	WSABUF wsabuf[2];
	int wsabufCnt = 1;
	int	restRecvBufferLen;
	DWORD flag = 0;
	int wsaRecvErrorCode;
	wsabuf[0].buf = ptrRecvRingBuffer->GetRearBufferPtr();
	wsabuf[0].len = ptrRecvRingBuffer->GetDirectEnqueueSize();
	if ((restRecvBufferLen = ptrRecvRingBuffer->GetFreeSize() - wsabuf[0].len) > 0)
	{
		wsabuf[1].buf = ptrRecvRingBuffer->GetInternalBufferPtr();
		wsabuf[1].len = restRecvBufferLen;
		++wsabufCnt;
	}
	ZeroMemory(&ptrSession->recvOverlapped, sizeof(WSAOVERLAPPED));
	InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
	if (WSARecv(ptrSession->socket, wsabuf, wsabufCnt, nullptr, &flag, (LPWSAOVERLAPPED)&ptrSession->recvOverlapped, nullptr) == SOCKET_ERROR
		&& (wsaRecvErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
	{
		_WSALog(dfLOG_LEVEL_SYSTEM, wsaRecvErrorCode, "WSARecv Error");
		if (InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt) == 0)
		{
			_Log(dfLOG_LEVEL_SYSTEM, "PostRecv OverlappedIOCnt 0 is occured");
		}
		/*
			릴리즈 코드를 정말로 이안에 포함하는 게 맞는 것인가, 그런 경우가 있는가??
		*/
	}
}

void PostSend(Session* ptrSession)
{
	if (InterlockedExchange((LONG*)&ptrSession->waitSend, true))
	{
		return;
	}

	RingBuffer* ptrSendRingBuffer = &ptrSession->sendRingBuffer;
	static WSABUF wsabuf[2];
	int wsabufCnt = 1;
	int	restSendBufferLen;
	int wsaSendErrorCode;
	char ttsendBuf[11] = { 0, };


	wsabuf[0].buf = ptrSendRingBuffer->GetFrontBufferPtr();
	wsabuf[0].len = ptrSendRingBuffer->GetDirectDequeueSize();
	//memcpy(ttsendBuf, wsabuf[0].buf, wsabuf[0].len);
	if ((restSendBufferLen = ptrSendRingBuffer->GetUseSize() - wsabuf[0].len) > 0)
	{
		wsabuf[1].buf = ptrSendRingBuffer->GetInternalBufferPtr();
		wsabuf[1].len = restSendBufferLen;

		//memcpy(ttsendBuf + wsabuf[0].len, wsabuf[1].buf, wsabuf[1].len);
		++wsabufCnt;
	}
	ptrSendRingBuffer->Peek(ttsendBuf, ptrSendRingBuffer->GetUseSize());

	if (memcmp(ttsendBuf, ptrSession->sendBuffer, 10) != 0)
	{
		printf("from send buffer: %lld, from recv buffer: %lld\n", *((_int64*)(ttsendBuf + 2)), *((_int64*)(ptrSession->sendBuffer + 2)));
	}

	ZeroMemory(&ptrSession->sendOverlapped, sizeof(WSAOVERLAPPED));
	InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
	if (WSASend(ptrSession->socket, wsabuf, wsabufCnt, nullptr, 0, (LPWSAOVERLAPPED)&ptrSession->sendOverlapped, nullptr) == SOCKET_ERROR
		&& (wsaSendErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
	{
		_WSALog(dfLOG_LEVEL_SYSTEM, wsaSendErrorCode, "WSASend Error");
		if (InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt) == 0)
		{
			_Log(dfLOG_LEVEL_SYSTEM, "PostSend OverlappedIOCnt 0 is occured");
			/*
		릴리즈 코드를 정말로 이안에 포함하는 게 맞는 것인가, 그런 경우가 있는가??
		*/
		}
	}

	//printf("from send buffer: %lld, from recv buffer: %lld\n", *((_int64*)(ttsendBuf + 2)), *((_int64*)(ptrSession->sendBuffer + 2)));
}

void SendPacket(SESSIONID sessionID, SerializationBuffer& sendPacket)
{
	Session* ptrSession = sessionMap[sessionID];

	NETWORK_HEADER len = sendPacket.GetUseSize();
	ptrSession->sendRingBuffer.Enqueue((char*)&len, sizeof(NETWORK_HEADER));
	ptrSession->sendRingBuffer.Enqueue(sendPacket.GetFrontBufferPtr(), len);
	sendPacket.MoveFront(len);

	PostSend(ptrSession);
}

