#!/bin/sh
set -eu
PORT="${1:-27015}"
HERE="$(cd "$(dirname "$0")" && pwd)"

apt-get install -y python3 >/dev/null
id windfall >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin windfall
install -d -o root -g root /opt/windfall
install -m 0644 "$HERE/windfall_server.py" /opt/windfall/windfall_server.py

cat > /etc/systemd/system/windfall.service <<EOF
[Unit]
Description=Windfall multiplayer server
After=network-online.target
Wants=network-online.target

[Service]
User=windfall
ExecStart=/usr/bin/python3 /opt/windfall/windfall_server.py --port $PORT
Restart=always
RestartSec=2
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow "$PORT/udp"
fi
systemctl daemon-reload
systemctl enable windfall
systemctl restart windfall  # also picks up a new windfall_server.py on reinstall
systemctl --no-pager status windfall | head -5
echo "Windfall server running: UDP port $PORT, WebSocket on 127.0.0.1:27016 (point a Cloudflare tunnel or reverse proxy at it)"
