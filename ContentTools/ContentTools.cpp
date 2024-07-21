#include "ToolsCommon.h"

namespace primal::tools
{
	extern void ShutDownTextureTools();
}

EDITOR_INTERFACE void ShutDownContentTools()
{
	using namespace primal::tools;
	ShutDownTextureTools();
}