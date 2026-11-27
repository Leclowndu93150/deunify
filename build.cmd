@echo off
setlocal

set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build"

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
	echo Visual Studio was not found
	exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"

if not exist "%BUILD_DIR%\build.ninja" (
	cmake -S "%ROOT%." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link || exit /b 1
)

cmake --build "%BUILD_DIR%" %* || exit /b 1
