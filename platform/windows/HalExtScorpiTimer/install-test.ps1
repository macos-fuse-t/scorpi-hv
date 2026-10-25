# Installs the test HAL extension in the current guest. Reboot separately.
param(
    [string]$Dll = "$PSScriptRoot\HalExtScorpiTimer.dll",
    [string]$CertificateDirectory = "${env:ProgramFiles(x86)}\Windows Kits\10\Tools\certificates"
)
$ErrorActionPreference = 'Stop'
$bcd = & bcdedit /enum '{current}' | Out-String
if ($bcd -notmatch 'testsigning\s+Yes') { throw 'Test signing must already be enabled for this test extension.' }
# HAL extensions have a distinct boot signing policy. A self-signed generic
# code-signing certificate can verify in Authenticode and still be rejected by
# winload with STATUS_INVALID_IMAGE_HASH. Use the WDK's OEM HAL test certificate.
$cert = Import-PfxCertificate -FilePath "$CertificateDirectory\OEM_HAL_Extension_Test_Cert_2017.pfx" -CertStoreLocation Cert:\LocalMachine\My
if ($cert.NotAfter -lt (Get-Date) -or $cert.NotBefore -gt (Get-Date)) {
    throw 'The WDK OEM HAL test certificate is not currently valid; update the WDK test certificates.'
}
Import-Certificate -FilePath "$CertificateDirectory\OEM_Root_CA_2017.cer" -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath "$CertificateDirectory\OEM_Intermediate_Cert_2017.cer" -CertStoreLocation Cert:\LocalMachine\CA | Out-Null
$target = "$env:SystemRoot\System32\HalExtScorpiTimer.dll"
Copy-Item $Dll $target -Force
$sig = Set-AuthenticodeSignature -FilePath $target -Certificate $cert -HashAlgorithm SHA256
if ($sig.Status -ne 'Valid') { throw "Signing failed: $($sig.StatusMessage)" }
$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\OSExtensionDatabase\VEN_SCPR&DEV_0001'
New-Item $key -Force | Out-Null
New-ItemProperty $key -Name HalExtension -PropertyType String -Value '\SystemRoot\System32\HalExtScorpiTimer.dll' -Force | Out-Null
Write-Output "Installed $target ($($sig.Status)); reboot the guest to load the extension."
