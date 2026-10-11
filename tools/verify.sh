#!/usr/bin/env bash
# The definition of "verified", run inside the ESP-IDF image (S-10). tools/verify.ps1 starts it
# there with the repository at /project, a build volume at /build and a cache volume at /ccache;
# CI runs verify.ps1 and nothing else. Five steps, each to out/verify/<step>.log, one summary line
# each, exit 1 when anything failed:
#
#   spec        python spec/tools/specsync.py verify spec     - the copy is byte for byte the spec's
#   spec-tools  linkref.py check, manifestref.py check        - the spec's own reference code agrees
#               with its fixtures (cryptography pinned in tools/requirements-verify.txt)
#   host-tests  host_test/ under ASan+UBSan, ctest            - the logic
#   build       idf.py build, merge-bin, size                 - the firmware, images to out/images/
#   partitions  tools/check_partitions.py --self-test, then against the build
#
#   tools/verify.sh            all five
#   tools/verify.sh <step>     one of them (verify.ps1 -Only <step>)
#
# The build directory is /build/fw, in the volume and never on the bind-mounted tree: a build over
# a Windows bind mount is several times slower. ccache lives in /ccache so the second run is quick.
set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD=${TWINOAK_BUILD_DIR:-/build}
CACHE=${TWINOAK_CACHE_DIR:-/ccache}
OUT=$ROOT/out
LOGS=$OUT/verify
IMAGES=$OUT/images
PROJECT_NAME=twinoak-walter
STEPS="spec spec-tools host-tests build partitions"
ONLY=${1:-}

mkdir -p "$LOGS" "$IMAGES" "$BUILD" "$CACHE"
export CCACHE_DIR=$CACHE/ccache
export IDF_CCACHE_ENABLE=1
export PIP_CACHE_DIR=$CACHE/pip
export IDF_COMPONENT_CACHE_PATH=$CACHE/components
export PYTHONUNBUFFERED=1
# The checkout is bind-mounted and owned by another user as far as this container is concerned.
git config --global --add safe.directory "$ROOT" 2>/dev/null || true

summary=()
failed=0

step_spec() {
  python spec/tools/specsync.py verify spec
}

step_spec_tools() {
  pip install --disable-pip-version-check --quiet -r tools/requirements-verify.txt &&
  python -c "import cryptography; print('cryptography', cryptography.__version__)" &&
  python spec/tools/linkref.py check spec/fixtures/link/link-v1.json &&
  python spec/tools/manifestref.py check spec/fixtures/manifest
}

step_host_tests() {
  local dir=$BUILD/host
  cmake -S host_test -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
  cmake --build "$dir" &&
  ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

step_build() {
  local dir=$BUILD/fw
  # sdkconfig.defaults is the configuration: when it changes, the generated sdkconfig is dropped so
  # that the change takes effect (ESP-IDF only fills in options that are missing from sdkconfig).
  local stamp=$dir/sdkconfig.defaults.sha256
  local want; want=$(sha256sum sdkconfig.defaults | cut -d' ' -f1)
  if [ -f "$dir/sdkconfig" ] && [ "$(cat "$stamp" 2>/dev/null)" != "$want" ]; then
    echo "sdkconfig.defaults changed: regenerating sdkconfig"
    rm -f "$dir/sdkconfig" "$dir/sdkconfig.old"
  fi
  mkdir -p "$dir" && echo "$want" > "$stamp"
  local t0; t0=$(date +%s)
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" build || return 1
  echo "idf.py build: $(( $(date +%s) - t0 )) s"
  # The factory image: bootloader, table, otadata and application as one file from 0x0 (what a
  # board out of the box is flashed with; the release names it in task 02).
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" merge-bin -o "$PROJECT_NAME-factory.bin" || return 1
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" size || return 1
  # Evidence to out/images/: what a bench flashes, and what task 02 releases.
  cp "$dir/$PROJECT_NAME.bin" "$dir/$PROJECT_NAME.elf" "$dir/$PROJECT_NAME.map" \
     "$dir/$PROJECT_NAME-factory.bin" "$dir/bootloader/bootloader.bin" \
     "$dir/partition_table/partition-table.bin" "$dir/ota_data_initial.bin" \
     "$dir/flasher_args.json" "$dir/sdkconfig" "$IMAGES/" || return 1
  ls -la "$IMAGES"
  # The banner is in the image once: the version string the console prints (core/version.cpp).
  local banner="twinoak-walter "
  if ! grep -a -q "$banner" "$dir/$PROJECT_NAME.bin"; then
    echo "the application image does not contain the banner '$banner...'"; return 1
  fi
  echo "banner in image: $(strings -n 8 "$dir/$PROJECT_NAME.bin" | grep -m1 '^twinoak-walter ')"
  # The modem library was compiled: its objects are in the build, and the anchor is in the map.
  local lib=$dir/esp-idf/dptechnics__walter-modem/libdptechnics__walter-modem.a
  if [ ! -f "$lib" ]; then echo "walter-modem was not built: $lib missing"; return 1; fi
  echo "walter-modem: $(ar t "$lib" | tr '\n' ' ')"
  grep -q "twinoak_modem_library_anchor" "$dir/$PROJECT_NAME.map" || { echo "anchor not linked"; return 1; }
  echo "walter-modem is in the link ($(grep -c 'dptechnics__walter-modem' "$dir/$PROJECT_NAME.map") references in the map)"
  echo "ccache: $(ccache -s 2>/dev/null | grep -E 'Hits|Misses|hit rate' | tr -s ' ' | tr '\n' ';')"
}

step_partitions() {
  local dir=$BUILD/fw
  python tools/check_partitions.py --self-test || return 1
  if [ ! -f "$dir/sdkconfig" ]; then echo "no build to check against ($dir/sdkconfig)"; return 1; fi
  python tools/check_partitions.py --table partitions.csv --sdkconfig "$dir/sdkconfig" \
         --bin "$dir/partition_table/partition-table.bin" --app "$dir/$PROJECT_NAME.bin" \
         --factory "$dir/$PROJECT_NAME-factory.bin"
}

run_step() {
  local name=$1 fn=$2 log=$LOGS/$1.log
  local t0; t0=$(date +%s)
  echo "== $name"
  if "$fn" > "$log" 2>&1; then
    local line="ok    $name ($(( $(date +%s) - t0 )) s)"
  else
    local line="FAIL  $name ($(( $(date +%s) - t0 )) s) - see out/verify/$name.log"
    failed=1
    tail -n 40 "$log"
  fi
  echo "$line"
  summary+=("$line")
}

if [ -n "$ONLY" ]; then
  case " $STEPS " in *" $ONLY "*) STEPS=$ONLY ;; *) echo "unknown step '$ONLY'; one of: $STEPS"; exit 2 ;; esac
fi

echo "verify: ESP-IDF $(cd "$IDF_PATH" && git describe --tags 2>/dev/null || cat version.txt) in $(grep PRETTY_NAME /etc/os-release | cut -d= -f2), build dir $BUILD, cache $CACHE"
T0=$(date +%s)
for s in $STEPS; do
  case $s in
    spec)        run_step spec step_spec ;;
    spec-tools)  run_step spec-tools step_spec_tools ;;
    host-tests)  run_step host-tests step_host_tests ;;
    build)       run_step build step_build ;;
    partitions)  run_step partitions step_partitions ;;
  esac
done
echo
echo "verify summary ($(( $(date +%s) - T0 )) s):"
printf '  %s\n' "${summary[@]}"
printf '%s\n' "${summary[@]}" > "$LOGS/summary.txt"
if [ "$failed" -ne 0 ]; then echo "verify: FAILED"; exit 1; fi
echo "verify: green"
