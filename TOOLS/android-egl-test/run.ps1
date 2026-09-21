#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$Gradle,
    [Parameter(Mandatory)][string]$FFmpegInclude,
    [Parameter(Mandatory)][string]$PlaceboInclude,
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$ContextSource = (Join-Path $PSScriptRoot '../../video/out/opengl/context_android.c'),
    [ValidateSet('x86_64', 'arm64-v8a', 'armeabi-v7a')][string]$Abi = 'x86_64',
    [ValidateSet('firstFrameAndPausedRedrawKeepDrawableAligned',
        'continuousResizePreservesSurfaceAndFrameSubmission',
        'pendingColorResizeAndWindowReattachKeepContext',
        'detachedWindow_nextProducerFollowsConsumerSize',
        'releasedWindow_nextProducerFollowsConsumerSize',
        'replacedWindow_nextProducerFollowsConsumerSize')][string]$TestMethod,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
if ($Serial -notmatch '^[a-zA-Z0-9_.:-]+$') { throw 'Unsupported device serial characters' }
foreach ($path in $AndroidSdk, $ContextSource, $FFmpegInclude, $PlaceboInclude, $Gradle) {
    if (!$path -or !(Test-Path -LiteralPath $path)) { throw "Missing input: $path" }
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$sourcePath = (Resolve-Path -LiteralPath $ContextSource).Path
$sourceHash = (Get-FileHash -LiteralPath $sourcePath).Hash
$adb = Join-Path $AndroidSdk 'platform-tools/adb.exe'
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$env:ANDROID_HOME = (Resolve-Path -LiteralPath $AndroidSdk).Path
$gradleArguments = @('-p', $PSScriptRoot, '--project-cache-dir', "$outputPath/cache",
    "-PoutputDir=$outputPath", "-PffmpegInclude=$FFmpegInclude",
    "-PplaceboInclude=$PlaceboInclude", "-PcontextSource=$sourcePath", "-Pabi=$Abi",
    '--max-workers=2', '--console=plain', 'assembleDebug', 'assembleDebugAndroidTest')
& $Gradle @gradleArguments *> "$outputPath/build.log"
if ($LASTEXITCODE -ne 0) {
    Get-Content -LiteralPath "$outputPath/build.log" -Tail 50
    throw 'Test APK build failed; no package was installed'
}
if ((Get-FileHash -LiteralPath $sourcePath).Hash -ne $sourceHash) {
    throw 'Backend source changed during build'
}
$app = "$outputPath/build/outputs/apk/debug/mpv-egl-test-debug.apk"
$test = "$outputPath/build/outputs/apk/androidTest/debug/mpv-egl-test-debug-androidTest.apk"
Get-FileHash -LiteralPath $sourcePath, $app, $test |
    Export-Csv -NoTypeInformation -LiteralPath "$outputPath/hashes.csv"
if ($BuildOnly) { return }
& $adb -s $Serial get-state
if ($LASTEXITCODE -ne 0) { throw 'The selected device is not online' }
$sdk = & $adb -s $Serial shell getprop ro.build.version.sdk
$abis = & $adb -s $Serial shell getprop ro.product.cpu.abilist
if ([int]$sdk -lt 28 -or $Abi -notin $abis.Trim().Split(',')) {
    throw 'Device does not support this test API/ABI'
}
$package = 'org.mpv.egl.testapp'
$testPackage = "$package.test"
try {
    foreach ($apk in $app, $test) {
        & $adb -s $Serial install -r -t $apk
        if ($LASTEXITCODE -ne 0) { throw "Test package installation failed: $apk" }
    }
    $class = 'org.mpv.egl.EglResizeTest'
    $testCount = 6
    if ($TestMethod) { $class += "#$TestMethod"; $testCount = 1 }
    $process = Start-Process -FilePath $adb -WindowStyle Hidden -PassThru -ArgumentList @(
        '-s', $Serial, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', $class,
        "$testPackage/androidx.test.runner.AndroidJUnitRunner") `
        -RedirectStandardOutput "$outputPath/instrumentation.txt" `
        -RedirectStandardError "$outputPath/instrumentation-error.txt"
    try {
        if (!$process.WaitForExit(90000)) {
            $process.Kill()
            throw 'Instrumentation exceeded 90 seconds'
        }
        $process.WaitForExit()
        $result = Get-Content -Raw -LiteralPath "$outputPath/instrumentation.txt"
        $result
        & $adb -s $Serial pull "/sdcard/Android/data/$package/files" "$outputPath/screenshots"
        $captureExit = $LASTEXITCODE
        $expected = '(?m)^OK \(' + $testCount + ' tests?\)\s*$'
        if ($process.ExitCode -ne 0 -or $result -notmatch $expected -or
            $result -match 'FAILURES!!!|INSTRUMENTATION_FAILED|Process crashed') {
            throw 'EGL regression failed; inspect instrumentation.txt and screenshots'
        }
        if ($captureExit -ne 0) { throw 'Failed to collect compositor screenshots' }
    } finally { $process.Dispose() }
} finally {
    & $adb -s $Serial shell am force-stop $package
    foreach ($name in $testPackage, $package) {
        & $adb -s $Serial uninstall $name
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not remove temporary test package $name" }
    }
}
"PASS: $testCount EGL tests; source SHA256 $sourceHash; evidence in $outputPath"
