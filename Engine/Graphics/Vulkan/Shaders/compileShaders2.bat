@ECHO OFF
SetLocal EnableDelayedExpansion
REM Compiler Shaders...

SET inVertShaderFiles=
FOR /R %%f in (*.vert) do (
	FOR %%n in ("%%~nf") do (
		SET inVertShaderFiles=%%~n
	)

	ECHO %~dp0!inVertShaderFiles!.vert
	ECHO %~dp0!inVertShaderFiles!.vert.spv
	%VULKAN_SDK%\bin\glslangValidator.exe -V %~dp0!inVertShaderFiles!.vert -o %~dp0!inVertShaderFiles!.vert.spv
	IF %ERRORLEVEL% NEQ 0 (ECHO Error: %ERRORLEVEL% && exit)
)


SET inFragShaderFiles=
FOR /R %%f in (*.frag) do (
	FOR %%n in ("%%~nf") do (
		SET inFragShaderFiles=%%~n
	)

	ECHO %~dp0!inFragShaderFiles!.frag
	ECHO %~dp0!inFragShaderFiles!.frag.spv
	%VULKAN_SDK%\bin\glslangValidator.exe -V %~dp0!inFragShaderFiles!.frag -o %~dp0!inFragShaderFiles!.frag.spv
	IF %ERRORLEVEL% NEQ 0 (ECHO Error: %ERRORLEVEL% && exit)
)