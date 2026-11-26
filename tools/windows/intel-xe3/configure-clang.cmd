@echo off
rem Configures the clang-cl + ThinLTO + PGO build described in docs/intel-arc/README.md.
rem Same flags as the official windows-x86_64-pgo job. Edit the four paths below.
rem Needs CMake 3.31 or newer on PATH (C:\Program Files\CMake\bin\cmake.exe, 4.5.0, works).
rem Set OUT before calling this to build a second tree (e.g. out-clang-v2) from another worktree.
setlocal
set "CL="
set "_CL_="
set "SRC=%~dp0..\..\.."
if not defined OUT set "OUT=%SRC%\..\out-clang"
set "QT=C:\Qt\6.12.0\msvc2022_64"
set "LLVM=C:\Program Files\LLVM\bin"
set "GLSLANG=C:\MyLibs\glslang-main-windows-x86_64-16.6.0\bin\glslang.exe"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
pushd "%SRC%"
set "PROFILE=%CD:\=/%/lichii-windows.profdata"
set "F=-DWIN32 -D_WINDOWS -DNOMINMAX -flto=thin -fprofile-use=%PROFILE% -Wno-error=profile-instr-out-of-date -Wno-error=profile-instr-unprofiled -Wno-error=backend-plugin"
cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_SDL3=ON -DENABLE_UPDATE_CHECKER=OFF -DLICHII_BUILD_TAG=build-150-intel-arc -DLICHII_BUILD_PLATFORM=windows-x86_64 -DUSE_DISCORD_PRESENCE=OFF "-DCMAKE_PREFIX_PATH=%QT%" "-DGLSLANGVALIDATOR=%GLSLANG%" "-DCMAKE_C_COMPILER=%LLVM%/clang-cl.exe" "-DCMAKE_CXX_COMPILER=%LLVM%/clang-cl.exe" "-DCMAKE_LINKER=%LLVM%/lld-link.exe" "-DCMAKE_AR=%LLVM%/llvm-lib.exe" "-DCMAKE_C_FLAGS=%F%" "-DCMAKE_CXX_FLAGS=-EHsc %F%"
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
