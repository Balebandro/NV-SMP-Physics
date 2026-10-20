@echo off
rem Builds the content tools (build\smpify.exe, smpdress.exe, smprepair.exe, nifcheck.exe, nifdump.exe, NVSMPPrepare.exe)
rem and the offline test (build\systemtest.exe).
rem Run build.bat first: the test links the plugin's physics code and build\bullet.lib.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set ROOT=%~dp0..\
set OUT=%ROOT%build
set BULLET=%ROOT%extern\bullet
if not exist "%OUT%\testobj" mkdir "%OUT%\testobj"

pushd "%OUT%"
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 "%ROOT%tools\smpify.cpp" /Fe:smpify.exe /Fo:smpify.obj || (popd & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 "%ROOT%tools\nifcheck.cpp" /Fe:nifcheck.exe /Fo:nifcheck.obj || (popd & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 "%ROOT%tools\smpdress.cpp" /Fe:smpdress.exe /Fo:smpdress.obj || (popd & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 "%ROOT%tools\smprepair.cpp" /Fe:smprepair.exe /Fo:smprepair.obj || (popd & exit /b 1)
cl /nologo /O2 /EHsc /std:c++17 /W3 /wd4244 /wd4267 /wd4996 "%ROOT%tools\nifdump.cpp" /Fe:nifdump.exe /Fo:nifdump.obj || (popd & exit /b 1)
rem NV SMP Prepare (a window program: pick NIFs, it adds what NV SMP Physics needs), with the previewer's Data folder search.
if not exist prepobj mkdir prepobj
cl /nologo /O2 /EHsc /MT /std:c++20 /W3 /wd4244 /wd4267 /wd4996 /DNOMINMAX /I"%ROOT%tools" "%ROOT%tools\smpprepare.cpp" "%ROOT%tools\previewer\datafiles.cpp" "%ROOT%tools\previewer\bsa.cpp" "%ROOT%tools\previewer\inflate.cpp" /Fe:NVSMPPrepare.exe /Fo:prepobj\ /link /SUBSYSTEM:WINDOWS user32.lib comdlg32.lib shell32.lib ole32.lib advapi32.lib d3d9.lib || (popd & exit /b 1)
popd

pushd "%OUT%\testobj"
cl /nologo /O2 /Oi /GL /arch:SSE2 /fp:fast /EHsc /MT /GS- /DNDEBUG /DWIN32 /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNVSMP_PROFILE /std:c++20 /W3 /wd4244 /wd4267 /wd4018 /wd4996 /permissive- /Zc:preprocessor /bigobj /Zi /c ^
	/I"%BULLET%" /I"%ROOT%src\compat" /I"%ROOT%src\nv" /I"%ROOT%src\fsmp" /I"%ROOT%src\fsmp\hdtSkinnedMesh" /I"%ROOT%tools" ^
	/FI"%ROOT%src\compat\PCH.h" ^
	"%ROOT%src\fsmp\hdtSkinnedMesh\*.cpp" "%ROOT%src\fsmp\XmlReader.cpp" "%ROOT%src\nv\hdtConvertNi.cpp" "%ROOT%src\nv\hdtSkyrimBody.cpp" "%ROOT%src\nv\hdtSkyrimBone.cpp" "%ROOT%src\nv\hdtSkyrimPhysicsWorld.cpp" "%ROOT%src\nv\hdtSkyrimSystem.cpp" "%ROOT%tools\scene.cpp" "%ROOT%tools\systemtest.cpp" || (popd & exit /b 1)
link /nologo /DEBUG /LTCG /OUT:"%OUT%\systemtest.exe" *.obj "%OUT%\bullet.lib" || (popd & exit /b 1)
popd
echo tools ok
