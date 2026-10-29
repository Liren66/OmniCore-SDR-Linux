# Reboots the board and measures, from the reboot command: USB re-enumeration, first
# successful `och info`, ping, SSH and first libiio context. Then prints systemd-analyze.
param([string]$Board = '192.168.2.1', [string]$Out = (Join-Path $PSScriptRoot 'results\boot_timing.txt'))
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$t = [Diagnostics.Stopwatch]::StartNew()
ssh -o BatchMode=yes OmniCore@$Board 'systemctl reboot' 2>$null
Start-Sleep 3
$marks = [ordered]@{}
while ($t.Elapsed.TotalSeconds -lt 180 -and $marks.Count -lt 4) {
    if (-not $marks.Contains('och_ready')) {
        och info --json 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $marks['och_ready'] = $t.Elapsed.TotalSeconds }
    }
    if (-not $marks.Contains('ping')) {
        if (Test-Connection $Board -Count 1 -TimeoutSeconds 1 -Quiet) { $marks['ping'] = $t.Elapsed.TotalSeconds }
    }
    if ($marks.Contains('ping') -and -not $marks.Contains('ssh')) {
        ssh -o BatchMode=yes -o ConnectTimeout=2 OmniCore@$Board true 2>$null
        if ($LASTEXITCODE -eq 0) { $marks['ssh'] = $t.Elapsed.TotalSeconds }
    }
    if ($marks.Contains('och_ready') -and $marks.Contains('ping') -and -not $marks.Contains('libiio')) {
        Start-Sleep 4   # quiet_ms after the och probe
        iio_attr -u "ip:$Board" -c ad9361-phy RX_LO frequency 2>$null | Out-Null
        if ($LASTEXITCODE -eq 0) { $marks['libiio'] = $t.Elapsed.TotalSeconds }
    }
    Start-Sleep -Milliseconds 300
}
$marks.GetEnumerator() | % { '{0,-10} {1,6:N1} s' -f $_.Key, $_.Value } | Tee-Object -FilePath $Out
Start-Sleep 8
ssh OmniCore@$Board 'systemd-analyze; systemd-analyze critical-chain omnicored.service | sed -n 3,12p; systemd-analyze blame | head -12; journalctl -b -u omnicored -o short-monotonic --no-pager | head -5' | Tee-Object -FilePath $Out -Append
