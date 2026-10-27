# OmniCore SDR Linux soak test (OCH has priority over libiio).
#   Part A: 60 s full-duplex USB benches (TX muted: amplitude 0, 89.75 dB).
#   Part B: rounds of  libiio takeover -> LO set/readback -> 8 MiB IIO capture ->
#           OCH command preempts libiio -> selftest -> 5 s full-duplex bench.
# Records startup RX overflows per bench (startup overflows after a handover are expected, see README).
param([string]$Board = '192.168.2.1', [int]$Rounds = 10, [int]$BenchSeconds = 60,
      [string]$Out = (Join-Path $PSScriptRoot 'results\soak'))
New-Item -ItemType Directory -Force $Out | Out-Null
$Uri = "ip:$Board"
$log = Join-Path $Out 'soak_log.txt'
Remove-Item $log -ErrorAction SilentlyContinue
function Log($s) { $line = "{0:HH:mm:ss} {1}" -f (Get-Date), $s; Write-Host $line; Add-Content $log $line }
function BenchPass($file) {
    $last = Get-Content $file | Where-Object { $_ -match '"command":"bench"' } | Select-Object -Last 1
    if (-not $last) { return 'pass=missing' }
    $j = $last | ConvertFrom-Json
    "pass=$($j.stability.pass) rx=$([math]::Round($j.rx.usb_MBps,2))MB/s tx=$([math]::Round($j.tx.usb_MBps,2))MB/s " +
    "rx_ovf=$($j.rx.device_overflows) tx_unf=$($j.tx.device_underflows) drop=$($j.rx.host_dropped_samples) xfer_err=$($j.rx.transfer_errors + $j.tx.transfer_errors)"
}

Log "=== soak start rounds=$Rounds bench=${BenchSeconds}s"
$cases = @(
    @{ name = 'fdx_2ch_30M72_12bit'; args = @('--sample-rate', '30.72M') },
    @{ name = 'fdx_2ch_61M44_8bit';  args = @('--sample-rate', '61.44M', '--iq-bits', '8') }
)
foreach ($c in $cases) {
    $f = Join-Path $Out "bench_$($c.name).json"
    och bench --rx --tx --channels '1,2' @($c.args) --seconds $BenchSeconds --amplitude 0 --attenuation 89.75 --json > $f 2>&1
    Log "A $($c.name) rc=$LASTEXITCODE $(BenchPass $f)"
}

$fail = 0
for ($i = 1; $i -le $Rounds; $i++) {
    Start-Sleep 4                                   # > quiet_ms
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lo = 1000000000 + $i * 10000000
    iio_attr -u $Uri -c ad9361-phy RX_LO frequency $lo 2>&1 | Out-Null
    $take = [int]$sw.Elapsed.TotalMilliseconds
    $rb = (iio_attr -u $Uri -c ad9361-phy RX_LO frequency 2>&1 | Out-String).Trim()
    $cap = Join-Path $Out "iio_round$i.bin"
    cmd /c "iio_readdev -u $Uri -b 65536 -s 1048576 cf-ad9361-lpc voltage0 voltage1 voltage2 voltage3 > `"$cap`" 2>nul"
    $capBytes = (Get-Item $cap).Length
    Remove-Item $cap
    $sw.Restart()
    och info --json 2>&1 | Out-Null
    $preRc = $LASTEXITCODE; $preMs = [int]$sw.Elapsed.TotalMilliseconds
    $st = och selftest --json 2>&1 | Out-String
    $stOk = $st -match 'rx_prbs=pass' -and $st -match 'tx_loopback=pass'
    $bf = Join-Path $Out "bench_round$i.json"
    och bench --rx --tx --channels '1,2' --sample-rate 30.72M --seconds 5 --amplitude 0 --attenuation 89.75 --json > $bf 2>&1
    $bp = BenchPass $bf
    $ok = ($rb -eq "$lo") -and ($capBytes -eq 8388608) -and ($preRc -eq 0) -and $stOk -and ($bp -match 'pass=True')
    if (-not $ok) { $fail++ }
    Log ("B round {0,2} {1} takeover={2}ms lo_rb={3} cap={4} och_preempt=rc{5}/{6}ms selftest={7} bench[{8}]" -f
         $i, $(if ($ok) { 'OK  ' } else { 'FAIL' }), $take, ($rb -eq "$lo"), $capBytes, $preRc, $preMs, $stOk, $bp)
}
Log "=== soak end: rounds failed=$fail/$Rounds"
ssh root@$Board 'journalctl -u omnicored -b --no-pager -o cat' | Out-File (Join-Path $Out 'omnicored_journal.txt')
