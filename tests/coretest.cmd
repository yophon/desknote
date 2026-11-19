@echo off
rem 在 Windows 上编译并跑核心测试（src/core + src/win/plat_win.cpp，带 C 运行库，只为了 printf）
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set OUT=%TEMP%\desknote-coretest
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /utf-8 /O1 /W3 /wd4244 /wd4267 /DUNICODE /D_UNICODE /I"%~dp0..\src\core" /I"%~dp0..\src\win" /Fo"%OUT%\\" /Fe"%OUT%\coretest.exe" "%~dp0coretest.cpp" "%~dp0..\src\core\*.cpp" "%~dp0..\src\win\plat_win.cpp" bcrypt.lib crypt32.lib winhttp.lib >"%OUT%\build.log" || (type "%OUT%\build.log" & exit /b 1)
"%OUT%\coretest.exe" "%OUT%"
