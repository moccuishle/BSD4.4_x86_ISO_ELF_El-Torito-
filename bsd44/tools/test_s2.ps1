$ws = 'C:\Users\Administrator\Documents\deepseek-harness\default-workspace\bsd44'
$iso = Join-Path $ws 'out\BSD44-ELF-x86.iso'
$log = "$ws\build\s2test.log"
Remove-Item $log -ErrorAction SilentlyContinue
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = 'C:\Program Files\qemu\qemu-system-i386.exe'
$psi.Arguments = "-cdrom `"$iso`" -boot d -display none -serial file:build/s2test.log -monitor stdio -m 64 -no-reboot"
$psi.WorkingDirectory = $ws
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)

function Wait-Text([string]$needle, [int]$secs) {
    for ($i = 0; $i -lt $secs * 2; $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path $log) {
            $t = Get-Content $log -Raw
            if ($t -match [regex]::Escape($needle)) { return $true }
        }
    }
    return $false
}
function Send-Str([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($ch -eq ' ') { 'spc' } else { "$ch" }
        $p.StandardInput.WriteLine("sendkey $k")
        Start-Sleep -Milliseconds 90
    }
    $p.StandardInput.WriteLine('sendkey ret')
    Start-Sleep -Milliseconds 400
}

Write-Host "prompt: $(Wait-Text 'bsd44>' 15)"
Send-Str 'usermode'
Write-Host "sh prompt: $(Wait-Text 'sh>' 8)"
Send-Str 'echo hi'
Start-Sleep -Seconds 2
Send-Str 'exit'
Start-Sleep -Seconds 1
$p.StandardInput.WriteLine('quit')
Start-Sleep -Milliseconds 500
if (!$p.HasExited) { $p.Kill() }
Write-Host '--- serial ---'
Get-Content $log -Raw

