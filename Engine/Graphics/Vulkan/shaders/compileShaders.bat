@ECHO OFF
SetLocal EnableDelayedExpansion
REM Compiler Shaders...

SET inVertShaderFiles=
FOR /R %%f in (*.vert.glsl) do (
	FOR %%n in ("%%~nf") do (
		SET inVertShaderFiles=%%~n
	)

	ECHO %~dp0!inVertShaderFiles!.glsl
	ECHO %~dp0!inVertShaderFiles!.spv
	%VULKAN_SDK%\bin\glslc.exe -fshader-stage=vert %~dp0!inVertShaderFiles!.glsl -o %~dp0!inVertShaderFiles!.spv
	IF %ERRORLEVEL% NEQ 0 (ECHO Error: %ERRORLEVEL% && exit)
)


SET inFragShaderFiles=
FOR /R %%f in (*.frag.glsl) do (
	FOR %%n in ("%%~nf") do (
		SET inFragShaderFiles=%%~n
	)

	ECHO %~dp0!inFragShaderFiles!.glsl
	ECHO %~dp0!inFragShaderFiles!.spv
	%VULKAN_SDK%\bin\glslc.exe -fshader-stage=frag %~dp0!inFragShaderFiles!.glsl -o %~dp0!inFragShaderFiles!.spv
	IF %ERRORLEVEL% NEQ 0 (ECHO Error: %ERRORLEVEL% && exit)
)