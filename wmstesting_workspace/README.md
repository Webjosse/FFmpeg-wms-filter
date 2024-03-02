# WMS Testing Workspace
*By Josse DE OLIVEIRA*

## Purpose
This folder is temporary, it contains the configurations I use to test the WMS filter

## Configuration I use
- **Requirements:**
  - TODO: List requirements
- **Configure** compilation using GPLv3 License, enabling what we need to test
```shell
./configure --enable-debug=3 --enable-gpl --enable-version3 --enable-libx264 --enable-libx265 --enable-libvpx --enable-openssl --extra-libs=-L/usr/lib/x86_64-linux-gnu
```
- **Compile** ffmpeg using `make`

## Use cases
*(all scripts are from projet root)*
- Create a video of 5 seconds going from Bruxelles (Belgium) to Lille (France)
```shell
ffmpeg -y -f lavfi -i "wms=xref='lerp(4.3517\, 3.0638\, t/5)':yref='lerp(50.8460\, 50.6355\, t/5)':x1=xref-0.5:y1=yref-0.5:x2=xref+0.5:y2=yref+0.5:s=256x256" -t 5 -c:v libx264 wmstesting_workspace/out.mp4
```

