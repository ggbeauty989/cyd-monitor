# Avvio automatico di monitor.py all'accesso a Windows (senza finestra).
#
#   powershell -ExecutionPolicy Bypass -File autostart_windows.ps1            # installa
#   powershell -ExecutionPolicy Bypass -File autostart_windows.ps1 -Remove    # rimuove
#
# Crea un'attività pianificata "CYD Monitor" per l'utente corrente: parte al login,
# gira in background con pythonw.exe e viene riavviata se si chiude per errore.
# Non servono permessi di amministratore.
param([switch]$Remove)

$TaskName = "CYD Monitor"

if ($Remove) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Avvio automatico rimosso."
    exit
}

$Script = Join-Path $PSScriptRoot "monitor.py"
$Python = (Get-Command python -ErrorAction Stop).Source
$PythonW = Join-Path (Split-Path $Python) "pythonw.exe"
if (-not (Test-Path $PythonW)) { throw "pythonw.exe non trovato accanto a $Python" }

$Action = New-ScheduledTaskAction -Execute $PythonW -Argument "`"$Script`" --no-ui" -WorkingDirectory $PSScriptRoot
$Trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$Trigger.Delay = "PT20S"  # lascia partire prima USB e LibreHardwareMonitor
$Settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger `
    -Settings $Settings -Description "Invia i dati hardware al display ESP32 CYD" -Force | Out-Null

Write-Host "Avvio automatico installato: '$TaskName'"
Write-Host "  $PythonW `"$Script`" --no-ui"
Write-Host "Avvialo subito con:  Start-ScheduledTask -TaskName '$TaskName'"
