$ErrorActionPreference = "Stop"

function Read-Required([string]$Prompt) {
    while ($true) {
        $value = Read-Host $Prompt
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            return $value.Trim()
        }
        Write-Host "A value is required." -ForegroundColor Yellow
    }
}

function Read-Key([string]$Prompt, [int]$Length, [string]$Label) {
    while ($true) {
        $value = (Read-Host $Prompt).Trim().ToUpperInvariant()
        if ($value -match "^[0-9A-F]+$" -and $value.Length -eq $Length) {
            return $value
        }
        Write-Host "$Label must contain exactly $Length HEX characters." -ForegroundColor Yellow
    }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$installer = Join-Path $repoRoot "installer\install.py"
$verifier = Join-Path $repoRoot "installer\verify.py"

Write-Host ""
Write-Host "=== MultiObserver Installer ===" -ForegroundColor Cyan
Write-Host ""

$meshcore = Read-Required "Clean MeshCore source path:"
$meshcore = [System.IO.Path]::GetFullPath($meshcore)

if (-not (Test-Path -LiteralPath $meshcore -PathType Container)) {
    throw "MeshCore source directory does not exist: $meshcore"
}

$name = Read-Required "Repeater name:"
$publicKey = Read-Key "Public key:" 64 "Public key"
$privateKey = Read-Key "Private key:" 128 "Private key"

Write-Host ""
Write-Host "Installing MultiObserver..." -ForegroundColor Cyan
Write-Host ""

$py = Get-Command py -ErrorAction SilentlyContinue
if ($null -ne $py) {
    & py -3 $installer $meshcore `
        --repeater-name $name `
        --public-key $publicKey `
        --private-key $privateKey
} else {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $python) {
        throw "Python 3 was not found. Install Python 3 and run the installer again."
    }

    & python $installer $meshcore `
        --repeater-name $name `
        --public-key $publicKey `
        --private-key $privateKey
}

if ($LASTEXITCODE -ne 0) {
    throw "MultiObserver installation failed. Exit code: $LASTEXITCODE"
}

Write-Host ""
Write-Host "Verifying installation..." -ForegroundColor Cyan
Write-Host ""

$py = Get-Command py -ErrorAction SilentlyContinue
if ($null -ne $py) {
    & py -3 $verifier $meshcore
} else {
    & python $verifier $meshcore
}

if ($LASTEXITCODE -ne 0) {
    throw "MultiObserver verification failed. Exit code: $LASTEXITCODE"
}

Write-Host ""
Write-Host "=== INSTALL + VERIFY OK ===" -ForegroundColor Green
