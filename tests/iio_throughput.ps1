# libiio RX throughput through omnicored (port 30431); N runs per channel set.
param([string]$Board = '192.168.2.1', [int]$Runs = 3, [int]$Samples = 16777216, [int]$Buf = 1048576)
$Uri = "ip:$Board"
iio_attr -u $Uri -c ad9361-phy RX_LO frequency 2>&1 | Out-Null          # take the chip
$hold = Start-Job { param($b) $s = New-Object Net.Sockets.TcpClient($b, 30431); Start-Sleep 120; $s.Close() } -ArgumentList $Board
Start-Sleep 1
foreach ($ch in 'voltage0 voltage1', 'voltage0 voltage1 voltage2 voltage3') {
    $n = $ch.Split(' ').Count
    $r = 1..$Runs | ForEach-Object {
        $t = [Diagnostics.Stopwatch]::StartNew()
        cmd /c "iio_readdev -u $Uri -b $Buf -s $Samples cf-ad9361-lpc $ch > NUL"
        [math]::Round($n * 2 * $Samples / 1e6 / $t.Elapsed.TotalSeconds, 1)
    }
    "iio ${n}ch buf=$Buf : " + ($r -join ' / ') + ' MB/s'
}
Stop-Job $hold; Remove-Job $hold -Force
