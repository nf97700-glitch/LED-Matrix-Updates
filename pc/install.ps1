# Установка LED Matrix Studio для Windows (без прав администратора)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$zipUrl = 'https://github.com/nf97700-glitch/LED-Matrix-Updates/releases/download/pc-runtime-1/LED-Matrix-Studio-Windows.zip'
$dir = Join-Path $env:LOCALAPPDATA 'LED Matrix Studio'
$zip = Join-Path $env:TEMP 'LED-Matrix-Studio-Windows.zip'

Write-Host ''
Write-Host '  LED Matrix Studio - установка' -ForegroundColor Red
Write-Host ''

# закрываем запущенную программу, если она уже установлена
Get-Process pythonw -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dir*" } | Stop-Process -Force
Start-Sleep -Milliseconds 500

Write-Host '  Скачиваю (около 35 МБ)...'
$ProgressPreference = 'SilentlyContinue'
Invoke-WebRequest -UseBasicParsing -Uri $zipUrl -OutFile $zip

Write-Host '  Распаковываю...'
# картинки (saved_pictures.json) в папке app сохраняются
if (Test-Path (Join-Path $dir 'runtime')) { Remove-Item (Join-Path $dir 'runtime') -Recurse -Force }
Expand-Archive -Path $zip -DestinationPath $dir -Force
Remove-Item $zip -Force

$pyw = Join-Path $dir 'runtime\pythonw.exe'
$app = Join-Path $dir 'app'
$shell = New-Object -ComObject WScript.Shell
$targets = @(
  (Join-Path ([Environment]::GetFolderPath('Desktop')) 'LED Matrix Studio.lnk'),
  (Join-Path ([Environment]::GetFolderPath('Programs')) 'LED Matrix Studio.lnk')
)
foreach ($t in $targets) {
  $lnk = $shell.CreateShortcut($t)
  $lnk.TargetPath = $pyw
  $lnk.Arguments = '"' + (Join-Path $app 'LEDMatrixStudio.pyc') + '"'
  $lnk.WorkingDirectory = $app
  $lnk.IconLocation = (Join-Path $app 'icon.ico') + ',0'
  $lnk.Description = 'LED Matrix Studio'
  $lnk.Save()
}

Write-Host '  Готово! Ярлык LED Matrix Studio - на рабочем столе и в меню Пуск.' -ForegroundColor Green
Start-Process -FilePath $pyw -ArgumentList ('"' + (Join-Path $app 'LEDMatrixStudio.pyc') + '"') -WorkingDirectory $app
