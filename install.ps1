# Installs Quiet Snow for Windows into %LOCALAPPDATA%\QuietSnow and starts it. No admin rights, nothing else touched.
# Uninstall: Stop-Process -Name QuietSnow; Remove-Item -Recurse "$env:LOCALAPPDATA\QuietSnow"
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA 'QuietSnow'
$zip = Join-Path $env:TEMP 'QuietSnow-Windows.zip'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Get-Process -Name QuietSnow -ErrorAction SilentlyContinue | Stop-Process -Force
Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/FinnTech3/quiet-snow/releases/latest/download/QuietSnow-Windows.zip' -OutFile $zip
Expand-Archive -Path $zip -DestinationPath $dir -Force
Remove-Item $zip
Start-Process -FilePath (Join-Path $dir 'QuietSnow.exe')
Write-Host 'Quiet Snow is running. Look for the snowflake in your tray (it may be under the ^ arrow).'
