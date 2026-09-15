[CmdletBinding()]
param(
    [ValidateSet('menu','remoodle','reagenda','restencil','recalc','reink','rmchat')]
    [string]$App = 'menu',
    [ValidateRange(1.0,2.0)]
    [double]$UiScale = 1.5,
    [switch]$Rebuild,
    [switch]$RegisterApps,
    [string]$AppBuildDirectory = '/root/repaper-build',
    [string]$Distribution = 'Ubuntu'
)
$ErrorActionPreference = 'Stop'
if ($App -eq 'rmchat') { throw 'RMChat est archivé et retiré du lanceur standard. Sources et données conservées ; réactivation de recherche explicite : apps/rmchat/ARCHIVED.md.' }
if ($Rebuild -and $RegisterApps) { throw 'Choisissez -Rebuild ou -RegisterApps, pas les deux.' }
$repoPath = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$windowsForwardPath = $repoPath.Replace('\', '/')
$linuxRepo = [string](& wsl.exe -d $Distribution -u root --exec wslpath -a $windowsForwardPath)
$linuxRepo = $linuxRepo.Trim()
if ($LASTEXITCODE -ne 0 -or -not $linuxRepo) { throw 'Impossible de trouver ce dossier dans WSL.' }
$stateDirectory = '/root/repaper-appload'
$sandboxDirectory = '/root/repaper-emulator'
$buildDirectory = $AppBuildDirectory
$requiredProfile = Get-Content -LiteralPath (Join-Path $repoPath 'packaging/appload/profile-version.json') -Raw | ConvertFrom-Json

& wsl.exe -d $Distribution -u root --exec python3 "$linuxRepo/tools/emulator-apps.py" --state-dir $stateDirectory check-available
if ($LASTEXITCODE -ne 0) { throw 'Ce profil AppLoad est déjà ouvert ou en cours de préparation. Fermez sa fenêtre avant de le relancer.' }

& wsl.exe -d $Distribution -u root --exec test -f "$stateDirectory/build/appload"
$setupMissing = $LASTEXITCODE -ne 0
if (-not $setupMissing) {
    & wsl.exe -d $Distribution -u root --exec test -f "$stateDirectory/build-info.json"
    $setupMissing = $LASTEXITCODE -ne 0
    if (-not $setupMissing) {
        $buildInfoText = & wsl.exe -d $Distribution -u root --exec cat "$stateDirectory/build-info.json"
        try { $buildInfo = ($buildInfoText -join "`n") | ConvertFrom-Json }
        catch { $buildInfo = $null }
        $setupMissing = $LASTEXITCODE -ne 0 -or $null -eq $buildInfo -or
            $buildInfo.profile_version -ne $requiredProfile.profileVersion -or
            $buildInfo.display_version -ne $requiredProfile.displayVersion -or
            $buildInfo.qtfb_socket -ne "$stateDirectory/runtime/qtfb.sock"
    }
}
if ($RegisterApps) {
    if ($setupMissing) { throw "Préparez d'abord ce profil AppLoad avant d'enregistrer de nouvelles applications." }
    $registerArguments = @('-d', $Distribution, '-u', 'root', '--exec', 'python3', "$linuxRepo/tools/emulator-apps.py", '--state-dir', $stateDirectory, 'register', '--app-build', $buildDirectory)
    if ($App -ne 'menu') { $registerArguments += @('--app', $App) }
    & wsl.exe @registerArguments
    if ($LASTEXITCODE -ne 0) { throw "L'enregistrement des applications a échoué." }
    return
}
if ($Rebuild -or $setupMissing) {
    & wsl.exe -d $Distribution -u root --exec env "REPAPER_BUILD_DIR=$buildDirectory" bash "$linuxRepo/tools/build-pc.sh"
    if ($LASTEXITCODE -ne 0) { throw 'La compilation ou les tests ont échoué.' }
    & wsl.exe -d $Distribution -u root --exec python3 "$linuxRepo/tools/emulator-apps.py" --state-dir $stateDirectory --sandbox-dir $sandboxDirectory setup --app-build $buildDirectory
    if ($LASTEXITCODE -ne 0) { throw 'La préparation AppLoad a échoué.' }
}
$scaleText = $UiScale.ToString([Globalization.CultureInfo]::InvariantCulture)
$launchArguments = @('-d', $Distribution, '-u', 'root', '--exec', 'python3', "$linuxRepo/tools/emulator-apps.py", '--state-dir', $stateDirectory, '--sandbox-dir', $sandboxDirectory, 'launch', '--ui-scale', $scaleText)
if ($App -ne 'menu') { $launchArguments += @('--app', $App) }
& wsl.exe @launchArguments
if ($LASTEXITCODE -ne 0) { throw "L'émulateur a échoué. Journal : $stateDirectory/emulator.log" }
