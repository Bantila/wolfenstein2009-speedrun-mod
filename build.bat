@echo off
rem Builds srmod (x86): timer self-check, mod DLL (out\srmod.dll, installed as binkw32.dll) and launcher.
setlocal
cd /d "%~dp0"
call "D:\TailCore\toolchain\vs\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul
where cl >nul || (echo MSVC not found & exit /b 1)
if not exist out mkdir out

cl /nologo /EHsc /O2 timer_test.cpp /Fe:out\timer_test.exe /Fo:out\ >nul || exit /b 1
out\timer_test.exe || exit /b 1

cl /nologo /EHsc /O2 /MT /LD /D_CRT_SECURE_NO_WARNINGS mod.cpp /Fe:out\srmod.dll /Fo:out\ /link /DEF:binkw32.def user32.lib >nul || exit /b 1

"%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe" /nologo /target:winexe /platform:anycpu /optimize /codepage:65001 /out:out\SrmodLauncher.exe launcher.cs || exit /b 1

if "%1"=="deploy" copy /y out\srmod.dll ..\SP\ >nul && copy /y out\SrmodLauncher.exe ..\SP\ >nul && echo Deployed to ..\SP
echo Build OK
