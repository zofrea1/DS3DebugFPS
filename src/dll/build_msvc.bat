@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
ml64 /nologo /c /Fo thunks.obj thunks.asm
if errorlevel 1 exit /b 1
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS dllmain.c thunks.obj /Fe:D3DCompiler_43.dll /link /DEF:exports.def /BASE:0x150000000 /OPT:REF
if errorlevel 1 exit /b 1
echo Built %~dp0D3DCompiler_43.dll
