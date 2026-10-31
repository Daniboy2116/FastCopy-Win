@echo off
setlocal
set ROOT=%~dp0
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
set MH=%ROOT%third_party\minhook
set ENGINE="%ROOT%src\engine\engine.c" "%ROOT%src\engine\log.c"
set LIBS=ole32.lib shell32.lib shlwapi.lib uuid.lib user32.lib gdi32.lib dwmapi.lib advapi32.lib msimg32.lib comctl32.lib

cl /nologo /utf-8 /O2 /W3 /MT /LD /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\fc_hook.dll" /I"%MH%\include" ^
  "%ROOT%src\hook\fchook.c" "%ROOT%src\hook\ui.c" %ENGINE% "%MH%\src\hook.c" "%MH%\src\buffer.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" ^
  %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\engine_test.exe" "%ROOT%tests\engine_test.c" %ENGINE% %LIBS% || exit /b 1

cl /nologo /utf-8 /O2 /W3 /MT /Fo"%ROOT%obj\\" /Fe"%ROOT%bin\fc_agent.exe" "%ROOT%src\agent\agent.c" "%ROOT%src\engine\log.c" ^
  shlwapi.lib /link /SUBSYSTEM:WINDOWS || exit /b 1
echo BUILD OK
