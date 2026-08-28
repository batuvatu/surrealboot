#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DEPS="$ROOT/.deps"
BUILD="$ROOT/build"
DIST="$ROOT/dist"

USBLITER8_REPO="${USBLITER8_REPO:-https://github.com/FogboundSloth25/usbliter8bootsurreal.git}"
USBLITER8_REF="${USBLITER8_REF:-afe8b5c8998fce63e76c0b2a88c606c61e2950c7}"

PICO_SDK_VERSION="${PICO_SDK_VERSION:-2.2.0}"

PICO_SDK="$DEPS/pico-sdk-$PICO_SDK_VERSION"
UPSTREAM_CACHE="$DEPS/usbliter8bootsurreal.git"
SRC="$DEPS/build-source"

BOARDS=(
    waveshare_rp2350_usb_a
    waveshare_rp2350_zero
    pimoroni_tiny2350
    pico2
)

die() {
    echo
    echo "ERROR: $*" >&2
    exit 1
}

info() {
    echo
    echo "============================================================"
    echo "$*"
    echo "============================================================"
}

trap 'echo; echo "ERROR: build failed at line $LINENO"; exit 1' ERR

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

run_root() {
    if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
        "$@"
    else
        have_cmd sudo || die "sudo is required to install packages"
        sudo "$@"
    fi
}

cpu_jobs() {
    if have_cmd nproc; then
        nproc
    elif have_cmd sysctl; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}

file_size() {
    if stat -c '%s' "$1" >/dev/null 2>&1; then
        stat -c '%s' "$1"
    else
        stat -f '%z' "$1"
    fi
}

resolve_path() {
    if have_cmd realpath; then
        realpath "$1"
    else
        python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
    fi
}

ensure_brew_in_path() {
    if have_cmd brew; then
        return 0
    fi

    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [[ -x /usr/local/bin/brew ]]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi

    have_cmd brew
}

detect_os() {
    local uname_s uname_m

    uname_s="$(uname -s)"
    uname_m="$(uname -m)"
    HOST_OS=""
    HOST_DISTRO=""
    HOST_ARCH="$uname_m"
    PKG_FAMILY=""

    case "$uname_s" in
        Darwin)
            HOST_OS="macos"
            PKG_FAMILY="brew"
            if [[ "$uname_m" == "arm64" ]]; then
                HOST_DISTRO="macos-apple-silicon"
            else
                HOST_DISTRO="macos-intel"
            fi
            ;;
        Linux)
            HOST_OS="linux"
            if [[ -r /etc/os-release ]]; then
                # shellcheck disable=SC1091
                . /etc/os-release
                HOST_DISTRO="${ID:-linux}"
                local like
                like=" ${ID:-} ${ID_LIKE:-} "
                if [[ "$like" == *" debian "* || "$like" == *" ubuntu "* ]]; then
                    PKG_FAMILY="apt"
                elif [[ "$like" == *" arch "* || "$HOST_DISTRO" == "arch" || "$HOST_DISTRO" == "manjaro" || "$HOST_DISTRO" == "endeavouros" || "$HOST_DISTRO" == "garuda" || "$HOST_DISTRO" == "cachyos" ]]; then
                    PKG_FAMILY="pacman"
                elif [[ "$like" == *" fedora "* || "$like" == *" rhel "* || "$HOST_DISTRO" == "fedora" ]]; then
                    PKG_FAMILY="dnf"
                fi
            fi

            if [[ -z "$PKG_FAMILY" ]]; then
                if have_cmd pacman; then
                    PKG_FAMILY="pacman"
                    HOST_DISTRO="${HOST_DISTRO:-arch}"
                elif have_cmd apt-get; then
                    PKG_FAMILY="apt"
                    HOST_DISTRO="${HOST_DISTRO:-debian}"
                elif have_cmd dnf; then
                    PKG_FAMILY="dnf"
                    HOST_DISTRO="${HOST_DISTRO:-fedora}"
                fi
            fi
            ;;
        *)
            die "Unsupported operating system: $uname_s"
            ;;
    esac

    [[ -n "$PKG_FAMILY" ]] || die "Could not detect a supported package manager"
}

# ============================================================
# Host platform
# ============================================================

detect_os

info "Detected host platform"
echo "OS:        $HOST_OS"
echo "Distro:    $HOST_DISTRO"
echo "Arch:      $HOST_ARCH"
echo "Packages:  $PKG_FAMILY"

# ============================================================
# Dependencies
# ============================================================

install_brew_deps() {
    ensure_brew_in_path || die "Homebrew is required on macOS. Install it from https://brew.sh"

    if ! xcode-select -p >/dev/null 2>&1; then
        echo "Xcode Command Line Tools are missing."
        echo "Install them with:  xcode-select --install"
        die "Xcode Command Line Tools are required"
    fi

    echo "Using Homebrew at: $(command -v brew)"
    brew --version | head -n 1

    brew update || true
    brew install \
        cmake \
        ninja \
        git \
        curl \
        wget \
        python3 \
        pkg-config \
        libusb

    # Homebrew arm-none-eabi-gcc is a compiler-only formula and does not
    # ship newlib. Pico SDK links with --specs=nosys.specs, so that
    # package fails on macOS with:
    #   cannot read spec file 'nosys.specs'
    # The official Arm embedded cask includes gcc + newlib + specs.
    brew uninstall --force \
        arm-none-eabi-gcc \
        arm-none-eabi-binutils \
        arm-none-eabi-gdb \
        >/dev/null 2>&1 || true

    brew install --cask gcc-arm-embedded
}

install_apt_deps() {
    have_cmd apt-get || die "apt-get was not found"

    run_root apt-get update

    # gcc-arm-none-eabi lives in Ubuntu universe on many releases.
    if have_cmd add-apt-repository; then
        run_root add-apt-repository -y universe || true
        run_root apt-get update || true
    fi

    run_root apt-get install -y \
        cmake \
        ninja-build \
        make \
        gcc \
        g++ \
        git \
        curl \
        wget \
        tar \
        xz-utils \
        python3 \
        pkg-config \
        libusb-1.0-0-dev \
        gcc-arm-none-eabi \
        binutils-arm-none-eabi \
        libnewlib-arm-none-eabi
}

install_pacman_deps() {
    have_cmd pacman || die "pacman was not found"

    run_root pacman -Sy --noconfirm --needed \
        cmake \
        ninja \
        make \
        gcc \
        git \
        curl \
        wget \
        tar \
        xz \
        python \
        libusb \
        arm-none-eabi-gcc \
        arm-none-eabi-binutils \
        arm-none-eabi-newlib
}

install_dnf_deps() {
    have_cmd dnf || die "dnf was not found"

    run_root dnf install -y \
        cmake \
        ninja-build \
        make \
        gcc \
        gcc-c++ \
        git \
        curl \
        wget \
        tar \
        xz \
        python3 \
        libusb1-devel \
        arm-none-eabi-gcc-cs \
        arm-none-eabi-binutils-cs \
        arm-none-eabi-newlib
}

info "Checking build dependencies"

if [[ "${SKIP_DEPS:-0}" == "1" ]]; then
    echo "SKIP_DEPS=1 set; not installing packages"
else
    case "$PKG_FAMILY" in
        brew)   install_brew_deps ;;
        apt)    install_apt_deps ;;
        pacman) install_pacman_deps ;;
        dnf)    install_dnf_deps ;;
        *)      die "Unsupported package family: $PKG_FAMILY" ;;
    esac
fi

mkdir -p "$DEPS" "$DIST"

# Homebrew may have been added during install; refresh PATH.
if [[ "$PKG_FAMILY" == "brew" ]]; then
    ensure_brew_in_path || true
fi

# ============================================================
# Board selection
# ============================================================

info "Selecting target board"

if [[ -n "${PICO_BOARD:-}" ]]; then
    printf '%s\n' "${BOARDS[@]}" | grep -Fxq "$PICO_BOARD" \
        || die "Unsupported PICO_BOARD: $PICO_BOARD"
else
    echo "Choose target board:"
    select BOARD in "${BOARDS[@]}"; do
        if [[ -n "${BOARD:-}" ]]; then
            PICO_BOARD="$BOARD"
            break
        fi
    done
fi

echo "Selected board: $PICO_BOARD"

# ============================================================
# ARM toolchain
# ============================================================

info "Locating ARM GNU toolchain"

if [[ "$PKG_FAMILY" == "brew" ]]; then
    if have_cmd brew; then
        LIBUSB_PREFIX="$(brew --prefix libusb 2>/dev/null || true)"
        if [[ -n "$LIBUSB_PREFIX" ]]; then
            export PKG_CONFIG_PATH="${LIBUSB_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        fi
    fi

    # Official Arm cask first, then Homebrew bin, then PATH.
    for cand in \
        /Applications/ArmGNUToolchain/*/arm-none-eabi/bin \
        /opt/homebrew/bin \
        /usr/local/bin
    do
        for dir in $cand; do
            if [[ -x "$dir/arm-none-eabi-gcc" ]]; then
                case ":$PATH:" in
                    *":$dir:"*) ;;
                    *) PATH="$dir:$PATH" ;;
                esac
            fi
        done
    done
    export PATH
fi

pick_working_arm_gcc() {
    local cand resolved specs_ok
    local -a found=()

    if have_cmd arm-none-eabi-gcc; then
        found+=("$(command -v arm-none-eabi-gcc)")
    fi

    for cand in \
        /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/arm-none-eabi-gcc \
        /opt/homebrew/bin/arm-none-eabi-gcc \
        /usr/local/bin/arm-none-eabi-gcc
    do
        for resolved in $cand; do
            if [[ -x "$resolved" ]]; then
                found+=("$resolved")
            fi
        done
    done

    ARM_GCC=""
    for cand in "${found[@]}"; do
        specs_ok=0
        if echo 'int main(void){return 0;}' | \
            "$cand" --specs=nosys.specs -mcpu=cortex-m33 -mthumb -x c - \
            -o /tmp/surrealboot-arm-specs-test.elf >/dev/null 2>&1
        then
            specs_ok=1
        fi
        rm -f /tmp/surrealboot-arm-specs-test.elf

        if [[ "$specs_ok" == "1" ]]; then
            ARM_GCC="$cand"
            return 0
        fi

        echo "Skipping broken toolchain (missing nosys.specs): $cand"
    done

    return 1
}

pick_working_arm_gcc \
    || die "No working arm-none-eabi-gcc with nosys.specs. On macOS run: brew uninstall --force arm-none-eabi-gcc arm-none-eabi-binutils && brew install --cask gcc-arm-embedded"

ARM_BIN_DIR="$(dirname "$(resolve_path "$ARM_GCC")")"
ARM_GXX="$ARM_BIN_DIR/arm-none-eabi-g++"
ARM_OBJCOPY="$ARM_BIN_DIR/arm-none-eabi-objcopy"
ARM_SIZE="$ARM_BIN_DIR/arm-none-eabi-size"

[[ -x "$ARM_GXX" ]] || die "arm-none-eabi-g++ not found next to gcc"
[[ -x "$ARM_OBJCOPY" ]] || die "arm-none-eabi-objcopy not found next to gcc"

# Prefer this toolchain over a leftover Homebrew formula in PATH.
export PATH="$ARM_BIN_DIR:$PATH"
export PICO_TOOLCHAIN_PATH="$ARM_BIN_DIR"

echo "ARM GCC:"
"$ARM_GCC" --version | head -n 1

echo "ARM toolchain directory:"
echo "  $ARM_BIN_DIR"
echo "nosys.specs: OK"

# ============================================================
# Pico SDK
#
# MUST include submodules.
# This fixes TinyUSB missing.
# ============================================================

info "Preparing Pico SDK $PICO_SDK_VERSION"

if [[ ! -d "$PICO_SDK/.git" ]]; then
    echo "Pico SDK not found. Cloning with recursive submodules..."

    rm -rf "$PICO_SDK"

    git clone \
        --depth 1 \
        --branch "$PICO_SDK_VERSION" \
        --recurse-submodules \
        https://github.com/raspberrypi/pico-sdk.git \
        "$PICO_SDK"
else
    echo "Using cached Pico SDK:"
    echo "  $PICO_SDK"

    pushd "$PICO_SDK" >/dev/null

    popd >/dev/null
fi

# Verify TinyUSB really exists.
TINYUSB_CMAKE="$PICO_SDK/src/rp2_common/tinyusb/CMakeLists.txt"
TINYUSB_SRC="$PICO_SDK/lib/tinyusb/src/tusb.c"

if [[ ! -f "$TINYUSB_CMAKE" ]]; then
    die "Pico SDK TinyUSB CMake integration is missing"
fi

if [[ ! -f "$TINYUSB_SRC" ]]; then
    echo
    echo "TinyUSB source is missing. Repairing submodules..."

    pushd "$PICO_SDK" >/dev/null
    git submodule sync --recursive
    git submodule update --init --recursive
    popd >/dev/null
fi

[[ -f "$TINYUSB_SRC" ]] \
    || die "TinyUSB is still missing after submodule repair"

export PICO_SDK_PATH="$PICO_SDK"

echo
echo "PICO_SDK_PATH=$PICO_SDK"
echo "PICO_TOOLCHAIN_PATH=$PICO_TOOLCHAIN_PATH"

# ============================================================
# USBLiter8 upstream
# ============================================================

info "Preparing USBLiter8 upstream source"

if [[ ! -d "$UPSTREAM_CACHE/.git" ]]; then
    echo "Cloning upstream source from:"
    echo "  $USBLITER8_REPO"

    git clone \
        --filter=blob:none \
        --no-checkout \
        "$USBLITER8_REPO" \
        "$UPSTREAM_CACHE"
else
    echo "Using cached upstream source:"
    echo "  $UPSTREAM_CACHE"
fi

pushd "$UPSTREAM_CACHE" >/dev/null

if ! git cat-file -e "$USBLITER8_REF^{commit}" 2>/dev/null; then
    echo "Fetching pinned commit..."
    git fetch origin "$USBLITER8_REF"
fi

git checkout --force --detach "$USBLITER8_REF"

popd >/dev/null

echo
echo "USBLiter8 HEAD:"
git -C "$UPSTREAM_CACHE" rev-parse HEAD

[[ "$(git -C "$UPSTREAM_CACHE" rev-parse HEAD)" == "$USBLITER8_REF" ]] \
    || die "USBLiter8 commit mismatch"

# ============================================================
# Prepare source tree
# ============================================================

info "Preparing build source tree"

rm -rf "$SRC"
mkdir -p "$SRC"

cp -a "$UPSTREAM_CACHE/." "$SRC/"

# Directly continue to payload when device is already PWNED.
# Also make Recovery mode devices return -2 (retry) instead of -1 (fatal).
python3 - "$SRC/exploit.c" <<'PYEXPLOIT'
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

# --- Patch 1: already-PWNED returns 1 (proceed to payload) ---
old = """    if (pwnd) {
        INFO("already PWNED!");
        return -2;
    }
"""

new = """    if (pwnd) {
        INFO("already PWNED! Skipping exploit and proceeding directly to payload.");
        return 1;
    }
"""

if old not in s:
    raise SystemExit("Could not find PWNED block in exploit.c")

s = s.replace(old, new, 1)

# --- Patch 2: Recovery mode devices return -2 (retry) instead of -1 (fatal) ---
# Original code:
#   if (dev_desc.idVendor != 0x5AC || dev_desc.idProduct != 0x1227) {
#       INFO("VID:0x%04X PID:0x%04X is not an Apple DFU device", ...);
#       return -1;
#   }
#
# Replace with a check that returns -2 for Recovery mode (retry/skip)
# so the exploit loop treats it as "no device yet" rather than fatal.

old_vid = '''    if (dev_desc.idVendor != 0x5AC || dev_desc.idProduct != 0x1227) {
        INFO("VID:0x%04X PID:0x%04X is not an Apple DFU device", dev_desc.idVendor, dev_desc.idProduct);
        return -1;
    }'''

new_vid = '''    if (dev_desc.idVendor == 0x5AC &&
        dev_desc.idProduct >= 0x1280 && dev_desc.idProduct <= 0x1283) {
        INFO("Apple Recovery device detected (PID=0x%04X), triggering DFU helper", dev_desc.idProduct);
        return -3;
    }
    if (dev_desc.idVendor != 0x5AC || dev_desc.idProduct != 0x1227) {
        /* Non-DFU or non-Apple device: return -2 so main loop retries cleanly without fatal red */
        return -2;
    }'''

if old_vid not in s:
    raise SystemExit("Could not find VID/PID check in exploit.c")

s = s.replace(old_vid, new_vid, 1)

p.write_text(s)
PYEXPLOIT


# Replace/add our custom files.
cp "$ROOT/CMakeLists.txt"   "$SRC/CMakeLists.txt"
cp "$ROOT/surreal_boot.c"   "$SRC/surreal_boot.c"
cp "$ROOT/surreal_boot.h"   "$SRC/surreal_boot.h"
cp "$ROOT/dfu_helper.c"     "$SRC/dfu_helper.c"
cp "$ROOT/dfu_helper.h"     "$SRC/dfu_helper.h"
cp "$ROOT/serial_flasher.c" "$SRC/serial_flasher.c"
cp "$ROOT/serial_flasher.h" "$SRC/serial_flasher.h"

# ============================================================
# Patch LED states
#
# Existing upstream LED states:
#   BOOTING
#   IDLE
#   RUNNING
#   SUCCESS
#   ERROR
#
# Add:
#   BOOT_PAYLOAD
#   BOOT_SUCCESS
#   DFU_HOLD_BUTTONS   (magenta/purple)
#   DFU_RELEASE_POWER  (cyan)
#   DFU_WAITING        (white blink)
# ============================================================

info "Patching LED states"

python3 - "$SRC/led.h" "$SRC/led.c" <<'PYLED'
from pathlib import Path
import sys

header = Path(sys.argv[1])
source = Path(sys.argv[2])

h = header.read_text()

if "LED_STATE_BOOT_PAYLOAD" not in h:
    old = """    LED_STATE_SUCCESS,
    LED_STATE_ERROR
"""
    new = """    LED_STATE_SUCCESS,
    LED_STATE_ERROR,
    LED_STATE_BOOT_PAYLOAD,
    LED_STATE_BOOT_SUCCESS,
    LED_STATE_DFU_HOLD_BUTTONS,
    LED_STATE_DFU_RELEASE_POWER,
    LED_STATE_DFU_WAITING
"""
    if old not in h:
        raise SystemExit("Could not find LED state enum")
    h = h.replace(old, new, 1)

header.write_text(h)

c = source.read_text()

if "case LED_STATE_BOOT_PAYLOAD" not in c:
    old = """        case LED_STATE_ERROR: {
            led_set_color(RED);
            led_set_blinking(0);
            break;
        }
"""

    new = """        case LED_STATE_ERROR: {
            led_set_color(RED);
            led_set_blinking(0);
            break;
        }

        case LED_STATE_BOOT_PAYLOAD: {
            led_set_color(GREEN);
            led_set_blinking(250);
            break;
        }

        case LED_STATE_BOOT_SUCCESS: {
            led_set_color(GREEN);
            led_set_blinking(0);
            break;
        }
"""

    if old not in c:
        raise SystemExit("Could not find LED_STATE_ERROR block")

    c = c.replace(old, new, 1)

# ---- Add DFU helper LED colours ----

# Define new colours (MAGENTA, CYAN, WHITE) alongside existing ones.
# NeoPixel path:
neo_anchor = "#define RED"
if neo_anchor in c and "MAGENTA" not in c:
    # Find the RED define line in the NeoPixel section
    red_line_neo = None
    for line in c.splitlines():
        if line.strip().startswith("#define RED") and "NEOPIXEL_RGB" in line:
            red_line_neo = line
            break

    if red_line_neo:
        c = c.replace(
            red_line_neo,
            red_line_neo + "\n"
            "#define MAGENTA     NEOPIXEL_RGB(18, 0, 18)\n"
            "#define CYAN        NEOPIXEL_RGB(0, 10, 18)\n"
            "#define WHITE       NEOPIXEL_RGB(10, 10, 10)",
            1
        )

# PWM path:
for line in c.splitlines():
    if line.strip().startswith("#define RED") and "PWM_RGB" in line:
        red_line_pwm = line
        if "MAGENTA" not in c or "PWM_RGB" not in c.split(red_line_pwm, 1)[-1][:200]:
            c = c.replace(
                red_line_pwm,
                red_line_pwm + "\n"
                "#define MAGENTA     PWM_RGB(120, 0, 120)\n"
                "#define CYAN        PWM_RGB(0, 120, 120)\n"
                "#define WHITE       PWM_RGB(100, 100, 100)",
                1
            )
        break

# Add DFU helper state cases to led_set_state().
# Insert after BOOT_SUCCESS case.
boot_success_block = """        case LED_STATE_BOOT_SUCCESS: {
            led_set_color(GREEN);
            led_set_blinking(0);
            break;
        }
"""

dfu_led_states = """        case LED_STATE_BOOT_SUCCESS: {
            led_set_color(GREEN);
            led_set_blinking(0);
            break;
        }

        case LED_STATE_DFU_HOLD_BUTTONS: {
            led_set_color(MAGENTA);
            led_set_blinking(0);
            break;
        }

        case LED_STATE_DFU_RELEASE_POWER: {
            led_set_color(CYAN);
            led_set_blinking(0);
            break;
        }

        case LED_STATE_DFU_WAITING: {
            led_set_color(WHITE);
            led_set_blinking(200);
            break;
        }
"""

if boot_success_block in c and "LED_STATE_DFU_HOLD_BUTTONS" not in c:
    c = c.replace(boot_success_block, dfu_led_states, 1)

source.write_text(c)
PYLED

# ============================================================
# Patch usb.h and usb.c for non-blocking device detection
# ============================================================

info "Patching usb.h and usb.c for non-blocking connection polling"

python3 - "$SRC/usb.h" "$SRC/usb.c" <<'PYUSB'
from pathlib import Path
import sys

h_path = Path(sys.argv[1])
h_text = h_path.read_text()
if 'usb_bus_is_connected' not in h_text:
    h_text += '\nbool usb_bus_is_connected(void);\n'
    h_path.write_text(h_text)

c_path = Path(sys.argv[2])
c_text = c_path.read_text()
if 'usb_bus_is_connected' not in c_text:
    c_text += '\nbool usb_bus_is_connected(void) {\n    return (gBus.root != NULL && gBus.root->connected);\n}\n'
    c_path.write_text(c_text)
PYUSB

# ============================================================
# Patch upstream main.c
# ============================================================

info "Patching USBLiter8 main runtime"

python3 - "$SRC/main.c" <<'PYMAIN'
from pathlib import Path
import sys

path = Path(sys.argv[1])
s = path.read_text()

# --- Add includes ---

if '#include "surreal_boot.h"' not in s:
    marker = '#include "log.h"'
    if marker not in s:
        raise SystemExit('Could not find log.h include')
    s = s.replace(
        marker,
        marker + '\n#include "surreal_boot.h"\n#include "dfu_helper.h"\n#include "serial_flasher.h"',
        1
    )

# --- Patch fatal_failure to keep servicing serial flasher ---
old_fatal_spin = """    while (1) {
        sleep_ms(100);
    }"""

new_fatal_spin = """    while (1) {
        serial_flasher_check(100);
        sleep_ms(100);
    }"""

if old_fatal_spin in s:
    s = s.replace(old_fatal_spin, new_fatal_spin)

# --- Patch do_auto for dynamic Recovery DFU helper and serial flasher ---
old_auto = """void do_auto(void) {
    while (true) {
        int ret = exploit_run();"""

new_auto = """void do_auto(void) {
    while (true) {
        serial_flasher_check(10);
        int ret = exploit_run();

        if (ret == -3) {
            printf("[AUTO] Apple Recovery device detected -> launching DFU helper!\\n");
            dfu_helper_run();
            usb_bus_reset_open_ep0();
            continue;
        }"""

if old_auto not in s:
    raise SystemExit('Could not find do_auto() while loop in main.c')

s = s.replace(old_auto, new_auto, 1)

# --- Patch exploit success to send boot payload ---
old = """        /* it all went well then */
        break;
"""

new = """        /* exploit succeeded; now send embedded boot payload */
        led_set_state(LED_STATE_SUCCESS);

        printf("\\n");
        printf("[BOOT] exploit succeeded; starting embedded boot payload transfer\\n");

        led_set_state(LED_STATE_BOOT_PAYLOAD);

        int boot_rc = surreal_boot_run();

        if (boot_rc != 0) {
            printf("[BOOT] payload transfer FAILED rc=%d\\n", boot_rc);
            led_set_state(LED_STATE_ERROR);
            fatal_failure();
        }

        led_set_state(LED_STATE_BOOT_SUCCESS);

        printf("[BOOT] payload transfer SUCCESS\\n");
        printf("[BOOT] device should now transition away from DFU\\n");

        break;
"""

if old not in s:
    raise SystemExit("Could not find exploit success point in main.c")

s = s.replace(old, new, 1)

# --- Replace blocking main() loop with non-blocking polling ---
old_main_tail = """    usb_start();
    usb_bus_init();
    usb_bus_wait_for_device();
    usb_bus_reset_open_ep0();

#if WITH_AUTO_MODE
    do_auto();
#else
    do_shell();
#endif"""

new_main_tail = """    usb_start();
    usb_bus_init();
    usb_bus_reset_open_ep0();

    do_auto();"""

if old_main_tail in s:
    s = s.replace(old_main_tail, new_main_tail, 1)

path.write_text(s)
PYMAIN

# ============================================================
# Validate ibss
# ============================================================

info "Checking embedded ibss"

BOOTFILES_DIR="$ROOT/ibss"

[[ -d "$BOOTFILES_DIR" ]] \
    || die "Missing ./ibss directory"

BOOTFILES=()
while IFS= read -r file; do
    [[ -n "$file" ]] && BOOTFILES+=("$file")
done < <(
    find "$BOOTFILES_DIR" \
        -type f \
        -name "*.boot" \
        -print \
        | sort
)

if [[ "${#BOOTFILES[@]}" -eq 0 ]]; then
    echo "Notice: No .boot files found in ./ibss"
    echo "Building universal base firmware (will load payload dynamically from flash at 0x10040000)"
    TOTAL_BYTES=0
else

TOTAL_BYTES=0

echo "Found ${#BOOTFILES[@]} boot payload(s):"

for file in "${BOOTFILES[@]}"; do
    size="$(file_size "$file")"
    TOTAL_BYTES=$((TOTAL_BYTES + size))

    echo "  ${file#"$ROOT/ibss"/} $size bytes"
done

echo
echo "Total boot payload bytes: $TOTAL_BYTES"
fi

# ============================================================
# Generate embedded payloads
#
# IMPORTANT:
# embed_bootfiles.py requires THREE arguments:
#
#   ibss directory
#   output .c
#   output .h
# ============================================================

info "Generating embedded boot payload"

GEN_DIR="$SRC/generated"

rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"

EMBED_SCRIPT="$ROOT/tools/embed_bootfiles.py"

[[ -f "$EMBED_SCRIPT" ]] \
    || die "Missing tools/embed_bootfiles.py"

python3 "$EMBED_SCRIPT" \
    "$ROOT/ibss" \
    "$GEN_DIR/bootfiles_data.c" \
    "$GEN_DIR/bootfiles_data.h"

[[ -s "$GEN_DIR/bootfiles_data.c" ]] \
    || die "Generated bootfiles_data.c is empty"

[[ -s "$GEN_DIR/bootfiles_data.h" ]] \
    || die "Generated bootfiles_data.h is empty"

echo
echo "Generated:"
echo "  $GEN_DIR/bootfiles_data.c"
echo "  $GEN_DIR/bootfiles_data.h"

# ============================================================
# Build
# ============================================================

info "Configuring CMake"

rm -rf "$BUILD"

have_cmd cmake || die "cmake not found"
have_cmd ninja || die "ninja not found"
have_cmd python3 || die "python3 not found"

cmake \
    -S "$SRC" \
    -B "$BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DPICO_BOARD="$PICO_BOARD" \
    -DPICO_SDK_PATH="$PICO_SDK_PATH" \
    -DPICO_TOOLCHAIN_PATH="$PICO_TOOLCHAIN_PATH"

info "Building"

cmake --build "$BUILD" --parallel "$(cpu_jobs)"

# ============================================================
# Verify result
# ============================================================

UF2="$BUILD/usbliter8.uf2"
ELF="$BUILD/usbliter8.elf"

[[ -f "$UF2" ]] \
    || die "Build finished but usbliter8.uf2 was not produced"

[[ -f "$ELF" ]] \
    || die "Build finished but usbliter8.elf was not produced"

mkdir -p "$DIST"

OUTPUT="$DIST/usbliter8-bootsurreal-$PICO_BOARD.uf2"

cp -f "$UF2" "$OUTPUT"

# ============================================================
# Size report
# ============================================================

echo

if [[ -n "$ARM_SIZE" ]]; then
    "$ARM_SIZE" "$ELF"
fi

echo
echo "UF2:"
ls -lh "$OUTPUT"

echo
echo "============================================================"
echo "BUILD SUCCESS"
echo "============================================================"
echo
echo "Host:"
echo "  $HOST_DISTRO ($HOST_ARCH)"
echo
echo "Board:"
echo "  $PICO_BOARD"
echo
echo "UF2:"
echo "  $OUTPUT"
echo
echo "Embedded payloads:"
for file in "${BOOTFILES[@]}"; do
    echo "  ${file#"$ROOT/ibss"/}"
done
echo
echo "DONE: $OUTPUT"
