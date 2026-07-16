#!/usr/bin/zsh

meson setup build64 . --wipe --prefix=/usr
meson compile -C build64

meson setup build32 . --wipe --cross-file cross-i386-fedora.txt
meson compile -C build32

sudo meson install -C build64
sudo mkdir -p /usr/lib/dri
sudo cp build32/nvidia_drv_video.so /usr/lib/dri/nvidia_drv_video.so

systemctl --user restart nvenc-helper.service