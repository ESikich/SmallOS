param(
    [string]$FtpHost = "127.0.0.1",
    [int]$FtpPort = 2121,
    [string]$RetrPath = "apps/demo/hello.elf",
    [string]$StorPath = "PS_SMOKE.TXT",
    [string]$StorPayload = "ftp smoke payload`r`n",
    [int]$TimeoutSeconds = 10
)

$ErrorActionPreference = "Stop"

function New-FtpClient {
    param(
        [string]$Server,
        [int]$ServerPort
    )

    $client = [System.Net.Sockets.TcpClient]::new()
    $client.ReceiveTimeout = $TimeoutSeconds * 1000
    $client.SendTimeout = $TimeoutSeconds * 1000
    $client.Connect($Server, $ServerPort)
    return $client
}

function Get-FtpLines {
    param(
        [System.IO.StreamReader]$Reader
    )

    $lines = New-Object System.Collections.Generic.List[string]
    $first = $Reader.ReadLine()
    if ($null -eq $first) {
        throw "FTP connection closed unexpectedly"
    }
    $lines.Add($first)

    if ($first.Length -ge 4 -and $first[3] -eq '-') {
        $code = $first.Substring(0, 3)
        while ($true) {
            $line = $Reader.ReadLine()
            if ($null -eq $line) {
                throw "FTP connection closed while waiting for multiline reply"
            }
            $lines.Add($line)
            if ($line.Length -ge 4 -and $line.StartsWith($code + " ")) {
                break
            }
        }
    }

    return ,$lines.ToArray()
}

function Send-FtpCommand {
    param(
        [System.IO.StreamWriter]$Writer,
        [System.IO.StreamReader]$Reader,
        [string]$Command
    )

    $Writer.WriteLine($Command)
    $Writer.Flush()
    $lines = Get-FtpLines -Reader $Reader
    return ,$lines
}

function Assert-ReplyCode {
    param(
        [string[]]$Lines,
        [string]$ExpectedPrefix,
        [string]$Context
    )

    if ($Lines.Count -eq 0 -or -not $Lines[-1].StartsWith($ExpectedPrefix)) {
        throw "$Context failed. Reply was: $($Lines -join ' | ')"
    }
}

function Open-PasvSocket {
    param(
        [System.IO.StreamWriter]$Writer,
        [System.IO.StreamReader]$Reader
    )

    $lines = Send-FtpCommand -Writer $Writer -Reader $Reader -Command "PASV"
    Assert-ReplyCode -Lines $lines -ExpectedPrefix "227" -Context "PASV"

    $match = [regex]::Match($lines[-1], '\((\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\)')
    if (-not $match.Success) {
        throw "PASV reply did not contain a host/port tuple: $($lines[-1])"
    }

    $dataHost = "{0}.{1}.{2}.{3}" -f $match.Groups[1].Value, $match.Groups[2].Value, $match.Groups[3].Value, $match.Groups[4].Value
    $dataPort = ([int]$match.Groups[5].Value * 256) + [int]$match.Groups[6].Value

    $dataClient = New-FtpClient -Server $dataHost -ServerPort $dataPort
    return [pscustomobject]@{
        Host = $dataHost
        Port = $dataPort
        Client = $dataClient
    }
}

function Read-AllBytes {
    param(
        [System.Net.Sockets.TcpClient]$Client
    )

    $stream = $Client.GetStream()
    $buffer = New-Object byte[] 4096
    $bytes = New-Object System.Collections.Generic.List[byte]
    $sawData = $false
    $idleSince = [DateTime]::UtcNow

    while ($true) {
        if ($stream.DataAvailable) {
            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) {
                break
            }
            for ($i = 0; $i -lt $read; $i++) {
                $bytes.Add($buffer[$i])
            }
            $sawData = $true
            $idleSince = [DateTime]::UtcNow
            continue
        }

        if ($sawData) {
            $idle = ([DateTime]::UtcNow - $idleSince).TotalMilliseconds
            if ($idle -ge 250) {
                break
            }
        }

        Start-Sleep -Milliseconds 25
    }

    return ,$bytes.ToArray()
}

function Invoke-DataCommand {
    param(
        [System.IO.StreamWriter]$Writer,
        [System.IO.StreamReader]$Reader,
        [string]$Command,
        [System.Net.Sockets.TcpClient]$DataClient,
        [byte[]]$UploadBytes
    )

    $lines = Send-FtpCommand -Writer $Writer -Reader $Reader -Command $Command
    Assert-ReplyCode -Lines $lines -ExpectedPrefix "150" -Context $Command

    if ($Command.StartsWith("STOR")) {
        $dataStream = $DataClient.GetStream()
        $dataStream.Write($UploadBytes, 0, $UploadBytes.Length)
        $DataClient.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        $DataClient.Close()
        $dataBytes = @()
    } else {
        $dataBytes = Read-AllBytes -Client $DataClient
        $DataClient.Close()
    }

    $finalLines = Get-FtpLines -Reader $Reader
    Assert-ReplyCode -Lines $finalLines -ExpectedPrefix "226" -Context "$Command completion"

    return [pscustomobject]@{
        DataBytes = $dataBytes
        ReplyLines = $finalLines
    }
}

$ftp = New-FtpClient -Server $FtpHost -ServerPort $FtpPort
$reader = [System.IO.StreamReader]::new($ftp.GetStream(), [System.Text.Encoding]::ASCII, $false, 1024, $true)
$writer = [System.IO.StreamWriter]::new($ftp.GetStream(), [System.Text.Encoding]::ASCII, 1024, $true)
$writer.NewLine = "`r`n"
$writer.AutoFlush = $true

try {
    $banner = Get-FtpLines -Reader $reader
    Assert-ReplyCode -Lines $banner -ExpectedPrefix "220" -Context "banner"
    Write-Host ("banner: {0}" -f ($banner -join " | "))

    $lines = Send-FtpCommand -Writer $writer -Reader $reader -Command "USER ftp"
    Assert-ReplyCode -Lines $lines -ExpectedPrefix "331" -Context "USER"
    Write-Host ("USER: {0}" -f ($lines -join " | "))

    $lines = Send-FtpCommand -Writer $writer -Reader $reader -Command "PASS ftp"
    Assert-ReplyCode -Lines $lines -ExpectedPrefix "230" -Context "PASS"
    Write-Host ("PASS: {0}" -f ($lines -join " | "))

    foreach ($cmd in @("SYST", "PWD", "CWD /")) {
        $lines = Send-FtpCommand -Writer $writer -Reader $reader -Command $cmd
        if ($cmd -eq "PWD") {
            Assert-ReplyCode -Lines $lines -ExpectedPrefix "257" -Context $cmd
        } else {
            Assert-ReplyCode -Lines $lines -ExpectedPrefix "2" -Context $cmd
        }
        Write-Host ("{0}: {1}" -f $cmd, ($lines -join " | "))
    }

    $pasv = Open-PasvSocket -Writer $writer -Reader $reader
    Write-Host ("PASV data endpoint: {0}:{1}" -f $pasv.Host, $pasv.Port)
    $list = Invoke-DataCommand -Writer $writer -Reader $reader -Command "LIST" -DataClient $pasv.Client
    $listText = [System.Text.Encoding]::ASCII.GetString($list.DataBytes)
    Write-Host ("LIST bytes: {0}" -f $list.DataBytes.Length)
    Write-Host ("LIST sample: {0}" -f $listText.Substring(0, [Math]::Min(200, $listText.Length)))

    $pasv = Open-PasvSocket -Writer $writer -Reader $reader
    $retr = Invoke-DataCommand -Writer $writer -Reader $reader -Command ("RETR {0}" -f $RetrPath) -DataClient $pasv.Client
    Write-Host ("RETR bytes: {0}" -f $retr.DataBytes.Length)
    if ($retr.DataBytes.Length -ge 4) {
        $sig = '{0:x2}{1:x2}{2:x2}{3:x2}' -f $retr.DataBytes[0], $retr.DataBytes[1], $retr.DataBytes[2], $retr.DataBytes[3]
        Write-Host ("RETR signature: {0}" -f $sig)
    }

    $pasv = Open-PasvSocket -Writer $writer -Reader $reader
    $uploadBytes = [System.Text.Encoding]::ASCII.GetBytes($StorPayload)
    $stor = Invoke-DataCommand -Writer $writer -Reader $reader -Command ("STOR {0}" -f $StorPath) -DataClient $pasv.Client -UploadBytes $uploadBytes
    Write-Host ("STOR uploaded bytes: {0}" -f $uploadBytes.Length)
    Write-Host ("STOR final: {0}" -f ($stor.ReplyLines -join " | "))

    $pasv = Open-PasvSocket -Writer $writer -Reader $reader
    $list2 = Invoke-DataCommand -Writer $writer -Reader $reader -Command "LIST" -DataClient $pasv.Client
    $list2Text = [System.Text.Encoding]::ASCII.GetString($list2.DataBytes)
    $found = $list2Text.ToUpper().Contains($StorPath.ToUpper())
    Write-Host ("LIST after STOR contains {0}: {1}" -f $StorPath, $found)

    $lines = Send-FtpCommand -Writer $writer -Reader $reader -Command "QUIT"
    Assert-ReplyCode -Lines $lines -ExpectedPrefix "221" -Context "QUIT"
    Write-Host ("QUIT: {0}" -f ($lines -join " | "))

    Write-Host "FTP smoke PASS"
}
finally {
    $ftp.Close()
}
