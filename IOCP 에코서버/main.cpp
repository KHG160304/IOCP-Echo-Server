#include "Network.h"
#include <stdio.h>
#include <conio.h>

#define SERVERPORT	11603

int shutdown = false;
int main(void)
{
	int key;
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

	printf("���� ������ ���� �Ϸ�\n");
	return 0;
}



