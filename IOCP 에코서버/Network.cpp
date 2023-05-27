#pragma comment(lib, "ws2_32")
#include "Network.h"
#include "Log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include "RingBuffer.h"
#include <map>
#include <locale.h>

#define EXIT_THREAD_CODE	

typedef UINT64 SESSIONID;

struct WsaOverlappedEX
{
	WSAOVERLAPPED overlapped;
	void* ptrSession;
};

struct Session
{
	SOCKET socket;
	SOCKADDR_IN clientAddr;
	SESSIONID sessionID;// ����ڰ� socket�� ���� �������� ���ϰ� �ϱ� ���ؼ�,
	WsaOverlappedEX sendOverlapped;
	WsaOverlappedEX recvOverlapped;
	RingBuffer sendRingBuffer;
	RingBuffer recvRingBuffer;
	int overlappedIOCnt;

	Session(SOCKET sock, SOCKADDR_IN* addr, SESSIONID id)
	: socket(sock)
	, clientAddr(*addr)
	, sessionID(id)
	, sendRingBuffer(1048576)
	, recvRingBuffer(1048576)
	, overlappedIOCnt(0)
	{
		sendOverlapped.ptrSession = this;
		recvOverlapped.ptrSession = this;
	}
};

SESSIONID gSessionID = 0;
SOCKET gListenSock;
SOCKADDR_IN gServerAddr;
SYSTEM_INFO gSystemInfo;
DWORD numberOfConcurrentThread;
DWORD numberOfCreateIOCPWorkerThread;
HANDLE hIOCP;
HANDLE hThreadAccept;
HANDLE* hThreadIOCPWorker;
std::map<SESSIONID, Session*> sessionMap;

void ReleaseServerResource()
{
	_Log(dfLOG_LEVEL_SYSTEM, "���� ���ҽ� ���� ����");
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
	_Log(dfLOG_LEVEL_SYSTEM, "���� ���ҽ� ���� �Ϸ�");
}

void RequestExitNetworkLibThread(void)
{
	_Log(dfLOG_LEVEL_SYSTEM, "���� ���� ó�� ����");
	closesocket(gListenSock); //Accept Thread ���Ḧ ���ؼ� ���� ���� ����;
	for (DWORD i = 0; i < numberOfCreateIOCPWorkerThread; ++i)
	{
		PostQueuedCompletionStatus(hIOCP, 0, 0, nullptr);
	}

	HANDLE* hThreadAll = new HANDLE[numberOfCreateIOCPWorkerThread + 1];
	hThreadAll[0] = hThreadAccept;
	for (DWORD i = 0; i < numberOfCreateIOCPWorkerThread; ++i)
	{
		hThreadAll[i + 1] = hThreadIOCPWorker[i];
	}
	if (WaitForMultipleObjects(numberOfCreateIOCPWorkerThread + 1, hThreadAll, true, INFINITE) == WAIT_FAILED)
	{
		printf("RequestExitProcess - WaitForMultipleObjects error code: %d\n", GetLastError());
	}
	_Log(dfLOG_LEVEL_SYSTEM, "������ ���� �Ϸ�");
	ReleaseServerResource();
	_Log(dfLOG_LEVEL_SYSTEM, "���� ���� ó�� �Ϸ�");
}

bool InitNetworkLib(WORD port)
{
	_wsetlocale(LC_ALL, L"korean");

	WSADATA wsa;
	WCHAR wstrIp[16];
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
	DWORD idx;
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
		Session* ptrNewSession = new Session(clientSock, &clientAddr, gSessionID);
		
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
				closesocket(clientSock);
				delete ptrNewSession;
			}
		}
		sessionMap.insert({ gSessionID, ptrNewSession });
		gSessionID += 1;
	}
}

unsigned WINAPI IOCPWorkerThread(LPVOID args)
{
	DWORD numberOfBytesTransferred = 0;
	SESSIONID sessionID = 0;
	WsaOverlappedEX* overlapped = 0;
	Session* ptrSession;
	int retvalGQCS;
	WSABUF wsaBuf;
	DWORD flag;
	int wsaRecvErrorCode;
	int wsaSendErrorCode;
	_Log(dfLOG_LEVEL_SYSTEM, "[No.%d] IOCPWorkerThread", (int)args);
	for (;;)
	{
		numberOfBytesTransferred = 0;
		sessionID = 0;
		overlapped = 0;
		retvalGQCS = GetQueuedCompletionStatus(hIOCP, &numberOfBytesTransferred, &sessionID, (LPOVERLAPPED*)&overlapped, INFINITE);
		if (numberOfBytesTransferred == 0 && sessionID == 0 && overlapped == nullptr)
		{
			_Log(dfLOG_LEVEL_SYSTEM, "[No.%d] IOCPWorkerThread Exit", (int)args);
			return 0;
		}

		ptrSession = (Session*)overlapped->ptrSession;
		if (retvalGQCS == true && numberOfBytesTransferred != 0)
		{
			if (&(ptrSession->recvOverlapped) == overlapped)
			{
				ptrSession->sendRingBuffer.Enqueue(ptrSession->recvRingBuffer.GetFrontBufferPtr(), ptrSession->recvRingBuffer.MoveRear(numberOfBytesTransferred));
				ptrSession->recvRingBuffer.MoveFront(numberOfBytesTransferred);

				wsaBuf.buf = ptrSession->sendRingBuffer.GetFrontBufferPtr();
				wsaBuf.len = ptrSession->sendRingBuffer.GetDirectDequeueSize();
				ZeroMemory(&ptrSession->sendOverlapped, sizeof(WSAOVERLAPPED));
				InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
				if (WSASend(ptrSession->socket, &wsaBuf, 1, nullptr, 0, (LPWSAOVERLAPPED)&ptrSession->sendOverlapped, nullptr) == SOCKET_ERROR
					&& (wsaSendErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
				{
					_Log(dfLOG_LEVEL_SYSTEM, "WSASend error code: %d", wsaSendErrorCode);
					InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt);
				}
				else
				{
					flag = 0;
					wsaBuf.buf = ptrSession->recvRingBuffer.GetRearBufferPtr();
					wsaBuf.len = ptrSession->recvRingBuffer.GetDirectEnqueueSize();
					ZeroMemory(&ptrSession->recvOverlapped, sizeof(WSAOVERLAPPED));
					InterlockedIncrement((LONG*)&ptrSession->overlappedIOCnt);
					if (WSARecv(ptrSession->socket, &wsaBuf, 1, nullptr, &flag, (LPWSAOVERLAPPED)&ptrSession->recvOverlapped, nullptr) == SOCKET_ERROR
						&& (wsaRecvErrorCode = WSAGetLastError()) != WSA_IO_PENDING)
					{
						_Log(dfLOG_LEVEL_SYSTEM, "WSARecv error code: %d", wsaRecvErrorCode);
						InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt);
					}
				}
			}
			else if (&(ptrSession->sendOverlapped) == overlapped)
			{
				ptrSession->sendRingBuffer.MoveFront(numberOfBytesTransferred);
			}
		}

		if (InterlockedDecrement((LONG*)&ptrSession->overlappedIOCnt) == 0)
		{
			//���� ����
			closesocket(ptrSession->socket);
			sessionMap.erase(sessionID);
			delete ptrSession;
		}
	}
}