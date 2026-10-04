param(
  [ValidateSet("Debug","Release")]
  [string]$Configuration = "Release",
  [string]$Project = "xenia-canary-uwp\xenia-canary-uwp.vcxproj",
  [int]$TimeoutSeconds = 600,
  [switch]$SkipPremake
)

$ErrorActionPreference = "Stop"
$repo = $PSScriptRoot
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$buildTemp = Join-Path $repo ".build-temp"
New-Item -ItemType Directory -Force -Path $buildTemp | Out-Null

$env:TEMP = $buildTemp
$env:TMP = $buildTemp
$env:XENIA_LOW_MEMORY_BUILD = "1"
$env:MSBUILDDISABLENODEREUSE = "1"
$env:PreferredToolArchitecture = "x64"
$env:CL_MPCount = "1"

$exitCode = 1

try {
  $zlib = Join-Path $repo "third_party\zlib-ng"
  $requiredZlib = @("zlib-ng.h","zconf-ng.h","zlib_name_mangling-ng.h","gzread.c")
  if ($requiredZlib | Where-Object { -not (Test-Path (Join-Path $zlib $_)) }) {
    Remove-Item (Join-Path $zlib "CMakeCache.txt") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $zlib "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $zlib "build.ninja"),(Join-Path $zlib "rules.ninja") -Force -ErrorAction SilentlyContinue
    & cmake -S $zlib -B $zlib -G "Visual Studio 17 2022" -A x64 -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF
    if ($LASTEXITCODE -ne 0) { throw "zlib-ng configure failed with exit code $LASTEXITCODE" }
  }

  $snappy = Join-Path $repo "third_party\snappy"
  if (-not (Test-Path (Join-Path $snappy "snappy-stubs-public.h"))) {
    Remove-Item (Join-Path $snappy "CMakeCache.txt") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $snappy "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $snappy "build.ninja"),(Join-Path $snappy "rules.ninja") -Force -ErrorAction SilentlyContinue
    & cmake -S $snappy -B $snappy -G "Visual Studio 17 2022" -A x64 -DSNAPPY_BUILD_TESTS=OFF -DSNAPPY_BUILD_BENCHMARKS=OFF -DSNAPPY_REQUIRE_AVX=ON
    if ($LASTEXITCODE -ne 0) { throw "Snappy configure failed with exit code $LASTEXITCODE" }
  }

  Push-Location $repo
  try {
    if (-not $SkipPremake) {
      & (Join-Path $repo "xb.bat") premake
      if ($LASTEXITCODE -ne 0) { throw "Premake failed with exit code $LASTEXITCODE" }
    }

    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $vsInstall) { throw "Visual Studio MSBuild installation not found" }
    $msbuild = Join-Path $vsInstall "MSBuild\Current\Bin\amd64\MSBuild.exe"
    if (-not (Test-Path $msbuild)) {
      $msbuild = Join-Path $vsInstall "MSBuild\Current\Bin\MSBuild.exe"
    }
    if (-not (Test-Path $msbuild)) { throw "MSBuild not found" }

    $projectPath = Join-Path $repo $Project
    if (-not (Test-Path $projectPath)) { throw "Project not found: $projectPath" }

    $projectStem = [IO.Path]::GetFileNameWithoutExtension($projectPath)
    $logPath = Join-Path $repo "$projectStem-build.log"

    $packageCertThumbprint = $null
    if ($Project -like "xenia-canary-uwp\*") {
      $manifestPath = Join-Path $repo "xenia-canary-uwp\Package.appxmanifest"
      [xml]$manifestXml = Get-Content -LiteralPath $manifestPath
      $publisher = [string]$manifestXml.Package.Identity.Publisher
      $packageCert = Get-ChildItem Cert:\CurrentUser\My |
          Where-Object { $_.Subject -eq $publisher -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30) } |
          Sort-Object NotAfter -Descending |
          Select-Object -First 1
      if (-not $packageCert) {
        Write-Host "Creating local UWP code-signing certificate for $publisher."
        $packageCert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $publisher -CertStoreLocation "Cert:\CurrentUser\My" -NotAfter (Get-Date).AddYears(5)
      }
      $packageCertThumbprint = $packageCert.Thumbprint
    }
    $args = @(
      $projectPath,
      "/m:1",
      "/nr:false",
      $(if ($Project -like "build\*") { '/p:Configuration="' + $Configuration + ' Windows"' } else { "/p:Configuration=$Configuration" }),
      "/p:Platform=x64",
      "/p:PreferredToolArchitecture=x64",
      "/p:CL_MPCount=1",
      "/p:UseMultiToolTask=false",
      "/v:minimal",
      "/fl",
      "/flp:logfile=$logPath;verbosity=minimal"
    )

    if ($packageCertThumbprint) {
      $args += "/p:AppxPackageSigningEnabled=true"
      $args += "/p:PackageCertificateKeyFile="
      $args += "/p:PackageCertificateThumbprint=$packageCertThumbprint"
      $args += "/p:AppxBundle=Never"
    }

    Write-Host "Building $Project with a hard timeout of $TimeoutSeconds seconds."
    Write-Host "TEMP=$buildTemp"

    $proc = Start-Process -FilePath $msbuild -ArgumentList $args -NoNewWindow -PassThru
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
      Write-Warning "MSBuild exceeded the timeout. Killing process tree PID $($proc.Id)."
      & taskkill.exe /PID $proc.Id /T /F | Out-Null
      $exitCode = 124
    } else {
      $exitCode = $proc.ExitCode
    }
  }
  finally {
    Pop-Location
  }
}
finally {
  if ($null -eq $oldTemp) { Remove-Item Env:TEMP -ErrorAction SilentlyContinue } else { $env:TEMP = $oldTemp }
  if ($null -eq $oldTmp) { Remove-Item Env:TMP -ErrorAction SilentlyContinue } else { $env:TMP = $oldTmp }
  Remove-Item Env:XENIA_LOW_MEMORY_BUILD -ErrorAction SilentlyContinue
  Remove-Item Env:MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue
  Remove-Item Env:PreferredToolArchitecture -ErrorAction SilentlyContinue
  Remove-Item Env:CL_MPCount -ErrorAction SilentlyContinue
}

exit $exitCode
