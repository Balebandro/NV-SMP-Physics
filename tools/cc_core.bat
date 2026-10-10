@echo off
rem Compiles FSMP's physics core and the New Vegas glue (no link), to check the port. Output in build\core.log.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set ROOT=%~dp0..
if not exist "%ROOT%\build\core" mkdir "%ROOT%\build\core"
pushd "%ROOT%\build\core"
cl /nologo /std:c++20 /O2 /Oi /arch:SSE2 /fp:fast /EHsc /MT /GS- /W3 /wd4244 /wd4267 /wd4018 /DNDEBUG /DWIN32 /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN /permissive- /Zc:preprocessor /bigobj /I"%ROOT%\extern\bullet" /I"%ROOT%\src\compat" /I"%ROOT%\src\nv" /I"%ROOT%\src\fsmp" /I"%ROOT%\src\fsmp\hdtSkinnedMesh" /FI"%ROOT%\src\compat\PCH.h" /c %* > "%ROOT%\build\core.log" 2>&1
set RC=%ERRORLEVEL%
popd
exit /b %RC%
