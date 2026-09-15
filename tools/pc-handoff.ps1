param([ValidateSet('open')][string]$Action, [switch]$Validate)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
try {
    $requestText = [Console]::In.ReadToEnd()
    if ($requestText.Length -gt 8192) { throw 'Invalid request' }
    $request = ConvertFrom-Json -InputObject $requestText
    $url = [string]$request.url
    $parsed = $null
    if ($url.Length -gt 4096 -or $url -match '[\x00-\x20\\"''<>`]' -or
        -not [Uri]::TryCreate($url,[UriKind]::Absolute,[ref]$parsed) -or $parsed.Scheme -ne 'https' -or
        -not $parsed.Host -or $parsed.UserInfo) { throw 'Invalid URL' }
    if (-not $Validate) {
        $browser = [Diagnostics.ProcessStartInfo]::new()
        $browser.FileName = $url
        $browser.UseShellExecute = $true
        [void][Diagnostics.Process]::Start($browser)
    }
    [Console]::Out.Write('{"ok":true}')
    exit 0
} catch {
    [Console]::Out.Write('{"ok":false,"error":"browser_unavailable"}')
    exit 1
}
