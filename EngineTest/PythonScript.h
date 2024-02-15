#pragma once
#include <C:/Users/zy/AppData/Local/Programs/Python/Python310/include/Python.h>

#ifdef _DEBUG
#pragma comment(lib, "C:/Users/zy/AppData/Local/Programs/Python/Python310/libs/python310_d.lib")
#else
#pragma comment(lib, "C:/Users/zy/AppData/Local/Programs/Python/Python310/libs/python310.lib")
#endif

int pyscript()
{
	Py_Initialize();

	//PyRun_SimpleString("print(hello!!!)\n");
	//PyRun_SimpleString("import os\n");
	//PyRun_SimpleString("print(os.getcwd())\n");

	PyObject *obj = Py_BuildValue("s", "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Python_Scripts/compileShaders.py");
	FILE* fp = _Py_fopen_obj(obj, "r");
	if (fp != NULL)
	{
		PyRun_SimpleFile(fp, "C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/Engine/Python_Scripts/compileShaders.py");
	}

	Py_Finalize();

	return 0;
}