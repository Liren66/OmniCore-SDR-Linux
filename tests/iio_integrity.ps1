# IIO data integrity through the DMA buffers: cyclic TX counters -> AD9361 digital
# loopback -> RX capture of several DMA blocks, checked sample by sample.
# TX attenuation stays at 89.75 dB (digital loopback only, nothing radiated).
param([string]$Board = '192.168.2.1', [int]$Block = 1048576, [int]$Samples = 8388608,
      [double]$Rate = 30720000, [int]$Channels = 4, [switch]$GapsOnly,
      [string]$Out = (Join-Path $PSScriptRoot 'results'))
New-Item -ItemType Directory -Force $Out | Out-Null
$Uri = "ip:$Board"
$pat = Join-Path $Out 'tx_counters.bin'
$cap = Join-Path $Out 'iio_counter_rx.bin'
python -I (Join-Path $PSScriptRoot 'iio_counter.py') gen $pat
Start-Sleep 4
iio_attr -u $Uri -i -c ad9361-phy voltage0 sampling_frequency ([int64]$Rate) | Out-Null
iio_attr -u $Uri -D ad9361-phy loopback 1 | Out-Null
$tx = Start-Job { param($u, $f)
    cmd /c "iio_writedev -u $u -c -b 4096 cf-ad9361-dds-core-lpc voltage0 voltage1 voltage2 voltage3 < `"$f`" 2>&1"
} -ArgumentList $Uri, $pat
Start-Sleep 3
$rx = (0..($Channels - 1) | ForEach-Object { "voltage$_" }) -join ' '
$t = [Diagnostics.Stopwatch]::StartNew()
cmd /c "iio_readdev -u $Uri -b $Block -s $Samples cf-ad9361-lpc $rx > `"$cap`""
"rate={0:N2} MSPS rx_channels={1} {2:N1} MB/s" -f ($Rate / 1e6), $Channels, ((Get-Item $cap).Length / 1e6 / $t.Elapsed.TotalSeconds)
"readdev rc=$LASTEXITCODE bytes=$((Get-Item $cap).Length)"
Stop-Job $tx; Remove-Job $tx -Force
iio_attr -u $Uri -D ad9361-phy loopback 0 | Out-Null
python -I (Join-Path $PSScriptRoot 'iio_counter.py') $(if ($GapsOnly) { 'gaps' } else { 'check' }) $cap $Block $Channels
"check rc=$LASTEXITCODE"
Remove-Item $cap
