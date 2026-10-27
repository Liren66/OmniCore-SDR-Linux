# IIO-mode TX path test without cables: the ADI DMA feeds cf-ad9361-dds-core-lpc
# (CH32 IP passes DMA samples to the DAC while USB TX is off), the AD9361 digital
# loopback (debug attribute loopback=1) returns TX to RX, and the two tones must
# appear on RX1 (+fs/32) and RX2 (+fs/16). TX attenuation stays at 89.75 dB.
param([string]$Board = '192.168.2.1', [string]$Out = (Join-Path $PSScriptRoot 'results'))
New-Item -ItemType Directory -Force $Out | Out-Null
$Uri = "ip:$Board"
$tone = Join-Path $Out 'tx_tones.bin'
$cap = Join-Path $Out 'iio_loopback_rx.bin'
python -I (Join-Path $PSScriptRoot 'iio_tx_tone.py') gen $tone

Start-Sleep 4
iio_attr -u $Uri -i -c ad9361-phy voltage0 sampling_frequency 30720000 | Out-Null
"tx hardwaregain: " + (iio_attr -u $Uri -o -c ad9361-phy voltage0 hardwaregain)
iio_attr -u $Uri -D ad9361-phy loopback 1 | Out-Null
"loopback: " + (iio_attr -u $Uri -D ad9361-phy loopback)
$tx = Start-Job { param($u, $f)
    cmd /c "iio_writedev -u $u -c -b 4096 cf-ad9361-dds-core-lpc voltage0 voltage1 voltage2 voltage3 < `"$f`" 2>&1"
} -ArgumentList $Uri, $tone
Start-Sleep 3
cmd /c "iio_readdev -u $Uri -b 65536 -s 65536 cf-ad9361-lpc voltage0 voltage1 voltage2 voltage3 > `"$cap`""
"readdev rc=$LASTEXITCODE bytes=$((Get-Item $cap).Length)"
Stop-Job $tx; Receive-Job $tx | Select-Object -First 3; Remove-Job $tx -Force
iio_attr -u $Uri -D ad9361-phy loopback 0 | Out-Null
python -I (Join-Path $PSScriptRoot 'iio_tx_tone.py') check $cap | Tee-Object (Join-Path $Out 'iio_tx_loopback.txt')
"check rc=$LASTEXITCODE"
