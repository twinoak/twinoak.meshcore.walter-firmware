#!/usr/bin/env bash
# The definition of "verified", run inside the ESP-IDF image (S-10). tools/verify.ps1 starts it
# there with the repository at /project, a build volume at /build and a cache volume at /ccache;
# CI runs verify.ps1 and nothing else. Seven steps, each to out/verify/<step>.log, one summary
# line each, exit 1 when anything failed:
#
#   spec          python spec/tools/specsync.py verify spec   - the copy is byte for byte the spec's
#   spec-tools    linkref.py check, manifestref.py check      - the spec's own reference code agrees
#                 with its fixtures (cryptography pinned in tools/requirements-verify.txt)
#   host-tests    host_test/ under ASan+UBSan, ctest          - the logic; first a grep that no GPIO
#                 call exists outside components/platform/ and that the seven radio pins are
#                 named only in components/core/board/ (task p0-03)
#   release-tests tools/release/test_release.py               - what the release tooling refuses
#   build         idf.py build, merge-bin, size               - the firmware, images to out/images/
#   partitions    tools/check_partitions.py --self-test, then against the build
#   release       a dry run of the whole release over the build just made, into out/release/:
#                 build_release.py, signed with the specification's public test key, then verified
#
#   tools/verify.sh            all seven
#   tools/verify.sh <step>     one of them (verify.ps1 -Only <step>)
#
# TWINOAK_VERSION (verify.ps1 -Version, and the release workflow) is the version baked into the
# build and carried by the dry run's manifest and by the descriptor inside the image. Without it,
# "0.0.0-dev" - one string for all three, so that they cannot disagree.
#
# TWINOAK_TRUST_TEST_KEY=1 (verify.ps1 -TestKey; task p2-01) builds the test-key variant: the
# specification's public test key in the trust table, the version stamped +TESTKEY, a warning at
# every boot. The build step then expects the opposite of what it expects of a normal build (the
# key's bytes present, +TESTKEY in the banner) and the release step expects build_release.py to
# refuse it. A normal build is held to: no test key bytes in the image, no +TESTKEY, and no
# signing code in the link.
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
RELEASE=$OUT/release
PROJECT_NAME=twinoak-walter
STEPS="spec spec-tools host-tests release-tests build partitions release"
ONLY=${1:-}
# One version string for the build, the descriptor inside the image and the dry run's manifest
# (spec/FIRMWARE-MANIFEST.md §4: the three must say the same thing), and the tag that names the
# files - the part of the version before the "+", as vX.Y.Z.
VERSION=${TWINOAK_VERSION:-0.0.0-dev}
RELEASE_TAG=v${VERSION%%+*}
# 0 or 1, always passed to idf.py explicitly: a cmake cache variable sticks between runs.
TESTKEY=${TWINOAK_TRUST_TEST_KEY:-0}
case $TESTKEY in 0|1) ;; *) echo "TWINOAK_TRUST_TEST_KEY must be 0 or 1"; exit 2 ;; esac

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

# Who may touch a pin (task p0-03, docs/ARCHITECTURE.md §2): the GPIO driver is called from
# components/platform/ and nowhere else, and the seven radio-facing pins (core/board/pin_map.h)
# are named by RadioPins and nowhere else in the firmware. A grep, not a promise.
check_pin_ownership() {
  local hits
  hits=$(grep -rnE '\b(gpio_[a-z_]+\(|GPIO_NUM_[0-9]+|GPIO_MODE_)' components main \
           --include='*.cpp' --include='*.h' --include='*.c' | grep -v '^components/platform/' || true)
  if [ -n "$hits" ]; then
    echo "a GPIO call outside components/platform/:"; echo "$hits"; return 1
  fi
  echo "no GPIO call outside components/platform/"
  hits=$(grep -rnE '\bk(NReset|SwdClk|SwdIo|PwrKill|Spare|WalterTx|WalterRx|RadioPins)\b' components main \
           --include='*.cpp' --include='*.h' | grep -v -E '^components/core/(board/|include/core/board/)' || true)
  if [ -n "$hits" ]; then
    echo "one of the seven radio pins named outside RadioPins (components/core/board/):"; echo "$hits"; return 1
  fi
  echo "the seven radio pins are named only in components/core/board/"
}

# The dual adapter's selector is switched off (task p1-06, docs/ARCHITECTURE.md §2 rule 6): the
# firmware constructs a SingleTarget and nothing else, so the sequence that steers a dual
# adapter's flip-flops - the one sanctioned "SWDCLK toggles while nRESET is asserted" - cannot
# reach the wires of a unit with one radio. DualTarget and selectorProgram() are compiled and
# tested on the host; they are named here only where they are defined. A grep, not a promise;
# host_test/core/board/test_slot_single.cpp is the other half.
check_selector_off() {
  local hits
  # Comment lines are dropped: main/main.cpp says in words that it builds a SingleTarget and not
  # this, and saying so must not be what trips the check.
  hits=$(grep -rnE '\b(DualTarget|selectorProgram)\b' components main \
           --include='*.cpp' --include='*.h' \
           | grep -vE ':[[:space:]]*(//|\*)' \
           | grep -v -E '^components/core/(board/(dual_target|radio_pins)\.cpp|include/core/board/(dual_target|radio_pins)\.h)' || true)
  if [ -n "$hits" ]; then
    echo "the dual adapter's selector is named outside core/board/dual_target and radio_pins:"
    echo "$hits"; return 1
  fi
  echo "the dual adapter's selector is compiled and constructed nowhere (SingleTarget only)"
}

step_host_tests() {
  local dir=$BUILD/host
  check_pin_ownership || return 1
  check_selector_off || return 1
  cmake -S host_test -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
  cmake --build "$dir" &&
  ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

# What a build trusts (task p2-01, spec/FIRMWARE-MANIFEST.md §3 and §5). A normal build carries
# the keys of keys/release-keys.txt and nothing else: the specification's public test key is not
# among its bytes, its version is not stamped +TESTKEY, and the linker map has none of the
# signing symbols a full TweetNaCl would have (components/core/third_party/tweetnacl/README.md:
# the extract keeps TweetNaCl's names, so the signer would be crypto_sign_ed25519_tweet). A
# test-key build (TWINOAK_TRUST_TEST_KEY=1) must carry the key and the stamp, and still no signing
# code. The key is read from the fixture index, never written down here.
check_trust_in_image() {
  local dir=$1
  local map=$dir/$PROJECT_NAME.map
  local hits
  hits=$(grep -nE '\b(crypto_sign_ed25519_tweet|crypto_sign_ed25519_tweet_keypair|randombytes|crypto_sign_keypair)\b' "$map" || true)
  if [ -n "$hits" ]; then
    echo "signing code is linked into the image:"; echo "$hits"; return 1
  fi
  echo "no Ed25519 signing symbol in the linker map (crypto_sign_ed25519_tweet, _keypair, randombytes)"
  # The stamp is looked for in the banner ("twinoak-walter <version>+TESTKEY"), not as the bare
  # word: the console's and main's warning texts name +TESTKEY in every build.
  python - "$dir/$PROJECT_NAME.bin" "$dir/$PROJECT_NAME-factory.bin" "$TESTKEY" "$VERSION" <<'EOF' || return 1
import json, sys
key = bytes.fromhex(json.load(open("spec/fixtures/manifest/index.json"))["test_public_key"])
want = sys.argv[3] == "1"
stamped = ("twinoak-walter %s+TESTKEY" % sys.argv[4]).encode("ascii")
bad = 0
for path in sys.argv[1:3]:
    data = open(path, "rb").read()
    has_key = key in data
    has_stamp = stamped in data
    print("%s: test key bytes %s, banner stamped +TESTKEY %s" % (path.rsplit("/", 1)[-1],
          "PRESENT" if has_key else "absent", "PRESENT" if has_stamp else "absent"))
    if has_key != want or has_stamp != want:
        bad += 1
if bad:
    print("a %s build must %s the test key and the +TESTKEY stamp" % (
        "test-key" if want else "normal", "carry" if want else "not carry"))
    sys.exit(1)
EOF
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
  # -DPROJECT_VER is always given, even when it is the default: it is a cmake cache variable, and a
  # build that inherited one from an earlier run would say a version nobody asked for.
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" -DPROJECT_VER="$VERSION" \
         -DTWINOAK_TRUST_TEST_KEY="$TESTKEY" build || return 1
  echo "idf.py build: $(( $(date +%s) - t0 )) s"
  # The factory image: bootloader, table, otadata and application as one file from 0x0 (what a
  # board out of the box is flashed with; the release publishes it as -factory.bin).
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" -DPROJECT_VER="$VERSION" \
         -DTWINOAK_TRUST_TEST_KEY="$TESTKEY" merge-bin -o "$PROJECT_NAME-factory.bin" || return 1
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" -DPROJECT_VER="$VERSION" \
         -DTWINOAK_TRUST_TEST_KEY="$TESTKEY" size || return 1
  # Evidence to out/images/: what a bench flashes, and what task 02 releases.
  cp "$dir/$PROJECT_NAME.bin" "$dir/$PROJECT_NAME.elf" "$dir/$PROJECT_NAME.map" \
     "$dir/$PROJECT_NAME-factory.bin" "$dir/bootloader/bootloader.bin" \
     "$dir/partition_table/partition-table.bin" "$dir/ota_data_initial.bin" \
     "$dir/flasher_args.json" "$dir/project_description.json" "$dir/sdkconfig" \
     "$IMAGES/" || return 1
  # project_description.json travels with the images so that out/images/ is enough for
  # tools/release/build_release.py --build-dir: the release workflow runs it on the runner, over
  # the artifact this step produced, and the build directory itself is a docker volume.
  ls -la "$IMAGES"
  # The banner is in the image once: the version string the console prints (core/version.cpp).
  # A test-key build's banner says +TESTKEY (CMakeLists.txt).
  local banner="twinoak-walter $VERSION"
  if [ "$TESTKEY" = 1 ]; then banner="$banner+TESTKEY"; fi
  if ! grep -a -q "$banner" "$dir/$PROJECT_NAME.bin"; then
    echo "the application image does not contain the banner '$banner'"; return 1
  fi
  # (-o: the banner's own bytes, wherever .rodata put them - after task p2-01 the literal before
  # it is not always terminated the way `strings` would start a new line.)
  echo "banner in image: $(grep -a -o -m1 'twinoak-walter [0-9A-Za-z.+-]*' "$dir/$PROJECT_NAME.bin" | head -1)"
  check_trust_in_image "$dir" || return 1
  # And the 96 bytes with which the image says what it is (spec/FIRMWARE-MANIFEST.md §4, task
  # p0-02): exactly one in each file, which is what manifestref.py descriptor insists on. The
  # release step then holds it against the manifest entry; this is the build's own check that the
  # linker kept the object and that its offset is on a 4-byte boundary.
  for d in "$dir/$PROJECT_NAME.bin" "$dir/$PROJECT_NAME-factory.bin"; do
    local line
    line=$(python spec/tools/manifestref.py descriptor "$d") || { echo "no single descriptor in $d"; return 1; }
    echo "descriptor in $(basename "$d") $line"
  done
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

step_release_tests() {
  # The release tooling's own tests: what it refuses, and why (tools/release/test_release.py).
  # They need no firmware - a stand-in image with a descriptor in it is enough - so they run
  # before the build, where a mistake in them is cheapest to find. Then the trust table
  # generator's (task p2-01), and the check that the vendored TweetNaCl extract is what it says.
  python tools/release/test_release.py &&
  python tools/test_gen_trust_table.py &&
  python tools/vendor_tweetnacl_verify.py selfcheck components/core/third_party/tweetnacl/tweetnacl_verify.c
}

step_release() {
  # A dry run of the release over the build just made, end to end, exactly as
  # .github/workflows/release.yml does it on a tag - except that it signs with the specification's
  # public test key and publishes nothing. The point is that the tag path is not run for the first
  # time on a tag (S-10: local first).
  local dir=$BUILD/fw
  if [ ! -f "$dir/$PROJECT_NAME.bin" ]; then
    echo "no build to release ($dir/$PROJECT_NAME.bin); run the build step first"; return 1
  fi
  local commit
  commit=$(git rev-parse HEAD 2>/dev/null || true)
  case $commit in
    *[!0-9a-f]* | "") commit=0000000000000000000000000000000000000000 ;;
  esac
  rm -rf "$RELEASE"
  if [ "$TESTKEY" = 1 ]; then
    # A test-key build is never released: the tooling must refuse it, by the +TESTKEY in its
    # version and by the key's bytes in the image, and that refusal is this step's whole result.
    if python tools/release/build_release.py --version "$VERSION+TESTKEY" --commit "$commit" \
         --tag "$RELEASE_TAG" --build-dir "$dir" --out "$RELEASE" --require-descriptor; then
      echo "build_release.py accepted a test-key build"; return 1
    fi
    if python tools/release/build_release.py --version "$VERSION" --commit "$commit" \
         --tag "$RELEASE_TAG" --build-dir "$dir" --out "$RELEASE" --require-descriptor; then
      echo "build_release.py accepted a test-key build under a plain version"; return 1
    fi
    echo "build_release.py refused the test-key build twice, as it must; nothing released"
    return 0
  fi
  python tools/release/build_release.py --version "$VERSION" --commit "$commit" \
         --tag "$RELEASE_TAG" --build-dir "$dir" --out "$RELEASE" --require-descriptor || return 1
  # The signature. --test-key takes the key pair the specification publishes, so a rehearsal has
  # no secret anywhere near it and nothing to print; it refuses a release version.
  python tools/release/sign_manifest.py --test-key "$RELEASE/manifest.json" || return 1
  # The gate the workflow runs before `gh release create`: all of §5 that applies here.
  python tools/release/verify_release.py --test-key "$RELEASE" || return 1
  # And the specification's own tool, by itself, over what was written - the task's acceptance
  # criterion. The public test key comes from the fixtures, never written down a second time.
  local public
  public=$(python -c 'import json;print(json.load(open("spec/fixtures/manifest/index.json"))["test_public_key"])') || return 1
  python spec/tools/manifestref.py verify "$public" "$RELEASE/manifest.json" || return 1
  local f line
  for f in "$RELEASE/$PROJECT_NAME-$RELEASE_TAG.bin" "$RELEASE/$PROJECT_NAME-$RELEASE_TAG-factory.bin"; do
    line=$(python spec/tools/manifestref.py descriptor "$f") || return 1
    echo "$(basename "$f") $line"
    case $line in
      *'"role": "manager"'*) ;;
      *) echo "the descriptor in $f does not say role manager"; return 1 ;;
    esac
  done
  ls -la "$RELEASE"
  echo "--- manifest.json"
  cat "$RELEASE/manifest.json"
  echo "--- manifest.json.sig"
  cat "$RELEASE/manifest.json.sig"
  echo "--- SHA256SUMS"
  cat "$RELEASE/SHA256SUMS"
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
echo "        version $VERSION, release files named $RELEASE_TAG$([ "$TESTKEY" = 1 ] && echo '; A TEST-KEY BUILD (+TESTKEY), never released')"
T0=$(date +%s)
for s in $STEPS; do
  case $s in
    spec)        run_step spec step_spec ;;
    spec-tools)  run_step spec-tools step_spec_tools ;;
    host-tests)  run_step host-tests step_host_tests ;;
    release-tests) run_step release-tests step_release_tests ;;
    build)       run_step build step_build ;;
    partitions)  run_step partitions step_partitions ;;
    release)     run_step release step_release ;;
  esac
done
echo
echo "verify summary ($(( $(date +%s) - T0 )) s):"
printf '  %s\n' "${summary[@]}"
printf '%s\n' "${summary[@]}" > "$LOGS/summary.txt"
if [ "$failed" -ne 0 ]; then echo "verify: FAILED"; exit 1; fi
echo "verify: green"
