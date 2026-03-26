#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── defaults ────────────────────────────────────────────────────────────────
TARGET=""
PORT=""
BAUD=921600
MONITOR=0
OTA_HOST=""
OTA_USER=""
OTA_PASS=""
EXTRA_IDFPY_ARGS=()

# ── usage ────────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build and flash the ESP32 XBee NTRIP firmware (serial or OTA).

Serial flash options:
  -t, --target TARGET   ESP32 target chip (esp32 | esp32s2 | esp32s3 | esp32c3)
                        Defaults to the value already set in sdkconfig, if present.
  -p, --port   PORT     Serial port (e.g. /dev/ttyUSB0).  Auto-detected if omitted.
  -b, --baud   BAUD     Flash baud rate (default: $BAUD)
  -m, --monitor         Launch idf.py monitor after flashing

OTA options:
  -o, --ota    HOST     Upload firmware over WiFi to http://HOST/ota/update
                        Skips serial flash; still builds first unless --no-build.
      --user   USER     HTTP basic auth username (if auth is enabled on device)
      --pass   PASS     HTTP basic auth password

Common:
      --no-build        Skip build step (use existing build/PROJECT.bin)
  -h, --help            Show this help

Examples:
  ./flash.sh -t esp32s3 -p /dev/ttyUSB0
  ./flash.sh -t esp32s3 --monitor
  ./flash.sh --ota 192.168.4.1
  ./flash.sh --ota 192.168.1.42 --user admin --pass secret
  ./flash.sh --ota 192.168.4.1 --no-build
EOF
}

# ── arg parsing ──────────────────────────────────────────────────────────────
NO_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -t|--target)   TARGET="$2";   shift 2 ;;
        -p|--port)     PORT="$2";     shift 2 ;;
        -b|--baud)     BAUD="$2";     shift 2 ;;
        -m|--monitor)  MONITOR=1;     shift   ;;
        -o|--ota)      OTA_HOST="$2"; shift 2 ;;
        --user)        OTA_USER="$2"; shift 2 ;;
        --pass)        OTA_PASS="$2"; shift 2 ;;
        --no-build)    NO_BUILD=1;    shift   ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

cd "$SCRIPT_DIR"

# ── source ESP-IDF if not already active ─────────────────────────────────────
# IDF 6.0 uses a shell alias for idf.py rather than a PATH entry, so check
# IDF_PATH instead of command -v idf.py to detect an active environment.
if [[ -z "${IDF_PATH:-}" ]]; then
    IDF_CANDIDATES=(
        "$HOME/.espressif/tools/activate_idf_v6.0.sh"
        "$HOME/esp/esp-idf/export.sh"
        "$HOME/.espressif/esp-idf/export.sh"
        "/opt/esp-idf/export.sh"
        "/usr/local/esp-idf/export.sh"
    )
    SOURCED=0
    for candidate in "${IDF_CANDIDATES[@]}"; do
        if [[ -f "$candidate" ]]; then
            echo "Sourcing ESP-IDF from: $candidate"
            # shellcheck disable=SC1090
            source "$candidate"
            SOURCED=1
            break
        fi
    done
    if [[ $SOURCED -eq 0 ]]; then
        echo "ERROR: ESP-IDF not found. Run '. \$IDF_PATH/export.sh' or"
        echo "       '. ~/.espressif/tools/activate_idf_v6.0.sh' before calling this script."
        exit 1
    fi
fi

# ── resolve idf.py invocation ─────────────────────────────────────────────────
# IDF 6.0: idf.py is a shell alias, not on PATH — invoke via Python directly.
# IDF 5.x: idf.py is a real command on PATH — use it directly.
if command -v idf.py &>/dev/null; then
    IDF_PY=(idf.py)
elif [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/tools/idf.py" ]]; then
    if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
        IDF_PY=("$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py")
    else
        IDF_PY=(python "$IDF_PATH/tools/idf.py")
    fi
else
    echo "ERROR: idf.py not found. Ensure ESP-IDF is properly installed and sourced."
    exit 1
fi

# ── resolve target ───────────────────────────────────────────────────────────
if [[ -z "$TARGET" ]]; then
    if [[ -f sdkconfig ]]; then
        TARGET=$(grep -m1 '^CONFIG_IDF_TARGET=' sdkconfig | cut -d'"' -f2)
        [[ -n "$TARGET" ]] && echo "Detected target from sdkconfig: $TARGET"
    fi
fi

if [[ -z "$TARGET" && $NO_BUILD -eq 0 ]]; then
    echo "ERROR: No target specified and no sdkconfig found."
    echo "       Use -t / --target (esp32 | esp32s2 | esp32s3 | esp32c3)"
    exit 1
fi

# ── set target if it changed ─────────────────────────────────────────────────
if [[ $NO_BUILD -eq 0 ]]; then
    CURRENT_TARGET=""
    if [[ -f sdkconfig ]]; then
        CURRENT_TARGET=$(grep -m1 '^CONFIG_IDF_TARGET=' sdkconfig | cut -d'"' -f2)
    fi
    if [[ "$TARGET" != "$CURRENT_TARGET" ]]; then
        echo "Setting target to: $TARGET"
        "${IDF_PY[@]}" set-target "$TARGET"
    fi
fi

# ── build ─────────────────────────────────────────────────────────────────────
if [[ $NO_BUILD -eq 0 ]]; then
    echo ""
    echo "Building for target: $TARGET"
    "${IDF_PY[@]}" build
fi

# ── find app binary ──────────────────────────────────────────────────────────
find_app_bin() {
    # idf.py names the binary after the project() name in CMakeLists.txt
    local project
    project=$(grep -m1 '^project(' CMakeLists.txt | sed 's/project(\(.*\))/\1/')
    local bin="build/${project}.bin"
    if [[ ! -f "$bin" ]]; then
        # fallback: any .bin in build/ that isn't a partition/bootloader binary
        bin=$(find build -maxdepth 1 -name '*.bin' \
              ! -name 'partition*' ! -name 'bootloader*' \
              | head -1)
    fi
    echo "$bin"
}

# ── OTA path ─────────────────────────────────────────────────────────────────
if [[ -n "$OTA_HOST" ]]; then
    APP_BIN=$(find_app_bin)
    if [[ -z "$APP_BIN" || ! -f "$APP_BIN" ]]; then
        echo "ERROR: App binary not found. Run without --no-build or check build/."
        exit 1
    fi

    OTA_URL="http://${OTA_HOST}/ota/update"
    CURL_ARGS=(-s -S --max-time 120 -w "\n%{http_code}")
    [[ -n "$OTA_USER" ]] && CURL_ARGS+=(-u "${OTA_USER}:${OTA_PASS}")

    echo ""
    echo "OTA upload: $(basename "$APP_BIN") ($(du -h "$APP_BIN" | cut -f1)) → $OTA_URL"

    RESPONSE=$(curl "${CURL_ARGS[@]}" \
        -X POST "$OTA_URL" \
        -H "Content-Type: application/octet-stream" \
        --data-binary "@${APP_BIN}")

    HTTP_CODE=$(tail -1 <<< "$RESPONSE")
    BODY=$(head -1 <<< "$RESPONSE")

    if [[ "$HTTP_CODE" == "200" ]]; then
        echo "OTA success: $BODY"
        echo "Device is rebooting..."
    else
        echo "ERROR: OTA failed (HTTP $HTTP_CODE): $BODY"
        exit 1
    fi
    exit 0
fi

# ── serial flash path ─────────────────────────────────────────────────────────
if [[ -z "$PORT" ]]; then
    for candidate in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
        if [[ -e "$candidate" ]]; then
            PORT="$candidate"
            echo "Auto-detected port: $PORT"
            break
        fi
    done
fi

if [[ -z "$PORT" ]]; then
    echo "ERROR: No serial port found. Plug in the device or use -p / --port."
    exit 1
fi

EXTRA_IDFPY_ARGS+=(-p "$PORT" -b "$BAUD")

echo ""
echo "Flashing to $PORT at ${BAUD} baud..."
"${IDF_PY[@]}" "${EXTRA_IDFPY_ARGS[@]}" flash

if [[ $MONITOR -eq 1 ]]; then
    echo ""
    echo "Starting monitor (Ctrl-] to exit)..."
    "${IDF_PY[@]}" -p "$PORT" monitor
fi
