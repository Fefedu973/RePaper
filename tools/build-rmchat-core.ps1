[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [Parameter(Mandatory = $true)]
    [string]$OutputFile,
    [ValidateSet('amd64', 'arm64')]
    [string]$TargetArchitecture = 'amd64'
)

$ErrorActionPreference = 'Stop'
$goBinary = Join-Path $env:ProgramFiles 'Go/bin/go.exe'
if (-not (Test-Path -LiteralPath $goBinary -PathType Leaf)) {
    throw 'Windows Go was not found in Program Files/Go. Install Go 1.25+ or select native Linux Go with GO_BIN.'
}
if (-not [IO.Path]::IsPathRooted($SourceDirectory) -or -not [IO.Path]::IsPathRooted($OutputFile)) {
    throw 'Go source and output paths must be absolute.'
}
$sourcePath = (Resolve-Path -LiteralPath $SourceDirectory).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $sourcePath 'go.mod') -PathType Leaf)) {
    throw 'The RMChat Go module is missing.'
}
if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($OutputFile)) -PathType Container)) {
    throw 'The output directory must already exist.'
}

# These settings belong to this invocation only. No persistent go env is changed.
$buildEnvironment = @{
    GOOS = 'linux'; GOARCH = $TargetArchitecture; CGO_ENABLED = '0'; GOTOOLCHAIN = 'local'
    GOPROXY = 'off'; GOSUMDB = 'off'; GONOPROXY = 'none'; GOVCS = '*:off'
    GOWORK = 'off'; GOENV = 'off'; GOFLAGS = ''
}
$previousEnvironment = @{}
try {
    foreach ($name in $buildEnvironment.Keys) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $buildEnvironment[$name], 'Process')
    }
    $arguments = @('-C', $sourcePath, 'build', '-mod=readonly', '-trimpath', '-buildvcs=false',
                   '-o', $OutputFile, './cmd/rmchat-core')
    & $goBinary @arguments
    if ($LASTEXITCODE -ne 0) { throw 'The Windows Go cross-compilation failed.' }
} finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}
