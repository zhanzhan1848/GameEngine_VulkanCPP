

/* Things to do to create a game project
	1) Generate an MSVC solution / project
	2) Add Files that contain the script
	3) Set include and library directories
	4) Set force include file (GameEntity.h)
	5) Set c++ language version and calling convension
*/
#ifdef _WIN64
#include "CommonHeaders.h"
#include <filesystem>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // !WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <crtdbg.h>
#ifndef  USE_WITH_EDITOR

extern bool engine_initialize();
extern void engine_update();
extern void engine_shutdown();

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
#if _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif
	if (engine_initialize())
	{
		MSG msg{};
		bool is_running{ true };
		while (is_running)
		{
			while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
				is_running &= (msg.message != WM_QUIT);
			}

			engine_update();
		}
	}
	engine_shutdown();
	return 0;
}
#endif // ! USE_WITH_EDITOR

#endif // _WIN64
