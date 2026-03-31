#!/usr/bin/env bash
# Build both the sysmodule and the config NRO
set -e

if [[ -z "$DEVKITPRO" ]]; then
    export DEVKITPRO=/opt/devkitpro
fi

export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$DEVKITARM/bin:$PATH"

echo "=== Building sysmodule ==="
make -C . clean
make -C .

echo ""
echo "=== Building config app ==="
make -C config_app clean
make -C config_app

echo ""
echo "=== Build complete ==="
echo "  Sysmodule NSO : out/main.nso"
echo "  Config app NRO: config_app/out/apple-bt-reconnect-config.nro"
echo ""
echo "See INSTALL.txt for deployment instructions."
