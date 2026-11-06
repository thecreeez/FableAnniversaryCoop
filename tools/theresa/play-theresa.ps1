param(
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$FixtureSet = 'mp',
    [string]$Save = 'AutoSave',
    [switch]$Check
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$launcher = Join-Path $root 'src\bin\Release\AlbionTogether.Launcher.exe'
$client = Join-Path $root 'src\bin\Release\AlbionTogether.Client.dll'
$game = Join-Path $root 'test-install'
$documents = Join-Path $root ('fixtures\' + $FixtureSet + '\guest\Documents')
foreach ($path in @($launcher, $client, (Join-Path $game 'Binaries\Win32\Fable Anniversary.exe'),
    (Join-Path $root 'src\bin\Release\theresa-garment-geometry.bin'),
    (Join-Path $game 'WellingtonGame\CookedPC\OrganizedForCooking\WeSISTER_C1_SF.upk'), $documents)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required preview file or folder is missing: $path" }
}
Write-Output "Theresa preview: one game client, fixture=$FixtureSet, save=$Save"
Write-Output 'The launcher copies the fixture into a new isolated profile. Close the game normally when finished.'
if ($Check) { Write-Output 'Preview launch files verified. No game started.'; exit 0 }
if (Get-Process -Name 'Fable Anniversary' -ErrorAction SilentlyContinue) { throw 'Close existing game clients before starting the single-client preview.' }
$launchArgs = @('--game-dir', $game, '--automation', 'theresa_preview', '--fixture-documents', $documents,
    '--host-save', $Save, '--timeout', '180', '--no-console')
$process = Start-Process -FilePath $launcher -ArgumentList $launchArgs -WorkingDirectory (Split-Path $launcher) `
    -NoNewWindow -PassThru -Wait
if ($process.ExitCode -ne 0) { throw "Theresa preview launcher failed: exit $($process.ExitCode)" }
Write-Output 'Game left running for manual inspection.'
