@echo off
rem DeskNote: C-style C++ without the C runtime, Direct2D + DirectWrite.
rem Usage: build.cmd [output.exe]
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0DeskNote.exe
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio C++ build tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set OBJ=%TEMP%\desknote-obj
if exist "%OBJ%" rmdir /s /q "%OBJ%"
mkdir "%OBJ%"
rc /nologo /fo "%OBJ%\DeskNote.res" "%~dp0DeskNote.rc" || exit /b 1
cl /nologo /c /utf-8 /O1 /Os /Oi /GS- /GR- /EHs-c- /Zc:threadSafeInit- /W3 /wd4244 /wd4267 /DUNICODE /D_UNICODE /I"%~dp0src\core" /I"%~dp0src\win" /Fo"%OBJ%\\" "%~dp0src\core\*.cpp" "%~dp0src\win\*.cpp" || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:Start /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /OUT:"%OUT%" "%OBJ%\*.obj" "%OBJ%\DeskNote.res" kernel32.lib user32.lib gdi32.lib shell32.lib comctl32.lib comdlg32.lib advapi32.lib crypt32.lib bcrypt.lib winhttp.lib d2d1.lib dwrite.lib ole32.lib uxtheme.lib dwmapi.lib imm32.lib || exit /b 1
echo built %OUT%
