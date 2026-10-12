@echo off
rem Builds NV SMP Physics: Bullet (once, into build\bullet.lib), then the plugin, build\NVSMPPhysics.dll.
rem Needs the Visual Studio 2022 C++ build tools. "build.bat clean" rebuilds Bullet too.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set ROOT=%~dp0
set OUT=%ROOT%build
if "%1"=="clean" rmdir /s /q "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

rem SSE in Bullet's API, as FSMP builds it: btVector3 and friends are 16-byte aligned __m128 values.
rem /GL + /LTCG: whole-program optimization across Bullet, FSMP and the glue (about 8% faster, same results).
set COMMON=/nologo /O2 /Oi /GL /arch:SSE2 /fp:fast /EHsc /MT /GS- /DNDEBUG /DWIN32 /D_WINDOWS /DBT_USE_SSE_IN_API /DNOMINMAX /DWIN32_LEAN_AND_MEAN
set BULLET=%ROOT%extern\bullet
set IMGUI=%ROOT%extern\imgui

if not exist "%OUT%\bullet.lib" (
	echo building bullet
	pushd "%OUT%"
	cl %COMMON% /W0 /c /I"%BULLET%" "%BULLET%\btLinearMathAll.cpp" "%BULLET%\btBulletCollisionAll.cpp" "%BULLET%\btBulletDynamicsAll.cpp" || (popd & exit /b 1)
	lib /nologo /OUT:bullet.lib btLinearMathAll.obj btBulletCollisionAll.obj btBulletDynamicsAll.obj || (popd & exit /b 1)
	popd
)

pushd "%OUT%\obj"
echo building imgui
cl %COMMON% /std:c++20 /W0 /c /I"%IMGUI%" "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\imgui_impl_dx9.cpp" "%IMGUI%\imgui_impl_win32.cpp" || (popd & exit /b 1)

echo building the plugin
rem src\nv before src\fsmp: the New Vegas glue keeps FSMP's file and class names (hdtSkyrimSystem.h ...).
cl %COMMON% /std:c++20 /W3 /wd4244 /wd4267 /wd4018 /wd4996 /permissive- /Zc:preprocessor /bigobj /Zi /c ^
	/I"%BULLET%" /I"%ROOT%src\compat" /I"%ROOT%src\nv" /I"%ROOT%src\fsmp" /I"%ROOT%src\fsmp\hdtSkinnedMesh" /I"%IMGUI%" ^
	/FI"%ROOT%src\compat\PCH.h" ^
	"%ROOT%src\fsmp\hdtSkinnedMesh\*.cpp" "%ROOT%src\fsmp\XmlReader.cpp" "%ROOT%src\nv\*.cpp" || (popd & exit /b 1)

link /nologo /DLL /DEBUG /LTCG /OPT:REF /OPT:ICF /OUT:"%OUT%\NVSMPPhysics.dll" *.obj "%OUT%\bullet.lib" user32.lib || (popd & exit /b 1)
popd
echo build ok
