# SPDX-License-Identifier: GPL-3.0-or-later
# Build paths for this host, dot-sourced by tools/env.ps1, the tests and the bench tools (no
# environment changes): $hostName, $buildRoot (build\<host>) and $cargoOut (the cargo release
# dir), both relative to the repo.
# PROCESSOR_ARCHITEW6432 is set when an x64 PowerShell runs emulated on ARM64.
$arm = "ARM64" -in $env:PROCESSOR_ARCHITECTURE, $env:PROCESSOR_ARCHITEW6432
$hostName = if ($arm) { "windows-arm64" } else { "windows-x64" }
$buildRoot = "build\$hostName"
$cargoOut = "$buildRoot\cargo\release"
