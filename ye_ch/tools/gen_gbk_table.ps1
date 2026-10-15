# 生成 GBK <-> Unicode 对照表 Projects/CommGateway/src/gbk_table.c
# 数据来源：Windows 代码页 936（GBK）。一次性工具，生成结果已放进工程，平时不用运行。
# 用法：powershell -ExecutionPolicy Bypass -File tools\gen_gbk_table.ps1

$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot '..\Projects\CommGateway\src\gbk_table.c'

# 用异常回退，避免 .NET 把无法转换的字符“近似替换”成别的字
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
        if ($u -ge 0xE000 -and $u -le 0xF8FF) { continue }   # 用户自定义区，不收录
        $toUni[($lead - $leadMin) * $span + ($trail - $trailMin)] = $u
        if (-not $uniToGbk.ContainsKey($u)) {
            $b = $enc.GetBytes($s)                            # 以编码器的标准编码为准
            if ($b.Length -eq 2) { $uniToGbk[$u] = ([int]$b[0] -shl 8) -bor [int]$b[1] }
        }
    }
}

# 把整数数组写成 C 数组的初始化内容：每行 16 个，格式 0xXXXX，每行以逗号结尾
#   $sb     —— 输出用的 StringBuilder，结果追加在末尾
#   $values —— 整数数组：GBK->Unicode 表，或 Unicode / GBK 码列表
# 调用：下面生成 gbk_table_to_uni、gbk_table_uni、gbk_table_gbk 三个数组时各调用一次
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
