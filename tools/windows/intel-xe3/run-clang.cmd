@echo off
rem Runs the build from build-clang.cmd with the switches this machine profiles with, plus the GPU
rem frame trace. The trace prints "DKB gpu_work_shape" once per sampled frame: the spread of the GPU
rem time per draw and per dispatch, which is what decides whether batching draws can pay.
rem   run-clang.cmd          trace on
rem   run-clang.cmd 0        trace off (the switches alone)
setlocal
set "SRC=%~dp0..\..\.."
set "OUT=%SRC%\..\out-clang"
set "EDEN_NX2_GPU_PROFILE=1"
set "EDEN_NX2_FRAME_LIMIT=1"
set "EDEN_NX2_DELTA_CB=1"
set "EDEN_NX2_FENCE_FLUSH_US=500"
set "EDEN_HYBRID_BACKGROUND=1"
set "EDEN_DKB_FRAME_TRACE=1"
if not "%~1"=="" set "EDEN_DKB_FRAME_TRACE=%~1"
start "" "%OUT%\bin\lichii.exe"
