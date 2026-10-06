#!/usr/bin/env bash
# One-time root setup for running blah2 with RTL-SDRs on Arch Linux.
set -euo pipefail
sudo pacman -S --needed docker docker-compose docker-buildx
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
sudo mkdir -p /opt/blah2/save
sudo chown "$USER:$USER" /opt/blah2/save
echo "Root setup done."
