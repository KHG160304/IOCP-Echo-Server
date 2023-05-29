#include "Network.h"
#include <stdio.h>
#include <conio.h>

#define SERVERPORT	6000

void OnRecv(SESSIONID sessionId, SerializationBuffer& packet)
{
	_int64 echoBody;
	SerializationBuffer sendPacket(sizeof(_int64));
	packet >> echoBody;

	//printf("SESSION_ID[%lld] %lld\n", sessionId, echoBody);

	sendPacket << echoBody;
	SendPacket(sessionId, sendPacket);
}

int shutdown = false;
int main(void)
{
	int key;

	SetOnRecvEvent(OnRecv);
	if (!InitNetworkLib(SERVERPORT))
	{
		return 0;
	}

	while (!shutdown)
	{
		if (_kbhit())
		{
			key = _getch();
			if (key == 'Q' || key == 'q')
			{
				RequestExitNetworkLibThread();
				shutdown = true;
			}
		}
	}

	printf("메인 스레드 종료 완료\n");
	return 0;
}



