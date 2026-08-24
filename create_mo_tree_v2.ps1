$ErrorActionPreference = "Stop"

$Root = "C:\Users\pjesi\Desktop\Pawel Projekty\MO"

$Files = @(
    "lib\MultiObserver\src\MO.h",
    "lib\MultiObserver\src\MO.cpp",

    "lib\MultiObserver\src\MOConfig.h",
    "lib\MultiObserver\src\MOConfig.cpp",

    "lib\MultiObserver\src\MOWifi.h",
    "lib\MultiObserver\src\MOWifi.cpp",
    "lib\MultiObserver\src\MOWifiPrefs.h",
    "lib\MultiObserver\src\MOWifiPrefs.cpp",

    "lib\MultiObserver\src\MOMQTT.h",
    "lib\MultiObserver\src\MOMQTT.cpp",
    "lib\MultiObserver\src\MOMQTTPrefs.h",
    "lib\MultiObserver\src\MOMQTTPrefs.cpp",

    "lib\MultiObserver\src\MOCli.h",
    "lib\MultiObserver\src\MOCli.cpp",

    "examples\simple_repeater\MOBridge.h",
    "examples\simple_repeater\MOBridge.cpp",

    "installer\install.py",
    "installer\verify.py"
)

New-Item -ItemType Directory -Path $Root -Force | Out-Null

foreach ($RelativePath in $Files) {
    $FullPath = Join-Path $Root $RelativePath
    $Directory = Split-Path -Parent $FullPath

    New-Item -ItemType Directory -Path $Directory -Force | Out-Null

    if (-not (Test-Path -LiteralPath $FullPath)) {
        New-Item -ItemType File -Path $FullPath -Force | Out-Null
        Write-Host "[CREATE] $RelativePath"
    }
    else {
        Write-Host "[EXISTS] $RelativePath"
    }
}

Write-Host ""
Write-Host "=== MultiObserver tree ==="

Get-ChildItem -LiteralPath $Root -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        Write-Host ("  " + $_.FullName.Substring($Root.Length + 1))
    }

Write-Host ""
Write-Host "Done."
