# OmniCore SDR Linux: USB/libiio switching with OCH priority.
#   1  libiio takes the idle AD9361 (time to first reply)
#   2  OCH command during a running libiio capture: preempts, runs, capture is cut
#   3  libiio while USB streams: refused with EBUSY (16), bench unaffected
#   4  libiio throughput through the splice proxy (2 and 4 channels)
# TX stays muted (89.75 dB) throughout.
param([string]$Board = '192.168.2.1', [string]$Out = (Join-Path $PSScriptRoot 'results'))
New-Item -ItemType Directory -Force $Out | Out-Null
$Uri = "ip:$Board"
$res = [ordered]@{}
function Rec($k, $v) { $res[$k] = $v; Write-Host ("[{0}] {1}" -f $k, $v) }

och selftest --json 2>&1 | Out-Null
Rec 'usb_selftest' "rc=$LASTEXITCODE"

# 1
Start-Sleep 4
$t = [Diagnostics.Stopwatch]::StartNew()
$lo = iio_attr -u $Uri -c ad9361-phy RX_LO frequency 2>&1
Rec 'iio_takeover' "rx_lo=$lo rc=$LASTEXITCODE $([int]$t.ElapsedMilliseconds) ms"

# 2
$cap = Join-Path $Out 'preempt_capture.bin'
$job = Start-Job { param($u, $f)
    $t = [Diagnostics.Stopwatch]::StartNew()
    cmd /c "iio_readdev -u $u -b 1048576 -s 268435456 cf-ad9361-lpc voltage0 voltage1 voltage2 voltage3 > `"$f`" 2>&1"
    "rc=$LASTEXITCODE after $([int]$t.ElapsedMilliseconds) ms"
} -ArgumentList $Uri, $cap
Start-Sleep 3
$t = [Diagnostics.Stopwatch]::StartNew()
$temp = och get core.temperature --json 2>&1 | Out-String
$ochMs = [int]$t.ElapsedMilliseconds
Rec 'och_preempt' ("rc=$LASTEXITCODE $ochMs ms valid=" + ($temp -match 'valid=1'))
Wait-Job $job -Timeout 60 | Out-Null
Rec 'iio_capture_cut' ((Receive-Job $job) -join ' ')
Remove-Job $job -Force; Remove-Item $cap -ErrorAction SilentlyContinue
och selftest --json 2>&1 | Out-File (Join-Path $Out 'selftest_after_preempt.json')
Rec 'usb_selftest_after' "rc=$LASTEXITCODE"

# 3
$bench = Start-Job { och bench --rx --tx --channels '1,2' --sample-rate 30.72M --seconds 8 --amplitude 0 --attenuation 89.75 --json 2>&1 }
Start-Sleep 3
$msg = iio_attr -u $Uri -c ad9361-phy RX_LO frequency 2>&1 | Out-String
Rec 'iio_refused' ("rc=$LASTEXITCODE " + $msg.Trim())
Wait-Job $bench -Timeout 60 | Out-Null
$b = Receive-Job $bench | Out-String; Remove-Job $bench -Force
$b | Out-File (Join-Path $Out 'bench_during_refusal.json')
Rec 'usb_bench' $(if ($b -match '"pass":true') { 'pass=true' } else { 'pass=false' })

# 4
Start-Sleep 4
$hold = Start-Job { param($b) $s = New-Object Net.Sockets.TcpClient($b, 30431); Start-Sleep 60; $s.Close() } -ArgumentList $Board
Start-Sleep 4
foreach ($ch in 'voltage0 voltage1', 'voltage0 voltage1 voltage2 voltage3') {
    $n = $ch.Split(' ').Count
    $t = [Diagnostics.Stopwatch]::StartNew()
    cmd /c "iio_readdev -u $Uri -b 1048576 -s 33554432 cf-ad9361-lpc $ch > NUL"
    Rec "iio_rx_${n}ch" ("{0:N1} MB/s rc=$LASTEXITCODE" -f ($n * 2 * 33554432 / 1e6 / $t.Elapsed.TotalSeconds))
}
Stop-Job $hold; Remove-Job $hold -Force
$res | ConvertTo-Json | Out-File (Join-Path $Out 'switching_results.json')
ssh OmniCore@$Board 'journalctl -u omnicored -b --no-pager -o cat | tail -40' | Out-File (Join-Path $Out 'omnicored_journal.txt')
