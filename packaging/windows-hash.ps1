# PowerShell installations used by CI and the local SDK do not always ship the
# Microsoft.PowerShell.Utility module. Keep package hashing independent of that
# optional cmdlet by using the .NET runtime available on every Windows host.
function Get-Sha256Hash {
    param([Parameter(Mandatory)][string]$LiteralPath)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($LiteralPath)
        try {
            return ([System.BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha256.Dispose()
    }
}
