@echo off
rem Builds what configure-clang.cmd configured. The executable and its DLLs land in OUT\bin.
rem
rem vcvars64 is required, not optional: clang-cl picks the newest MSVC toolchain it finds, and a
rem VS 2026 Build Tools install beside VS 2022 (the one this tree was configured against) moves it to
rem a newer STL. The tree then fails with "Microsoft Visual C/C++ Version differs in precompiled file"
rem or, at link, "undefined symbol: __std_find_not_ch_1" and friends. Importing the VS 2022
rem environment makes clang-cl use MSVC 14.44, which is what CMAKE_CXX_COMPILER was checked with.
rem A stray CL variable breaks it too: clang-cl reads it and takes /flags for file names.
rem Set OUT before calling this to build a second tree (e.g. out-clang-v2) from another worktree.
setlocal
set "CL="
set "_CL_="
set "SRC=%~dp0..\..\.."
if not defined OUT set "OUT=%SRC%\..\out-clang"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
cmake --build "%OUT%" --target lichii
exit /b %errorlevel%
