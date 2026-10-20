# Real runtime transport check. Uses two isolated FM2 instances, not player saves.
param([string]$GameDataRoot)
$ErrorActionPreference='Stop'
$taskProcesses=@()
$taskInfos=@()
try {
 foreach($role in @('host','guest')) {
  $taskParams=@{Role=$role;Background=$true;SelfTest=$true}
  if($GameDataRoot) {$taskParams.GameDataRoot=$GameDataRoot}
  $info=& (Join-Path $PSScriptRoot 'lan-lab.ps1') @taskParams
  $taskInfos+=$info
  $taskProcesses+=Get-Process -Id $info.ProcessId
 }
 foreach($p in $taskProcesses) {if(-not $p.WaitForExit(20000)) {throw 'LAN transport test timed out'}}
 foreach($info in $taskInfos) {
  $log=[IO.File]::ReadAllText($info.Log)
  if($log -notmatch 'FM2 LAN TRANSPORT SELFTEST PASSED') {throw "Transport failed for $($info.Role). See $($info.Log)"}
  Write-Output "PASS $($info.Role): $($info.LocalIp) <-> $($info.PeerIp)"
 }
 Write-Output 'Both game processes exchanged transport-test discovery and acknowledgements. This does not validate a multiplayer race.'
} finally {
 foreach($p in $taskProcesses) {if(-not $p.HasExited) {Stop-Process -Id $p.Id}}
}
