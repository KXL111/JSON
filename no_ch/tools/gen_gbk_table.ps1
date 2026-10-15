$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot '..\Projects\CommGateway\src\gbk_table.c'

$enc = [System.Text.Encoding]::GetEncoding(936,
        [System.Text.EncoderFallback]::ExceptionFallback,
        [System.Text.DecoderFallback]::ExceptionFallback)

$leadMin = 0x81; $leadMax = 0xFE; $trailMin = 0x40; $trailMax = 0xFE
$span = $trailMax - $trailMin + 1
$toUni = New-Object 'int[]' (($leadMax - $leadMin + 1) * $span)
$uniToGbk = New-Object 'System.Collections.Generic.SortedDictionary[int,int]'

for ($lead = $leadMin; $lead -le $leadMax; $lead++) {
    for ($trail = $trailMin; $trail -le $trailMax; $trail++) {
        if ($trail -eq 0x7F) { continue }
        try { $s = $enc.GetString([byte[]]@($lead, $trail)) } catch { continue }
        if ($s.Length -ne 1) { continue }
        $u = [int][char]$s
        if ($u -ge 0xE000 -and $u -le 0xF8FF) { continue }
        $toUni[($lead - $leadMin) * $span + ($trail - $trailMin)] = $u
        if (-not $uniToGbk.ContainsKey($u)) {
            $b = $enc.GetBytes($s)
            if ($b.Length -eq 2) { $uniToGbk[$u] = ([int]$b[0] -shl 8) -bor [int]$b[1] }
        }
    }
}

function Write-Array($sb, $values) {
    for ($i = 0; $i -lt $values.Count; $i += 16) {
        $end = [Math]::Min($i + 16, $values.Count) - 1
        $row = ($values[$i..$end] | ForEach-Object { '0x{0:X4}' -f $_ }) -join ','
        [void]$sb.Append('    ').Append($row).Append(",`n")
    }
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("/* 由 tools/gen_gbk_table.ps1 生成（Windows 代码页 936），请勿手工修改 */`n`n")
[void]$sb.Append("#include <stdint.h>`n`n")
[void]$sb.Append("/* GBK -> Unicode：下标 (首字节-0x81)*191 + (尾字节-0x40)，0 表示无此字 */`n")
[void]$sb.Append("const uint16_t gbk_table_to_uni[$($toUni.Count)] = {`n")
Write-Array $sb $toUni
[void]$sb.Append("};`n`n")
[void]$sb.Append("/* Unicode -> GBK：按 Unicode 升序排列，二分查找 */`n")
[void]$sb.Append("const uint16_t gbk_table_count = $($uniToGbk.Count);`n`n")
[void]$sb.Append("const uint16_t gbk_table_uni[$($uniToGbk.Count)] = {`n")
Write-Array $sb ([int[]]@($uniToGbk.Keys))
[void]$sb.Append("};`n`n")
[void]$sb.Append("const uint16_t gbk_table_gbk[$($uniToGbk.Count)] = {`n")
Write-Array $sb ([int[]]@($uniToGbk.Values))
[void]$sb.Append("};`n")

[System.IO.File]::WriteAllText($out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "gbk->uni entries: $($toUni.Count), uni->gbk pairs: $($uniToGbk.Count)"
