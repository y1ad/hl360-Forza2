param(
 # host/guest pick the direct-pair addresses; with -Relay any short name works
 # (e.g. p1..p4), each with its own profile and data.
 [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9]{1,10}$')][string]$Role,
 [string]$LocalIp,
 [string]$PeerIp,
 [string]$GameDataRoot,
 [switch]$Background,
 [switch]$SelfTest,
 [switch]$ScriptedOnly,
 [string]$PadFile,
 # host:port of an FM2 relay (tools/fm2-relay); replaces the direct pair.
 [string]$Relay,
 # Extra game flags, e.g. --frame_stats=true
 [string[]]$Extra=@()
)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot
$taskExe=Join-Path $taskRoot 'out\build\win-amd64-vulkan\forza2.exe'
if(-not (Test-Path -LiteralPath $taskExe)) {throw 'Build FM2 first with build-vulkan.bat'}
if(-not $Relay -and $Role -notin @('host','guest') -and -not ($LocalIp -and $PeerIp)) {throw 'Roles other than host/guest need -Relay (or -LocalIp and -PeerIp)'}
if(-not $LocalIp) {$LocalIp=if($Role -eq 'host') {'127.0.0.2'} else {'127.0.0.3'}}
if(-not $PeerIp) {$PeerIp=if($Role -eq 'host') {'127.0.0.3'} else {'127.0.0.2'}}
foreach($taskIp in @($LocalIp,$PeerIp)) {
 $taskParsed=$null
 if(-not [Net.IPAddress]::TryParse($taskIp,[ref]$taskParsed) -or $taskParsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {throw "Invalid IPv4: $taskIp"}
}
if(-not $GameDataRoot) {$GameDataRoot=Join-Path $taskRoot 'assets'}
if(-not (Test-Path -LiteralPath (Join-Path $GameDataRoot 'default.xex'))) {throw 'Pass -GameDataRoot with the imported game directory'}
$taskLab=Join-Path $taskRoot "out\lan-lab\$Role"
New-Item -ItemType Directory -Force $taskLab | Out-Null
$taskUser=Join-Path $taskLab 'user'
$taskCache=Join-Path $taskLab 'cache'
$taskLog=Join-Path $taskLab ("session-"+[Guid]::NewGuid().ToString("N")+".log")
$taskNet=if($Relay) {@("--fm2_relay=$Relay")} else {@('--fm2_lan=true',"--fm2_lan_ip=$LocalIp","--fm2_lan_peer=$PeerIp")}
$taskArgs=@($taskNet)+@('--fm2_lan_port=2302',"--game_data_root=$GameDataRoot","--user_data_root=$taskUser","--cache_root=$taskCache","--gamertag=LAN$Role",'--fullscreen=false','--window_width=960','--window_height=540','--video_mode_refresh_rate=60','--host_present_fps_limit=60','--resolution_scale=1','--log_level=info',"--log_file=$taskLog")
$taskArgs+=$Extra
if($SelfTest) {$taskArgs+="--fm2_lan_selftest=true"}
if($ScriptedOnly -and -not $PadFile) {throw "-ScriptedOnly requires -PadFile"}
if($PadFile) {$taskArgs+="--scripted_input=$PadFile"}
if($ScriptedOnly) {$taskArgs+="--scripted_input_exclusive=true"}
# Quote each argument because Start-Process passes a single Windows command line.
$taskArguments=($taskArgs | ForEach-Object {'"'+$_.Replace('"','\"')+'"'}) -join ' '
if($Background) {$taskProcess=Start-Process -FilePath $taskExe -WorkingDirectory (Split-Path $taskExe) -ArgumentList $taskArguments -PassThru -WindowStyle Hidden}
else {$taskProcess=Start-Process -FilePath $taskExe -WorkingDirectory (Split-Path $taskExe) -ArgumentList $taskArguments -PassThru}
[pscustomobject]@{Role=$Role;ProcessId=$taskProcess.Id;LocalIp=$LocalIp;PeerIp=$PeerIp;Relay=$Relay;Log=$taskLog;Data=$taskUser}
