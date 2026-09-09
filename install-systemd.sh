#!/bin/sh
set -eu

SCRIPT_PATH=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/$(basename -- "$0")
APP_DIR=$(CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)

# A system service needs root only to install the unit. It still runs as the
# logged-in desktop user so it can access that user's X11 display and JSON config.
if [ "$(id -u)" -ne 0 ]; then
    INSTALL_USER=${USER:-$(id -un)}
    INSTALL_DISPLAY=${DISPLAY:-:0}
    INSTALL_XAUTHORITY=${XAUTHORITY:-"$(getent passwd "$INSTALL_USER" | cut -d: -f6)/.Xauthority"}
    exec pkexec env \
        CODEX_PET_INSTALL_USER="$INSTALL_USER" \
        CODEX_PET_DISPLAY="$INSTALL_DISPLAY" \
        CODEX_PET_XAUTHORITY="$INSTALL_XAUTHORITY" \
        "$SCRIPT_PATH"
fi

INSTALL_USER=${CODEX_PET_INSTALL_USER:-${SUDO_USER:-}}
if [ -z "$INSTALL_USER" ]; then
    INSTALL_USER=$(logname 2>/dev/null || true)
fi
if [ -z "$INSTALL_USER" ] || ! id "$INSTALL_USER" >/dev/null 2>&1; then
    printf '%s\n' '无法确定桌面用户。请用普通用户执行 ./install-systemd.sh，或执行 sudo CODEX_PET_INSTALL_USER=用户名 ./install-systemd.sh' >&2
    exit 1
fi
INSTALL_DISPLAY=${CODEX_PET_DISPLAY:-${DISPLAY:-:0}}
INSTALL_XAUTHORITY=${CODEX_PET_XAUTHORITY:-${XAUTHORITY:-/home/$INSTALL_USER/.Xauthority}}
INSTALL_HOME=$(getent passwd "$INSTALL_USER" | cut -d: -f6)
INSTALL_GROUP=$(id -gn "$INSTALL_USER")
INSTALL_UID=$(id -u "$INSTALL_USER")
SERVICE=/etc/systemd/system/codex-pet.service
DESKTOP_DIR="$INSTALL_HOME/.local/share/applications"
DESKTOP="$DESKTOP_DIR/codex-pet-settings.desktop"
DESKTOP_HOME="$INSTALL_HOME/Desktop"

cat > "$SERVICE" <<EOF
[Unit]
Description=Codex Pet X11 overlay
After=graphical.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=$INSTALL_USER
WorkingDirectory=$APP_DIR
Environment=DISPLAY=$INSTALL_DISPLAY
Environment=XAUTHORITY=$INSTALL_XAUTHORITY
Environment=XDG_RUNTIME_DIR=/run/user/$INSTALL_UID
Environment=PULSE_SERVER=unix:/run/user/$INSTALL_UID/pulse/native
ExecStart=$APP_DIR/codex-pet
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical.target
EOF

mkdir -p "$DESKTOP_DIR"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Codex Pet Settings
Comment=Edit Codex Pet bubble displays
Exec=$APP_DIR/codex-pet --settings $APP_DIR/codex-pet.json
Terminal=false
Categories=Settings;Utility;
EOF
chown "$INSTALL_USER:$INSTALL_GROUP" "$DESKTOP"
mkdir -p "$DESKTOP_HOME"
cp "$DESKTOP" "$DESKTOP_HOME/codex-pet-settings.desktop"
chown "$INSTALL_USER:$INSTALL_GROUP" "$DESKTOP_HOME/codex-pet-settings.desktop"

systemctl daemon-reload
systemctl enable --now codex-pet.service
printf 'Codex Pet system service enabled for %s\n' "$INSTALL_USER"
