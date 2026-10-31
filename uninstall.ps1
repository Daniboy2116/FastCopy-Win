# Removes FastCopy and restarts Explorer so the hook is unloaded.
$dst = Join-Path $env:ProgramFiles 'FastCopy'

Unregister-ScheduledTask -TaskName 'FastCopyAgent' -Confirm:$false -ErrorAction SilentlyContinue
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' 'FastCopyAgent' -ErrorAction SilentlyContinue
Get-Process fc_agent -ErrorAction SilentlyContinue | Stop-Process -Force
Stop-Process -Name explorer -Force
Start-Sleep 3
if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
Start-Process powershell -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', "Remove-Item '$dst' -Recurse -Force"
Write-Host 'FastCopy desinstalado; el Explorador vuelve a copiar de forma nativa.'
