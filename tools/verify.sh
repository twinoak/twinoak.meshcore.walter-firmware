#!/usr/bin/env bash
# The definition of "verified", run inside the ESP-IDF image (S-10). tools/verify.ps1 starts it
# there with the repository at /project, a build volume at /build and a cache volume at /ccache;
# CI runs verify.ps1 and nothing else. Eight steps, each to out/verify/<step>.log, one summary
# line each, exit 1 when anything failed:
#
#   spec          python spec/tools/specsync.py verify spec   - the copy is byte for byte the spec's
#   spec-tools    linkref.py check, manifestref.py check      - the spec's own reference code agrees
#                 with its fixtures (cryptography pinned in tools/requirements-verify.txt)
#   deps          idf.py reconfigure                          - the managed components resolved
#                 into managed_components/ before anything compiles against them (task p4-01:
#                 host_test/ builds agent-core against the ArduinoJson the board gets)
#   host-tests    tools/check_rails.py, then host_test/ under ASan+UBSan, ctest
#                 - **the rails** first (task p3-06: one entry per rail, the greps that used to be
#                 in this file among them - pin ownership, the dual adapter's selector, core/ and
#                 the modem library, TLS - and, for a rail held by a test, that the test is still
#                 there); then the logic, with host_test/rails/ holding the assertions that are
#                 about every test in the suite
#   release-tests tools/release/test_release.py               - what the release tooling refuses
#   build         idf.py build, merge-bin, size, heap_at_start.py
#                 - the firmware, then **the heap at start** (task p11-02: the main task's stack
#                 has to have a block, with HEAP_MIN_SPARE to spare, or the image cannot start and
#                 is never copied anywhere), then images to out/images/
#   partitions    tools/check_partitions.py --self-test, then against the build
#   release       a dry run of the whole release over the build just made, into out/release/:
#                 build_release.py, signed with the specification's public test key, then verified
#                 - except at a release's own version (TWINOAK_VERSION=X.Y.Z+<sha7>, which is what
#                 the release workflow's build job passes), where the test key may not sign and the
#                 signature is the workflow's: see step_release
#
#   tools/verify.sh            all eight
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
STEPS="spec spec-tools deps host-tests release-tests build partitions release"
ONLY=${1:-}
# One version string for the build, the descriptor inside the image and the dry run's manifest
# (spec/FIRMWARE-MANIFEST.md §4: the three must say the same thing), and the tag that names the
# files - the part of the version before the "+", as vX.Y.Z.
VERSION=${TWINOAK_VERSION:-0.0.0-dev}
RELEASE_TAG=v${VERSION%%+*}
# 0 or 1, always passed to idf.py explicitly: a cmake cache variable sticks between runs.
TESTKEY=${TWINOAK_TRUST_TEST_KEY:-0}
case $TESTKEY in 0|1) ;; *) echo "TWINOAK_TRUST_TEST_KEY must be 0 or 1"; exit 2 ;; esac
# The rail of task p11-02: how much must be left in the block the main task's stack comes out of,
# after ESP-IDF's start-up and this firmware's static initialisers (tools/heap_at_start.py,
# docs/ARCHITECTURE.md section 14). 8 KB, which is task p11-01's figure: v0.1.2 started with
# 12 796 bytes left in that block and 0.1.4 with 11 788, and v0.1.3 - the image that could not
# create its main task at all - had no block for it. 8 192 is under both builds that run and far
# above the arrangement that failed, so a build that drops below it has lost a third of the room
# two releases had and is a finding before it is an image.
HEAP_MIN_SPARE=8192

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

# Every rail, in one place (task p3-06): tools/check_rails.py has one entry per rail of
# tasks/phase-3/06-the-rails-and-the-bench.md, each naming the decision it guards - the greps that
# used to sit in this file (pin ownership, the dual adapter's selector, core/ and the modem
# library, the TLS rail) among them - and for a rail held by a test it checks that the test is
# still there. It runs first, before anything is compiled, because a broken rail is cheapest to
# find then. `python tools/check_rails.py --list` prints the table.
step_host_tests() {
  local dir=$BUILD/host
  python tools/check_rails.py || return 1
  cmake -S host_test -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWALTER_AGENT=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
  cmake --build "$dir" &&
  ctest --test-dir "$dir" --output-on-failure --no-tests=error || return 1
  # The second configuration (task p4-05): the unit without an agent - WALTER_AGENT=0, the wake
  # cycle over the null backend, nothing of components/agent/ or the copied library compiled -
  # built in a directory of its own and run for the wake cycle's own tests and the rails' two
  # months, which is phase 3's month unchanged. The flag reaches the sources through one
  # generated header, so ccache answers for every object it does not change.
  local noagent=$BUILD/host-noagent
  echo "== the second host configuration: WALTER_AGENT=0"
  cmake -S host_test -B "$noagent" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWALTER_AGENT=OFF \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
  cmake --build "$noagent" --target core_tests &&
  ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    "$noagent/core_tests" --no-intro -sf='*/core/cycle/*,*/rails/test_rail_updates.cpp,*/rails/test_rail_secrets.cpp'
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

# Does the image start at all (task p11-02; docs/ARCHITECTURE.md section 14, rail R22)? The one
# thing every check this repository had before p11-01 missed: v0.1.3 fitted its slot, its DIRAM
# fitted its segment, every host test passed, and ESP-IDF could not create the main task, because
# no heap region had a free block of CONFIG_ESP_MAIN_TASK_STACK_SIZE bytes left once the static
# initialisers had taken theirs. tools/heap_at_start.py is the model of that moment - the regions
# from this build's map and sdkconfig, three sizes from its DWARF, the allocator and the region
# table from ESP-IDF's own sources, the start-up's allocations from a recording of this firmware
# running - and it exits 1 when the stack would not fit with --min-spare to spare.
#
# It runs over every build this script makes, before anything is copied to out/images/ or
# released, so an image that cannot start is never published. Its whole output goes to the log:
# the regions, every allocation in order, the heap block by block, the block the main task got,
# the tightest of the big allocations, and an upper bound on what the board's own `heap:` boot
# line will then say (docs/bench.md section 1 is where that one is held to a board).
check_heap_at_start() {
  local dir=$1
  echo "== the heap at start: can this image create its main task? (min spare $HEAP_MIN_SPARE)"
  if ! python tools/heap_at_start.py --build-dir "$dir" --min-spare "$HEAP_MIN_SPARE"; then
    echo "the main task's stack would not fit, or not with $HEAP_MIN_SPARE bytes to spare:"
    echo "this image would end every boot in 'assert failed: esp_startup_start_app"
    echo "app_startup.c:86 (res == pdTRUE)', or is a few hundred bytes of .data or .bss from it."
    echo "docs/ARCHITECTURE.md section 14 says what to do about it; it is static storage that"
    echo "has to come down, and the model's 'the tightest is N bytes' line is where to look."
    return 1
  fi
}

# Every idf.py of this script, with the one build directory and the three cache variables that
# must be passed every time. -DPROJECT_VER is always given, even when it is the default: it is a
# cmake cache variable, and a build that inherited one from an earlier run would say a version
# nobody asked for. sdkconfig.defaults is the configuration: when it changes, the generated
# sdkconfig is dropped so that the change takes effect (ESP-IDF only fills in options that are
# missing from sdkconfig).
idf_run() {
  local dir=$BUILD/fw
  local stamp=$dir/sdkconfig.defaults.sha256
  local want; want=$(sha256sum sdkconfig.defaults | cut -d' ' -f1)
  if [ -f "$dir/sdkconfig" ] && [ "$(cat "$stamp" 2>/dev/null)" != "$want" ]; then
    echo "sdkconfig.defaults changed: regenerating sdkconfig"
    rm -f "$dir/sdkconfig" "$dir/sdkconfig.old"
  fi
  mkdir -p "$dir" && echo "$want" > "$stamp"
  idf.py -B "$dir" -DSDKCONFIG="$dir/sdkconfig" -DPROJECT_VER="$VERSION" \
         -DTWINOAK_TRUST_TEST_KEY="$TESTKEY" "$@"
}

# The managed components, into managed_components/ (task p4-01). This runs before the host tests
# because host_test/ compiles `agent-core` against the ArduinoJson **the board gets** - the same
# bytes, not a second copy - and the component manager is what puts them there. `reconfigure` is
# the resolution plus a cmake configure of the build directory the build step then uses, so
# nothing is configured twice.
#
# Resolution needs the network on a cold run, as the pip install of spec-tools does; afterwards
# IDF_COMPONENT_CACHE_PATH (/ccache/components) answers. Nothing here reaches for the agent core
# itself: that is a checked copy under components/agent-core/, verified by
# tools/sync-agent-core.ps1 -Verify before this script is started at all (docs/upstream.md).
step_deps() {
  idf_run reconfigure || return 1
  local json=managed_components/bblanchon__arduinojson/src/ArduinoJson.h
  if [ ! -f "$json" ]; then echo "the component manager did not produce $json"; return 1; fi
  echo "managed components: $(ls managed_components | tr '\n' ' ')"
  echo "--- dependencies.lock"
  cat dependencies.lock
}

step_build() {
  local dir=$BUILD/fw
  local t0; t0=$(date +%s)
  idf_run build || return 1
  echo "idf.py build: $(( $(date +%s) - t0 )) s"
  # The factory image: bootloader, table, otadata and application as one file from 0x0 (what a
  # board out of the box is flashed with; the release publishes it as -factory.bin).
  idf_run merge-bin -o "$PROJECT_NAME-factory.bin" || return 1
  idf_run size || return 1
  # The rail before the images: an image that cannot start is not copied anywhere and is not
  # released (task p11-02).
  check_heap_at_start "$dir" || return 1
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
  # The modem library was compiled, and this firmware's own uplink over it is in the link: the
  # thin thing that calls it (task p3-01, components/modem/walter_uplink.cpp) and the policy that
  # drives it (core/uplink). Task p0-01's anchor object, which existed only because nothing
  # called the library, is gone with them.
  local lib=$dir/esp-idf/dptechnics__walter-modem/libdptechnics__walter-modem.a
  if [ ! -f "$lib" ]; then echo "walter-modem was not built: $lib missing"; return 1; fi
  echo "walter-modem: $(ar t "$lib" | tr '\n' ' ')"
  # The map names an object by its archive and its base name - `libcore.a(policy.cpp.obj)` - and
  # not by the path it was compiled from, so these are the two archives' names for them.
  grep -q "libmodem.a(walter_uplink.cpp.obj" "$dir/$PROJECT_NAME.map" || {
    echo "components/modem's uplink is not in the link (walter_uplink.cpp.obj not in the map)"
    return 1
  }
  grep -q "libcore.a(policy.cpp.obj" "$dir/$PROJECT_NAME.map" || {
    echo "core/uplink's policy is not in the link (policy.cpp.obj not in the map)"
    return 1
  }
  # The HTTPS client (task p3-02): the TLS socket over the library and the client, the download
  # and the fetch in core/ are called from the console, so --gc-sections keeps them; this is the
  # check that they are there.
  for obj in "libmodem.a(walter_tls.cpp.obj" "libcore.a(client.cpp.obj" "libcore.a(download.cpp.obj" \
             "libcore.a(fetch.cpp.obj" "libcore.a(http1.cpp.obj" "libcore.a(trust.cpp.obj"; do
    grep -q "$obj" "$dir/$PROJECT_NAME.map" || {
      echo "the HTTPS client is not in the link ($obj not in the map)"
      return 1
    }
  done
  echo "the HTTPS client is in the link: walter_tls, http1, trust, client, download, fetch"
  echo "walter-modem is in the link ($(grep -c 'dptechnics__walter-modem' "$dir/$PROJECT_NAME.map") references in the map), called from components/modem/walter_uplink.cpp"
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
  # time on a tag (S-10: local first). At a release's own version the signature is the workflow's
  # and everything else still runs here; the branch below says why.
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
  # Whether this run's version is a release's own - X.Y.Z+<sha7>, which is what the release
  # workflow's build job passes (-Version) and what a person rehearsing a tag types. The shape is
  # read from the tooling, so there is one definition of it (task p7-01).
  local is_release=1
  python -c 'import sys; sys.path.insert(0, "tools/release"); from build_release import RELEASE_VERSION; sys.exit(0 if RELEASE_VERSION.fullmatch(sys.argv[1]) else 1)' \
         "$VERSION" || is_release=0
  if [ "$is_release" = 1 ]; then
    # The one part of the path a laptop cannot walk: the release key is the RELEASE_SIGNING_KEY
    # secret of the release workflow and is nowhere else (S-8), and the specification's test key
    # may not sign a release version - sign_manifest.py refuses it, and that refusal is a rail,
    # not an inconvenience. So at a release's own version this step is the files and every refusal
    # over them; the signature and the §5 gate are the publish job's, with the secret, immediately
    # before `gh release create` (.github/workflows/release.yml). At every other version - the
    # 0.0.0-dev of a plain run and the 0.0.0-dryrun of a rehearsal - the whole path runs below.
    echo "$VERSION is a release's own version: the files and their refusals are checked here, and"
    echo "the signature and verify_release.py are the release workflow's, with the secret"
    if python tools/release/sign_manifest.py --test-key "$RELEASE/manifest.json"; then
      echo "sign_manifest.py signed a release version with the specification's test key"; return 1
    fi
    echo "ok    sign_manifest.py refused the test key for $VERSION, as it must"
  else
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
  fi
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
  if [ -f "$RELEASE/manifest.json.sig" ]; then
    echo "--- manifest.json.sig"
    cat "$RELEASE/manifest.json.sig"
  else
    echo "--- manifest.json.sig: none here; a release is signed in the workflow, with the secret"
  fi
  echo "--- SHA256SUMS"
  cat "$RELEASE/SHA256SUMS"
  echo "--- NOTES.md"
  cat "$RELEASE/NOTES.md"
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
    deps)        run_step deps step_deps ;;
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
