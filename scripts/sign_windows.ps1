# Sign a Windows binary (exe/dll) with signtool.
#
# Certificate source is controlled by env vars (checked in this order):
#   WIN_SIGN_THUMBPRINT   - thumbprint of a cert already in the Windows cert
#                           store (used by cloud HSM / EV tokens, and by
#                           self-signed certs installed via Import-PfxCertificate)
#   WIN_SIGN_PFX / WIN_SIGN_PFX_PASSWORD - path to a .pfx file + its password
#
# If neither is set, the script prints instructions for generating a free
# self-signed certificate for internal testing and exits without signing.
# Swapping to a real (EV / Azure Trusted Signing / purchased) certificate
# later requires no changes to this script or the build - just point the
# same env vars at the new certificate.
#
# Usage: sign_windows.ps1 <path-to-exe-or-dll> [more files...]

param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
    $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($signtool) { return $signtool.Source }
    $candidates = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } | Sort-Object FullName -Descending
    if ($candidates) { return $candidates[0].FullName }
    throw "signtool.exe not found. Install the Windows SDK."
}

$thumbprint = $env:WIN_SIGN_THUMBPRINT
$pfx = $env:WIN_SIGN_PFX
$pfxPassword = $env:WIN_SIGN_PFX_PASSWORD

if (-not $thumbprint -and -not $pfx) {
    Write-Warning "No signing certificate configured (WIN_SIGN_THUMBPRINT or WIN_SIGN_PFX not set)."
    Write-Host ""
    Write-Host "Fastest free option for internal testing - self-signed certificate:"
    Write-Host '  $cert = New-SelfSignedCertificate -Type CodeSigning -Subject "CN=CoPrint Slicer Dev" -CertStoreLocation Cert:\CurrentUser\My'
    Write-Host '  $env:WIN_SIGN_THUMBPRINT = $cert.Thumbprint'
    Write-Host ""
    Write-Host "To trust it on this machine (so SmartScreen/AV do not warn locally), also import it into Trusted Root + Trusted Publishers:"
    Write-Host '  Export-Certificate -Cert $cert -FilePath codesign-dev.cer'
    Write-Host '  Import-Certificate -FilePath codesign-dev.cer -CertStoreLocation Cert:\LocalMachine\Root'
    Write-Host '  Import-Certificate -FilePath codesign-dev.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher'
    Write-Host ""
    Write-Host "Note: a self-signed cert is only trusted on machines where you import it -"
    Write-Host "it does NOT remove SmartScreen warnings for other users. Replace it with a"
    Write-Host "real certificate (EV, or Azure Trusted Signing) before public distribution."
    Write-Host ""
    Write-Host "Skipping signing." -ForegroundColor Yellow
    exit 0
}

$signtool = Find-SignTool
Write-Host "Using signtool: $signtool"

foreach ($file in $Files) {
    if (-not (Test-Path $file)) {
        throw "File not found: $file"
    }
    Write-Host "Signing $file"
    if ($thumbprint) {
        & $signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /sha1 $thumbprint $file
    } else {
        $pwArg = if ($pfxPassword) { $pfxPassword } else { "" }
        & $signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /f $pfx /p $pwArg $file
    }
    if ($LASTEXITCODE -ne 0) {
        throw "signtool failed on $file (exit $LASTEXITCODE)"
    }
    & $signtool verify /pa $file
}

Write-Host "Done."
