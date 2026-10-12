@echo off
rem Builds the rig previewer (build\NVRigPreviewer.exe): tools\scene.cpp's in-memory NIF (which links the
rem plugin's physics core, though the rig previewer never runs it), tools\kf.cpp's animations, the SMP
rem previewer's Data folder and BSA lookup, a Direct3D 9 window and Dear ImGui.
rem Run build.bat first: this links build\bullet.lib.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set ROOT=%~dp0..\
set OUT=%ROOT%build
set BULLET=%ROOT%extern\bullet
set IMGUI=%ROOT%extern\imgui
if not exist "%OUT%\rigobj" mkdir "%OUT%\rigobj"

pushd "%OUT%\rigobj"
cl /nologo /O2 /Oi /GL /arch:SSE2 /fp:fast /EHsc /MT /GS- /DNDEBUG /DWIN32 /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN /std:c++20 /W3 /wd4244 /wd4267 /wd4018 /wd4996 /permissive- /Zc:preprocessor /bigobj /Zi /c ^
	/I"%BULLET%" /I"%ROOT%src\compat" /I"%ROOT%src\nv" /I"%ROOT%src\fsmp" /I"%ROOT%src\fsmp\hdtSkinnedMesh" /I"%ROOT%tools" /I"%ROOT%tools\previewer" /I"%IMGUI%" ^
	/FI"%ROOT%src\compat\PCH.h" ^
	"%ROOT%src\fsmp\hdtSkinnedMesh\*.cpp" "%ROOT%src\fsmp\XmlReader.cpp" "%ROOT%src\nv\hdtConvertNi.cpp" "%ROOT%src\nv\hdtSkyrimBody.cpp" "%ROOT%src\nv\hdtSkyrimBone.cpp" "%ROOT%src\nv\hdtSkyrimPhysicsWorld.cpp" "%ROOT%src\nv\hdtSkyrimSystem.cpp" ^
	"%ROOT%tools\scene.cpp" "%ROOT%tools\kf.cpp" "%ROOT%tools\previewer\datafiles.cpp" "%ROOT%tools\previewer\bsa.cpp" "%ROOT%tools\previewer\inflate.cpp" "%ROOT%tools\rigpreviewer\rigpreviewer.cpp" ^
	"%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\imgui_impl_dx9.cpp" "%IMGUI%\imgui_impl_win32.cpp" || (popd & exit /b 1)
link /nologo /DEBUG /LTCG /SUBSYSTEM:WINDOWS /OUT:"%OUT%\NVRigPreviewer.exe" *.obj "%OUT%\bullet.lib" || (popd & exit /b 1)
popd
echo rig previewer ok
