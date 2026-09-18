[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',

    [string]$QMakePath,

    [string]$BuildRoot,

    [ValidateSet('NotRun', 'Passed', 'Failed')]
    [string]$QtClientToWindowsServer = 'NotRun',

    [ValidateSet('NotRun', 'Passed', 'Failed')]
    [string]$WindowsClientToQtServer = 'NotRun',

    [string]$OperatorNote = '',

    [switch]$SkipBuild,

    [switch]$SkipSmoke,

    [switch]$RequireInterop
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $repositoryRoot 'build/rdp017-validation'
} elseif (-not [IO.Path]::IsPathRooted($BuildRoot)) {
    $BuildRoot = Join-Path $repositoryRoot $BuildRoot
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$reportPath = Join-Path $BuildRoot 'RDP-017-report.md'
$runId = [Guid]::NewGuid().ToString('N').Substring(0, 8)

$results = [Collections.Generic.List[object]]::new()

function Add-Result {
    param(
        [Parameter(Mandatory)] [string]$Category,
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Status,
        [Parameter(Mandatory)] [string]$Details,
        [string]$LogPath = ''
    )

    $results.Add([pscustomobject]@{
        Category = $Category
        Name = $Name
        Status = $Status
        Details = $Details
        LogPath = $LogPath
    })
}

function Resolve-QMake {
    if (-not [string]::IsNullOrWhiteSpace($QMakePath)) {
        $resolved = Resolve-Path -LiteralPath $QMakePath -ErrorAction Stop
        return $resolved.Path
    }

    foreach ($commandName in @('qmake6.exe', 'qmake.exe', 'qmake6', 'qmake')) {
        $command = Get-Command $commandName -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
    }

    $candidates = @(Get-ChildItem -Path 'C:\Qt\*\msvc2022_64\bin\qmake.exe' -File `
            -ErrorAction SilentlyContinue)
    if ($candidates.Count -gt 0) {
        return ($candidates | Sort-Object {
                try { [version]$_.VersionInfo.FileVersion } catch { [version]'0.0' }
            } -Descending | Select-Object -First 1).FullName
    }

    throw 'Qt 6 qmake was not found. Pass -QMakePath with the MSVC 2022 64-bit qmake.exe path.'
}

function Resolve-NMake {
    $command = Get-Command 'nmake.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidates = @(Get-ChildItem `
            -Path 'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\HostX64\x64\nmake.exe' `
            -File -ErrorAction SilentlyContinue)
    if ($candidates.Count -eq 0) {
        throw "Required command 'nmake.exe' was not found."
    }
    return ($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}

function Initialize-MsvcEnvironment {
    if (-not [string]::IsNullOrWhiteSpace($env:INCLUDE) -and
        -not [string]::IsNullOrWhiteSpace($env:LIB)) {
        return
    }

    $vcVarsCandidates = @(Get-ChildItem `
            -Path 'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat' `
            -File -ErrorAction SilentlyContinue)
    if ($vcVarsCandidates.Count -eq 0) {
        throw 'The MSVC x64 developer environment is not initialized and vcvars64.bat was not found.'
    }

    $vcVarsPath = ($vcVarsCandidates | Sort-Object FullName | Select-Object -First 1).FullName
    $environmentLines = & $env:ComSpec /d /s /c "call `"$vcVarsPath`" >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat failed with exit code $LASTEXITCODE."
    }

    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -Path "Env:$name" -Value $value
    }

    if ([string]::IsNullOrWhiteSpace($env:INCLUDE) -or
        [string]::IsNullOrWhiteSpace($env:LIB)) {
        throw 'vcvars64.bat did not provide the required INCLUDE and LIB environment variables.'
    }
}

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string]$LogPath
    )

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments *> $LogPath
        $exitCode = $LASTEXITCODE
    } catch {
        $_ | Out-String | Add-Content -LiteralPath $LogPath -Encoding utf8
        $exitCode = 1
    } finally {
        Pop-Location
    }

    return $exitCode
}

function Invoke-QMakeBuild {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ProjectPath,
        [Parameter(Mandatory)] [string]$OutputDirectory,
        [Parameter(Mandatory)] [string]$BuildConfiguration,
        [Parameter(Mandatory)] [string]$QMake,
        [Parameter(Mandatory)] [string]$NMake
    )

    if ($SkipBuild) {
        Add-Result -Category 'Build' -Name "$Name ($BuildConfiguration)" -Status 'Skipped' `
            -Details 'Build was skipped by command-line option.'
        return $true
    }

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $logDirectory = Join-Path $BuildRoot 'logs'
    $safeName = ($Name -replace '[^A-Za-z0-9_.-]', '-')
    $qmakeLog = Join-Path $logDirectory "$safeName-$($BuildConfiguration.ToLowerInvariant())-qmake.log"
    $buildLog = Join-Path $logDirectory "$safeName-$($BuildConfiguration.ToLowerInvariant())-build.log"
    $configurationArgument = "CONFIG+=$($BuildConfiguration.ToLowerInvariant())"

    $qmakeExitCode = Invoke-LoggedCommand -FilePath $QMake `
        -Arguments @('-o', 'Makefile', $ProjectPath, $configurationArgument) `
        -WorkingDirectory $OutputDirectory -LogPath $qmakeLog
    if ($qmakeExitCode -ne 0) {
        Add-Result -Category 'Build' -Name "$Name ($BuildConfiguration)" -Status 'Failed' `
            -Details "qmake exited with code $qmakeExitCode." -LogPath $qmakeLog
        return $false
    }

    $buildExitCode = Invoke-LoggedCommand -FilePath $NMake -Arguments @('/NOLOGO') `
        -WorkingDirectory $OutputDirectory -LogPath $buildLog
    if ($buildExitCode -ne 0) {
        Add-Result -Category 'Build' -Name "$Name ($BuildConfiguration)" -Status 'Failed' `
            -Details "nmake exited with code $buildExitCode." -LogPath $buildLog
        return $false
    }

    Add-Result -Category 'Build' -Name "$Name ($BuildConfiguration)" -Status 'Passed' `
        -Details 'qmake generation and nmake build succeeded.' -LogPath $buildLog
    return $true
}

function Find-BuiltExecutable {
    param(
        [Parameter(Mandatory)] [string]$SearchRoot,
        [Parameter(Mandatory)] [string]$FileName,
        [Parameter(Mandatory)] [string]$BuildConfiguration
    )

    if (-not (Test-Path -LiteralPath $SearchRoot)) {
        return $null
    }

    $configurationDirectory = $BuildConfiguration.ToLowerInvariant()
    $matches = @(Get-ChildItem -LiteralPath $SearchRoot -Recurse -File -Filter $FileName `
            -ErrorAction SilentlyContinue | Where-Object {
                $_.Directory.Name -ieq $configurationDirectory
            } | Sort-Object LastWriteTime -Descending)
    if ($matches.Count -eq 0) {
        return $null
    }
    return $matches[0].FullName
}

function Invoke-AutomatedTest {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$ExecutablePath,
        [Parameter(Mandatory)] [string]$BuildConfiguration,
        [Parameter(Mandatory)] [string]$QtBinPath,
        [Parameter(Mandatory)] [string]$FreeRdpBinPath
    )

    if ([string]::IsNullOrWhiteSpace($ExecutablePath) -or
        -not (Test-Path -LiteralPath $ExecutablePath)) {
        Add-Result -Category 'Automated test' -Name "$Name ($BuildConfiguration)" `
            -Status 'Failed' -Details 'The test executable was not found.'
        return $false
    }

    $logPath = Join-Path (Join-Path $BuildRoot 'logs') `
        "test-$($Name.ToLowerInvariant())-$($BuildConfiguration.ToLowerInvariant()).log"
    $processLogPath = Join-Path (Join-Path $BuildRoot 'logs') `
        "test-$($Name.ToLowerInvariant())-$($BuildConfiguration.ToLowerInvariant())-process.log"
    $oldPath = $env:PATH
    $oldQpaPlatform = $env:QT_QPA_PLATFORM
    try {
        $env:PATH = "$QtBinPath;$FreeRdpBinPath;$([IO.Path]::GetDirectoryName($ExecutablePath));$oldPath"
        $env:QT_QPA_PLATFORM = 'offscreen'
        $exitCode = Invoke-LoggedCommand -FilePath $ExecutablePath `
            -Arguments @('-o', "$logPath,txt") `
            -WorkingDirectory ([IO.Path]::GetDirectoryName($ExecutablePath)) `
            -LogPath $processLogPath
    } finally {
        $env:PATH = $oldPath
        $env:QT_QPA_PLATFORM = $oldQpaPlatform
    }

    if ($exitCode -ne 0) {
        Add-Result -Category 'Automated test' -Name "$Name ($BuildConfiguration)" `
            -Status 'Failed' -Details "Test exited with code $exitCode." -LogPath $processLogPath
        return $false
    }

    if (-not (Test-Path -LiteralPath $logPath)) {
        Add-Result -Category 'Automated test' -Name "$Name ($BuildConfiguration)" `
            -Status 'Failed' -Details 'Qt Test did not create its result log.' `
            -LogPath $processLogPath
        return $false
    }

    $totals = Get-Content -LiteralPath $logPath | Where-Object { $_ -like 'Totals:*' } |
        Select-Object -Last 1
    $details = if ($totals) { $totals } else { 'Qt Test exited successfully.' }

    Add-Result -Category 'Automated test' -Name "$Name ($BuildConfiguration)" `
        -Status 'Passed' -Details $details -LogPath $logPath
    return $true
}

function Start-IsolatedProcess {
    param(
        [Parameter(Mandatory)] [string]$ExecutablePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string]$QtBinPath,
        [Parameter(Mandatory)] [string]$FreeRdpBinPath
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ExecutablePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    $startInfo.Environment['QT_QPA_PLATFORM'] = 'offscreen'
    $startInfo.Environment['PATH'] =
        "$QtBinPath;$FreeRdpBinPath;$([IO.Path]::GetDirectoryName($ExecutablePath));$env:PATH"

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Failed to start $ExecutablePath."
    }
    return $process
}

function Stop-IsolatedProcess {
    param([Diagnostics.Process]$Process)

    if (-not $Process) {
        return
    }
    if (-not $Process.HasExited) {
        $Process.Kill($true)
        $Process.WaitForExit(5000) | Out-Null
    }
    $Process.Dispose()
}

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Test-TcpListener {
    param(
        [Parameter(Mandatory)] [int]$Port,
        [Parameter(Mandatory)] [Diagnostics.Process]$Process,
        [int]$TimeoutMilliseconds = 10000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            return $false
        }
        $client = [Net.Sockets.TcpClient]::new()
        try {
            $connection = $client.ConnectAsync([Net.IPAddress]::Loopback, $Port)
            if ($connection.Wait(250) -and $client.Connected) {
                return $true
            }
        } catch {
            # The listener may not be ready yet.
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Invoke-ClientSmokeTest {
    param(
        [Parameter(Mandatory)] [string]$ExecutablePath,
        [Parameter(Mandatory)] [string]$BuildConfiguration,
        [Parameter(Mandatory)] [string]$QtBinPath,
        [Parameter(Mandatory)] [string]$FreeRdpBinPath
    )

    $process = $null
    try {
        $process = Start-IsolatedProcess -ExecutablePath $ExecutablePath `
            -WorkingDirectory ([IO.Path]::GetDirectoryName($ExecutablePath)) `
            -QtBinPath $QtBinPath -FreeRdpBinPath $FreeRdpBinPath
        Start-Sleep -Milliseconds 1500
        if ($process.HasExited) {
            $errorText = $process.StandardError.ReadToEnd().Trim()
            Add-Result -Category 'Smoke test' -Name "RDPClient startup ($BuildConfiguration)" `
                -Status 'Failed' -Details "Process exited early with code $($process.ExitCode): $errorText"
            return $false
        }

        Add-Result -Category 'Smoke test' -Name "RDPClient startup ($BuildConfiguration)" `
            -Status 'Passed' -Details 'The offscreen GUI process stayed alive during the startup probe.'
        return $true
    } finally {
        Stop-IsolatedProcess -Process $process
    }
}

function Invoke-ServerSmokeTest {
    param(
        [Parameter(Mandatory)] [string]$ExecutablePath,
        [Parameter(Mandatory)] [string]$BuildConfiguration,
        [Parameter(Mandatory)] [string]$QtBinPath,
        [Parameter(Mandatory)] [string]$FreeRdpBinPath
    )

    $smokeDirectory = Join-Path $BuildRoot `
        "smoke/$($BuildConfiguration.ToLowerInvariant())/server-$runId"
    New-Item -ItemType Directory -Force -Path $smokeDirectory | Out-Null
    Copy-Item -LiteralPath $ExecutablePath -Destination $smokeDirectory -Force
    Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($ExecutablePath)) -File -Filter '*.dll' |
        Copy-Item -Destination $smokeDirectory -Force

    $port = Get-FreeTcpPort
    $configurationText = @"
[Server]
ListenAddress=127.0.0.1
RdpPort=$port
Authentication=Disabled
Certificate=
PrivateKey=
NtlmSamFile=
Capture=Desktop
MaximumClientCount=1
LogLevel=Off
LogFile=RDPServer.log
"@
    [IO.File]::WriteAllText((Join-Path $smokeDirectory 'RDPServer.ini'),
        $configurationText, [Text.UTF8Encoding]::new($false))

    $isolatedExecutable = Join-Path $smokeDirectory 'RDPServer.exe'
    $process = $null
    try {
        $process = Start-IsolatedProcess -ExecutablePath $isolatedExecutable `
            -Arguments @('--background') -WorkingDirectory $smokeDirectory `
            -QtBinPath $QtBinPath -FreeRdpBinPath $FreeRdpBinPath
        if (-not (Test-TcpListener -Port $port -Process $process)) {
            $errorText = if ($process.HasExited) { $process.StandardError.ReadToEnd().Trim() } else { '' }
            Add-Result -Category 'Smoke test' -Name "RDPServer listener ($BuildConfiguration)" `
                -Status 'Failed' `
                -Details "The background process did not accept a loopback TCP connection on port $port. $errorText"
            return $false
        }

        Add-Result -Category 'Smoke test' -Name "RDPServer listener ($BuildConfiguration)" `
            -Status 'Passed' `
            -Details "The background process accepted a loopback TCP connection on port $port."
        return $true
    } finally {
        Stop-IsolatedProcess -Process $process
    }
}

function Convert-ToTableText {
    param([string]$Value)
    if ($null -eq $Value) {
        return ''
    }
    return ($Value -replace '\|', '\|' -replace "`r?`n", '<br>')
}

function Write-Report {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$OverallStatus,
        [Parameter(Mandatory)] [string]$QMake,
        [Parameter(Mandatory)] [string[]]$Configurations
    )

    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('# RDP-017 Windows 1차 Release 통합 테스트 결과')
    $lines.Add('')
    $lines.Add("- 실행 시각(UTC): $([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))")
    $lines.Add("- Repository: ``$repositoryRoot``")
    $lines.Add("- Configuration: $($Configurations -join ', ')")
    $lines.Add("- qmake: ``$QMake``")
    $lines.Add("- 종합 결과: **$OverallStatus**")
    $lines.Add('')
    $lines.Add('| 구분 | 항목 | 결과 | 상세 | 로그 |')
    $lines.Add('| --- | --- | --- | --- | --- |')
    foreach ($result in $results) {
        $relativeLog = ''
        if (-not [string]::IsNullOrWhiteSpace($result.LogPath)) {
            $relativeLog = [IO.Path]::GetRelativePath($BuildRoot, $result.LogPath)
        }
        $lines.Add("| $(Convert-ToTableText $result.Category) | " +
            "$(Convert-ToTableText $result.Name) | $(Convert-ToTableText $result.Status) | " +
            "$(Convert-ToTableText $result.Details) | $(Convert-ToTableText $relativeLog) |")
    }
    $lines.Add('')
    $lines.Add('## 수동 상호 운용성 Passed 판정 기준')
    $lines.Add('')
    $lines.Add('각 조합은 연결, Desktop 표시/갱신, keyboard, mouse 이동/button/wheel, ' +
        '양방향 Unicode text clipboard, 정상 disconnect를 모두 확인한 경우에만 Passed로 기록한다.')
    if (-not [string]::IsNullOrWhiteSpace($OperatorNote)) {
        $lines.Add('')
        $lines.Add("Operator note: $(Convert-ToTableText $OperatorNote)")
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllLines($Path, $lines, [Text.UTF8Encoding]::new($false))
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

try {
    if (-not $IsWindows) {
        throw 'RDP-017 validation must run on Windows.'
    }

    $qmake = Resolve-QMake
    $nmake = Resolve-NMake
    Initialize-MsvcEnvironment
    $qtBinPath = Split-Path -Parent $qmake
    $freeRdpBinPath = Join-Path $repositoryRoot 'build/freerdp-install/bin'
    if (-not (Test-Path -LiteralPath $freeRdpBinPath)) {
        throw "FreeRDP runtime directory was not found: $freeRdpBinPath"
    }

    $configurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
    $testProjects = @(
        @{ Name = 'RdpServerTest'; Project = 'RDPServer/tests/RDPServerTests.pro'; Exe = 'tst_rdpserver.exe' },
        @{ Name = 'RdpClientChannelTest'; Project = 'RDPClient/tests/rdpclient_channel_test.pro'; Exe = 'rdpclient_channel_test.exe' },
        @{ Name = 'ClipboardTextCodecTest'; Project = 'RDPClient/tests/clipboardtextcodec_test.pro'; Exe = 'clipboardtextcodec_test.exe' },
        @{ Name = 'ClipboardUiBridgeTest'; Project = 'RDPClient/tests/clipboarduibridge_test.pro'; Exe = 'clipboarduibridge_test.exe' }
    )

    foreach ($buildConfiguration in $configurations) {
        $configurationRoot = Join-Path $BuildRoot $buildConfiguration.ToLowerInvariant()
        $applicationBuildRoot = Join-Path $configurationRoot 'apps'
        $appsBuilt = Invoke-QMakeBuild -Name 'QtRdp applications' `
            -ProjectPath (Join-Path $repositoryRoot 'QtRdp.pro') `
            -OutputDirectory $applicationBuildRoot -BuildConfiguration $buildConfiguration `
            -QMake $qmake -NMake $nmake

        foreach ($testProject in $testProjects) {
            $testBuildRoot = Join-Path $configurationRoot "tests/$($testProject.Name)"
            $testBuilt = Invoke-QMakeBuild -Name $testProject.Name `
                -ProjectPath (Join-Path $repositoryRoot $testProject.Project) `
                -OutputDirectory $testBuildRoot -BuildConfiguration $buildConfiguration `
                -QMake $qmake -NMake $nmake
            if ($testBuilt) {
                $testExecutable = Find-BuiltExecutable -SearchRoot $testBuildRoot `
                    -FileName $testProject.Exe -BuildConfiguration $buildConfiguration
                Invoke-AutomatedTest -Name $testProject.Name -ExecutablePath $testExecutable `
                    -BuildConfiguration $buildConfiguration -QtBinPath $qtBinPath `
                    -FreeRdpBinPath $freeRdpBinPath | Out-Null
            }
        }

        if ($SkipSmoke) {
            Add-Result -Category 'Smoke test' -Name "Application startup ($buildConfiguration)" `
                -Status 'Skipped' -Details 'Smoke tests were skipped by command-line option.'
        } elseif ($appsBuilt) {
            $clientExecutable = Find-BuiltExecutable -SearchRoot $applicationBuildRoot `
                -FileName 'RDPClient.exe' -BuildConfiguration $buildConfiguration
            $serverExecutable = Find-BuiltExecutable -SearchRoot $applicationBuildRoot `
                -FileName 'RDPServer.exe' -BuildConfiguration $buildConfiguration
            if ($clientExecutable) {
                Invoke-ClientSmokeTest -ExecutablePath $clientExecutable `
                    -BuildConfiguration $buildConfiguration -QtBinPath $qtBinPath `
                    -FreeRdpBinPath $freeRdpBinPath | Out-Null
            } else {
                Add-Result -Category 'Smoke test' -Name "RDPClient startup ($buildConfiguration)" `
                    -Status 'Failed' -Details 'RDPClient.exe was not found after the application build.'
            }
            if ($serverExecutable) {
                Invoke-ServerSmokeTest -ExecutablePath $serverExecutable `
                    -BuildConfiguration $buildConfiguration -QtBinPath $qtBinPath `
                    -FreeRdpBinPath $freeRdpBinPath | Out-Null
            } else {
                Add-Result -Category 'Smoke test' -Name "RDPServer listener ($buildConfiguration)" `
                    -Status 'Failed' -Details 'RDPServer.exe was not found after the application build.'
            }
        }
    }

    $mstscPath = Join-Path $env:SystemRoot 'System32/mstsc.exe'
    Add-Result -Category 'Interop preflight' -Name 'Windows Remote Desktop client' `
        -Status $(if (Test-Path -LiteralPath $mstscPath) { 'Passed' } else { 'Unavailable' }) `
        -Details $(if (Test-Path -LiteralPath $mstscPath) { $mstscPath } else { 'mstsc.exe was not found.' })

    $termService = Get-Service -Name 'TermService' -ErrorAction SilentlyContinue
    Add-Result -Category 'Interop preflight' -Name 'Windows Remote Desktop server' `
        -Status $(if ($termService -and $termService.Status -eq 'Running') { 'Passed' } else { 'Unavailable' }) `
        -Details $(if ($termService) { "TermService status: $($termService.Status)" } else { 'TermService was not found.' })

    $windowsRdpListeners = @(Get-NetTCPConnection -State Listen -LocalPort 3389 `
            -ErrorAction SilentlyContinue)
    Add-Result -Category 'Interop preflight' -Name 'Windows RDP default listener' `
        -Status $(if ($windowsRdpListeners.Count -gt 0) { 'Passed' } else { 'Unavailable' }) `
        -Details $(if ($windowsRdpListeners.Count -gt 0) {
                "Listening endpoints: $((@($windowsRdpListeners.LocalAddress | Sort-Object -Unique) -join ', ')):3389"
            } else { 'No local listener was found on TCP port 3389.' })

    Add-Result -Category 'Manual interoperability' -Name 'QtRdp Client → Windows RDP Server' `
        -Status $QtClientToWindowsServer `
        -Details 'Operator-supplied result; all client checklist items must pass.'
    Add-Result -Category 'Manual interoperability' -Name 'Windows RDP Client → QtRdp Server' `
        -Status $WindowsClientToQtServer `
        -Details 'Operator-supplied result; all server checklist items must pass.'

    $automatedFailures = @($results | Where-Object {
            $_.Category -in @('Build', 'Automated test', 'Smoke test') -and $_.Status -eq 'Failed'
        })
    $manualFailed = $QtClientToWindowsServer -eq 'Failed' -or
        $WindowsClientToQtServer -eq 'Failed'
    $manualPassed = $QtClientToWindowsServer -eq 'Passed' -and
        $WindowsClientToQtServer -eq 'Passed'

    $overallStatus = if ($automatedFailures.Count -gt 0 -or $manualFailed) {
        'Failed'
    } elseif ($manualPassed) {
        'Passed'
    } else {
        'Incomplete — automated gate passed, manual interoperability is not complete'
    }
    Write-Report -Path $reportPath -OverallStatus $overallStatus -QMake $qmake `
        -Configurations $configurations
    Write-Host "RDP-017 report: $reportPath"
    Write-Host "Overall status: $overallStatus"

    if ($automatedFailures.Count -gt 0 -or $manualFailed) {
        exit 1
    }
    if ($RequireInterop -and -not $manualPassed) {
        exit 2
    }
    exit 0
} catch {
    Add-Result -Category 'Harness' -Name 'Release validation' -Status 'Failed' `
        -Details $_.Exception.Message
    $fallbackQMake = if ([string]::IsNullOrWhiteSpace($QMakePath)) { 'unresolved' } else { $QMakePath }
    $fallbackConfigurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
    Write-Report -Path $reportPath -OverallStatus 'Failed' -QMake $fallbackQMake `
        -Configurations $fallbackConfigurations
    Write-Error "RDP-017 validation failed: $($_.Exception.Message). Report: $reportPath"
    exit 1
}
