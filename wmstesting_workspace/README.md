# WMS Testing Workspace
*By Josse DE OLIVEIRA*

## Purpose
This folder is temporary, it contains the configurations I use to test the WMS filter

## Configuration I use
- **Requirements:**
  - TODO: List requirements
- **Configure** compilation using GPLv3 License, enabling what we need to test
```shell
./configure --enable-debug=3 --enable-gpl --enable-version3 --enable-libx264 --enable-libxml2 --enable-openssl --extra-libs=-L/usr/lib/x86_64-linux-gnu
```
- **Compile** ffmpeg using `make`

## Use cases
*(all scripts are from projet root)*
- Create a 5-second video with zoom-out in Lyon, France & add a red border
```shell
./ffmpeg \
  -loglevel debug -y -f lavfi \
  -i "wms=xref=45.7507:yref=4.8340:x1='xref-0.01-(t/100)':y1=yref-0.01-(t/100):x2=xref+0.01+(t/100):y2=yref+0.01+(t/100):s=256x256:url='https\://imagerie.data.grandlyon.com/all/wms':layers=ortho_latest" \
  -vf drawbox=c=red:t=10 -t 5 -c:v libx264 wmstesting_workspace/out.mp4 
```

