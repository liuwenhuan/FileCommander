param(
    [string]$Output = "build/wmf-fixtures"
)

$ErrorActionPreference = 'Stop'

$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
if (-not $ffmpeg) {
    throw 'ffmpeg is required to generate WMF probe fixtures.'
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null

& $ffmpeg.Source -y -f lavfi -i "sine=frequency=440:duration=2" `
    -c:a pcm_s16le (Join-Path $Output 'tone.wav')
& $ffmpeg.Source -y -f lavfi -i "sine=frequency=440:duration=2" `
    -c:a libmp3lame -b:a 128k (Join-Path $Output 'tone.mp3')
& $ffmpeg.Source -y -f lavfi -i "sine=frequency=440:duration=2" `
    -c:a flac (Join-Path $Output 'tone.flac')
& $ffmpeg.Source -y -f lavfi -i "testsrc=size=320x180:rate=24:duration=2" `
    -pix_fmt yuv420p -c:v libx264 -preset veryfast (Join-Path $Output 'video-h264.mp4')
& $ffmpeg.Source -y -f lavfi -i "testsrc=size=320x180:rate=24:duration=2" `
    -pix_fmt yuv420p -c:v libx264 -preset veryfast (Join-Path $Output 'video-h264.mkv')

@"
1
00:00:00,250 --> 00:00:01,750
FileCommander WMF subtitle probe
"@ | Set-Content -LiteralPath (Join-Path $Output 'subtitle.srt') -Encoding UTF8

$hasHevcEncoder = (& $ffmpeg.Source -hide_banner -encoders 2>$null |
    Select-String -Pattern 'libx265|hevc' -Quiet)
if ($hasHevcEncoder) {
    & $ffmpeg.Source -y -f lavfi -i "testsrc=size=320x180:rate=24:duration=2" `
        -pix_fmt yuv420p -c:v libx265 -preset ultrafast (Join-Path $Output 'video-hevc.mp4')
}
