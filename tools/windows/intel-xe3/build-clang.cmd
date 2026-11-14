@echo off
rem Builds what configure-clang.cmd configured. The executable and its DLLs land in OUT\bin.
setlocal
set "SRC=%~dp0..\..\.."
set "OUT=%SRC%\..\out-clang"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cmake --build "%OUT%" --target yuzu
exit /b %errorlevel%
