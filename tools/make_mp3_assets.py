"""Recreate the synthetic MP3 fixtures; requires FFmpeg with libmp3lame."""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--ffmpeg', default='ffmpeg')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]/'apps/mp3play'
root.mkdir(parents=True, exist_ok=True)
fixtures=[
    ('test-tone.mp3', 'sine=frequency=541.7:sample_rate=44100:duration=4',
     '128k', ['-af', 'pan=stereo|c0=c0|c1=0.7*c0']),
    ('test-48k.mp3',
     'aevalsrc=0.12*sin(2*PI*541.7*t)+0.08*sin(2*PI*2200*t)+0.04*(random(0)-0.5)|'
     '0.10*sin(2*PI*730*t)+0.07*sin(2*PI*4600*t)+0.04*(random(1)-0.5):s=48000:d=2',
     '320k', []),
    ('test-24k.mp3', 'sine=frequency=541.7:sample_rate=24000:duration=2', '64k', []),
]
for name, source, bitrate, extra in fixtures:
    subprocess.run([a.ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    '-i', source, *extra, '-c:a', 'libmp3lame', '-b:a', bitrate,
                    '-write_xing', '0', '-id3v2_version', '0', '-y', str(root/name)], check=True)
    print(root/name)
