@echo off
rem Builds and zips what a user needs: dist\FastCopy-Win.zip (publish it as a GitHub release).
setlocal
set ROOT=%~dp0
call "%ROOT%build.bat" || exit /b 1
if exist "%ROOT%dist" rmdir /s /q "%ROOT%dist"
mkdir "%ROOT%dist\FastCopy-Win"
for %%F in (bin\fc_hook.dll bin\fc_agent.exe install.ps1 uninstall.ps1 Instalar.bat Desinstalar.bat) do copy "%ROOT%%%F" "%ROOT%dist\FastCopy-Win\" >nul || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Path '%ROOT%dist\FastCopy-Win\*' -DestinationPath '%ROOT%dist\FastCopy-Win.zip' -Force" || exit /b 1
echo ZIP OK: %ROOT%dist\FastCopy-Win.zip
