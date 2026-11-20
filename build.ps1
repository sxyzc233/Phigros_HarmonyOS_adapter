param(
    [string]$DevEcoHome = $env:DEVECO_STUDIO_HOME,
    [string]$HvigorModules = $env:HVIGOR_MODULES
)
$ErrorActionPreference = 'Stop'
if (-not $DevEcoHome -or -not (Test-Path -LiteralPath $DevEcoHome)) {
    throw 'Set DEVECO_STUDIO_HOME or pass -DevEcoHome with your DevEco Studio installation directory.'
}
$env:DEVECO_SDK_HOME = Join-Path $DevEcoHome 'sdk'
$env:JAVA_HOME = Join-Path $DevEcoHome 'jbr'
$env:HVIGOR_USER_HOME = Join-Path $PSScriptRoot '.hvigor-user'
if ($HvigorModules) {
    $env:NODE_PATH = $HvigorModules
    # TypeScript's resolver also needs a normal node_modules entry.
    # The link stays local and is excluded from Git.
    $taskPluginRoot = Join-Path $PSScriptRoot 'node_modules/@ohos'
    if (-not (Test-Path -LiteralPath $taskPluginRoot)) {
        New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'node_modules') -Force | Out-Null
        New-Item -ItemType Junction -Path $taskPluginRoot -Target (Join-Path $HvigorModules '@ohos') | Out-Null
    }
} else {
    # The IDE ships Hvigor and its plugin as siblings, outside Node's normal package layout.
    $toolRoot = Join-Path $DevEcoHome 'tools/hvigor'
    $projectModules = Join-Path $PSScriptRoot 'node_modules'
    foreach ($package in 'hvigor', 'hvigor-ohos-plugin') {
        $bundled = Join-Path $toolRoot $package
        $localPackage = Join-Path $projectModules "@ohos/$package"
        if ((Test-Path -LiteralPath (Join-Path $bundled 'package.json')) -and
            -not (Test-Path -LiteralPath $localPackage)) {
            New-Item -ItemType Directory -Path (Split-Path $localPackage) -Force | Out-Null
            New-Item -ItemType Junction -Path $localPackage -Target $bundled | Out-Null
        }
    }
    $env:NODE_PATH = "$projectModules;$env:NODE_PATH"
}
$env:PATH = (Join-Path $env:JAVA_HOME 'bin') + ';' + (Join-Path $DevEcoHome 'tools/node') + ';' + (Join-Path $DevEcoHome 'tools/ohpm/bin') + ';' + $env:PATH
Push-Location $PSScriptRoot
try {
    & (Join-Path $DevEcoHome 'tools/node/node.exe') (Join-Path $DevEcoHome 'tools/hvigor/hvigor/bin/hvigor.js') assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "Hvigor failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
