@echo off
rem Builds the SMP previewer (build\NVSMPPreviewer.exe): the plugin's physics core and system builder,
rem tools\scene.cpp's in-memory NIF, a Direct3D 9 window and Dear ImGui. Then the NV SMP Maker (build\NVSMPMaker.exe):
rem the same program built with NVSMP_MAKER, opening on its Make SMP window (painting chains, smpdress, smpify, smprepair).
rem Run build.bat first: the previewer links build\bullet.lib.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set ROOT=%~dp0..\
set OUT=%ROOT%build
set BULLET=%ROOT%extern\bullet
set IMGUI=%ROOT%extern\imgui
set FLAGS=/nologo /O2 /Oi /GL /arch:SSE2 /fp:fast /EHsc /MT /GS- /DNDEBUG /DWIN32 /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN /std:c++20 /W3 /wd4244 /wd4267 /wd4018 /wd4996 /permissive- /Zc:preprocessor /bigobj /Zi /c
set INCLUDES=/I"%BULLET%" /I"%ROOT%src\compat" /I"%ROOT%src\nv" /I"%ROOT%src\fsmp" /I"%ROOT%src\fsmp\hdtSkinnedMesh" /I"%ROOT%tools" /I"%IMGUI%" /FI"%ROOT%src\compat\PCH.h"
if not exist "%OUT%\previewobj" mkdir "%OUT%\previewobj"
if not exist "%OUT%\makerobj" mkdir "%OUT%\makerobj"

pushd "%OUT%\previewobj"
cl %FLAGS% %INCLUDES% ^
	"%ROOT%src\fsmp\hdtSkinnedMesh\*.cpp" "%ROOT%src\fsmp\XmlReader.cpp" "%ROOT%src\nv\hdtConvertNi.cpp" "%ROOT%src\nv\hdtSkyrimBody.cpp" "%ROOT%src\nv\hdtSkyrimBone.cpp" "%ROOT%src\nv\hdtSkyrimPhysicsWorld.cpp" "%ROOT%src\nv\hdtSkyrimSystem.cpp" ^
	"%ROOT%tools\scene.cpp" "%ROOT%tools\kf.cpp" "%ROOT%tools\previewer\datafiles.cpp" "%ROOT%tools\previewer\bsa.cpp" "%ROOT%tools\previewer\inflate.cpp" "%ROOT%tools\previewer\previewer.cpp" ^
	"%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\imgui_impl_dx9.cpp" "%IMGUI%\imgui_impl_win32.cpp" || (popd & exit /b 1)
link /nologo /DEBUG /LTCG /SUBSYSTEM:WINDOWS /OUT:"%OUT%\NVSMPPreviewer.exe" *.obj "%OUT%\bullet.lib" || (popd & exit /b 1)
rem The Maker: previewer.cpp again with NVSMP_MAKER, linked with everything else above.
if exist "%OUT%\makerobj\objs.rsp" del "%OUT%\makerobj\objs.rsp"
for %%f in (*.obj) do if /I not "%%f"=="previewer.obj" echo "%OUT%\previewobj\%%f">>"%OUT%\makerobj\objs.rsp"
popd

pushd "%OUT%\makerobj"
cl %FLAGS% /DNVSMP_MAKER %INCLUDES% "%ROOT%tools\previewer\previewer.cpp" /Fo:maker.obj /Fd:maker.pdb || (popd & exit /b 1)
link /nologo /DEBUG /LTCG /SUBSYSTEM:WINDOWS /OUT:"%OUT%\NVSMPMaker.exe" maker.obj @objs.rsp "%OUT%\bullet.lib" || (popd & exit /b 1)
popd
echo previewer ok
