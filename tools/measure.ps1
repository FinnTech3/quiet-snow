# Starts the built Windows app and reports working set and CPU over a 20 second window (after 5 seconds of start-up).
param([string]$Exe = ".\QuietSnow.exe")
$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds 5
$p.Refresh()
if ($p.HasExited) { "exited early with code $($p.ExitCode)"; exit 1 }
$t1 = $p.TotalProcessorTime.TotalSeconds
Start-Sleep -Seconds 20
$p.Refresh()
if ($p.HasExited) { "exited during the run with code $($p.ExitCode)"; exit 1 }
$t2 = $p.TotalProcessorTime.TotalSeconds
$ws = $p.WorkingSet64 / 1MB
Stop-Process -Id $p.Id -Force
"rss_mb={0:N1} cpu_pct={1:N2}" -f $ws, ((($t2 - $t1) / 20) * 100)
