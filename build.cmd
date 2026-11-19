@echo off
rem Build with the built-in .NET Framework 4 compiler (no SDK needed)
rem Icon: powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1 DeskNote.ico
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0DeskNote.exe
set FW=C:\Windows\Microsoft.NET\Framework64\v4.0.30319
"%FW%\csc.exe" /nologo /target:winexe /optimize+ /out:"%OUT%" ^
  /win32icon:"%~dp0DeskNote.ico" /win32manifest:"%~dp0app.manifest" ^
  /resource:"%~dp0DeskNote.ico",DeskNote.ico ^
  /r:"%FW%\WPF\PresentationFramework.dll" /r:"%FW%\WPF\PresentationCore.dll" /r:"%FW%\WPF\WindowsBase.dll" ^
  /r:System.Xaml.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll /r:System.Security.dll ^
  "%~dp0src\*.cs"
