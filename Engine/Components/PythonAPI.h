#pragma once
#include "CommonHeaders.h"

#pragma comment(lib, "C:/Users/27042/AppData/Local/Programs/Python/Python37/libs/python37_d.lib")
#pragma comment(lib, "C:/Users/27042/AppData/Local/Programs/Python/Python37/libs/python37.lib")
#pragma comment(lib, "C:/Users/27042/AppData/Local/Programs/Python/Python37/libs/python3.lib")
#pragma comment(lib, "C:/Users/27042/AppData/Local/Programs/Python/Python37/libs/python3_d.lib")

#include "C:/Users/27042/AppData/Local/Programs/Python/Python37/include/Python.h"

namespace primal::python
{

	void run_python_script(const char* name)
	{
		Py_Initialize();
		PyObject *obj = Py_BuildValue("s", name);
		FILE* fp = _Py_fopen_obj(obj, "rb");
		if (Py_IsInitialized() && fp != NULL)
		{
			PyRun_SimpleFile(fp, name);
			Py_Finalize();
		}			
	}

}

#define RUN_PYTHON_SCRIPT(name) primal::python::run_python_script(##name)
