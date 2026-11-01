# Installs FastCopy: copies bin\ to %ProgramFiles%\FastCopy (elevated, so only admins can swap
# the DLL Explorer loads) and starts the agent at logon (scheduled task) and now, as the normal user.
# Not %LOCALAPPDATA%: Explorer fails to LoadLibrary from AppData on this build.
$ErrorActionPreference = 'Stop'
# From a clone the binaries are in bin\; in the downloaded zip they sit next to this script.
$src = if (Test-Path (Join-Path $PSScriptRoot 'bin' | Join-Path -ChildPath 'fc_hook.dll')) { Join-Path $PSScriptRoot 'bin' } else { $PSScriptRoot }
if (-not (Test-Path (Join-Path $src 'fc_hook.dll'))) { throw 'No encuentro fc_hook.dll. Extrae el ZIP completo antes de instalar.' }
$dst = Join-Path $env:ProgramFiles 'FastCopy'
$hook = Join-Path $dst 'fc_hook.dll'

Get-Process fc_agent -ErrorAction SilentlyContinue | Stop-Process -Force
$loaded = (Get-Process explorer -ErrorAction SilentlyContinue).Modules | Where-Object { $_.ModuleName -eq 'fc_hook.dll' }

# A DLL loaded by Explorer can't be overwritten but can be renamed out of the way.
$copy = @"
`$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force '$dst' | Out-Null
if (Test-Path '$hook') { try { Remove-Item '$hook' } catch { Rename-Item '$hook' "fc_hook.old-`$([DateTime]::Now.Ticks).dll" } }
Get-ChildItem '$dst' -Filter 'fc_hook.old-*.dll' | ForEach-Object { try { Remove-Item `$_.FullName } catch {} }
Copy-Item '$src\fc_agent.exe', '$src\fc_hook.dll' '$dst' -Force
"@
$p = Start-Process powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $copy
if ($p.ExitCode -ne 0 -or -not (Test-Path $hook)) { throw "La copia a $dst falló (código $($p.ExitCode))." }

# A logon task, not HKCU\Run: on this machine Run entries start ~1 min after logon and ours was skipped.
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' 'FastCopyAgent' -ErrorAction SilentlyContinue
$user = "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit 0 -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName 'FastCopyAgent' -Force -Settings $settings `
    -Action (New-ScheduledTaskAction -Execute "$dst\fc_agent.exe") `
    -Trigger (New-ScheduledTaskTrigger -AtLogOn -User $user) `
    -Principal (New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited) | Out-Null

if ($loaded) {
    Write-Host 'Reiniciando el Explorador para cargar la nueva versión...'
    Stop-Process -Name explorer -Force
    Start-Sleep 3
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
}
Start-Process (Join-Path $dst 'fc_agent.exe')
Write-Host "FastCopy instalado en $dst"
