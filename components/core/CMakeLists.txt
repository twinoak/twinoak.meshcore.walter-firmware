# core/ - everything that is logic. No ESP-IDF include, no FreeRTOS, no heap after start-up
# (docs/ARCHITECTURE.md §1). What it needs of the world it takes as an interface from interfaces/
# (the architecture's hal/). The same sources are compiled on the host by host_test/.
idf_component_register(
  SRCS "version/version.cpp"
       "image/this_image.cpp"
       "board/radio_pins.cpp"
       "board/slot.cpp"
       # The dual adapter's selector (task p1-06): compiled here so that the build of the unit
       # that will never use it still checks it, and linked out of the image because nothing
       # constructs it (main/main.cpp builds a SingleTarget).
       "board/dual_target.cpp"
       "health/health.cpp"
       # The `coredump` partition, read (task p11-03): the flash header, the ELF, its notes and
       # the TCBs' names, so that a rolled-back trial can say what killed it.
       "health/coredump.cpp"
       "console/parser.cpp"
       "console/commands.cpp"
       "console/console.cpp"
       "console/log_ring.cpp"
       # The terminal's LF -> CR LF, in one place (task p8-02): the platform's console and the
       # console host of host_test/ both convert through it, so a test sees a board's bytes.
       "console/crlf.cpp"
       # What a trace line may not carry out (task p10-06): the token's and the PIN's two places
       # in what goes to the modem, and every occurrence of either, taken out before a line is
       # printed. The hook itself is components/modem/'s - core/ may not name esp_log.
       "trace/redact.cpp"
       "link/codec/cobs.cpp"
       "link/codec/packet.cpp"
       "link/codec/frame.cpp"
       "link/codec/messages.cpp"
       "link/client/port.cpp"
       "link/client/client.cpp"
       "link/client/words.cpp"
       # The radio's identity in three states, in one place (task p10-03): known, not made
       # yet, and lost - the refusal repeater v0.1.2 answers IDENTITY with.
       "link/radio_identity.cpp"
       "swd/wire.cpp"
       "swd/dp.cpp"
       "swd/mem_ap.cpp"
       "nrf52/nrf52.cpp"
       # The one door (task p5-02, S-5a): the CTRL-AP's erase and soft reset, the UICR and the
       # base's flash behind a grant only a typed console command can make. The only file but
       # regs.h that may name the forbidden registers (rail R3); included by core/provision/
       # alone (rail R3b).
       "nrf52/provision_door.cpp"
       # The sequence (task p5-03, S-5a): `rak provision`, the door's one caller (rail R3b),
       # and the variant table it acts on.
       "provision/variant.cpp"
       "provision/provision.cpp"
       "image/crypto/sha256.cpp"
       "flasher/board_profile.cpp"
       "flasher/bootloader_settings.cpp"
       "flasher/flasher.cpp"
       # Manifests and signatures (task p2-01): the vendored TweetNaCl verification extract (C),
       # SHA-512 and Ed25519 over it, the trust table, the signature line, the manifest, the
       # choice of image, the file's checks and the verdict.
       "third_party/tweetnacl/tweetnacl_verify.c"
       "image/crypto/sha512.cpp"
       "image/crypto/ed25519.cpp"
       "image/trust.cpp"
       "image/signature.cpp"
       "image/manifest.cpp"
       "image/choose.cpp"
       "image/file_check.cpp"
       # The board's base (task p5-01): FIRMWARE-MANIFEST.md section 4a over a `provision` file,
       # which check 5p of the verdict applies.
       "image/base.cpp"
       "image/verdict.cpp"
       # The image store (task p2-02): the records in the rakfw partition, and the framed
       # transfer that loads one over USB.
       "image/store.cpp"
       "image/receive.cpp"
       # The radio's update (task p2-03): the journal, the battery over the link, the machine.
       "update/journal.cpp"
       "update/link_battery.cpp"
       "update/radio_update.cpp"
       # The Walter's own update (task p2-04): the other slot as a transfer target, the trial.
       "update/self_update.cpp"
       # The supervisor (task p2-05): the radio's heartbeat and the ladder, and the battery's
       # last good reading that a rung reached over a dead link still has.
       "update/last_good_battery.cpp"
       "supervisor/supervisor.cpp"
       # The uplink (task p3-01): the failure taxonomy and its backoff, the 3GPP power-saving
       # timers, and the policy that is the whole sequence of getting onto the network.
       "uplink/failure.cpp"
       "uplink/psm.cpp"
       "uplink/policy.cpp"
       # The HTTPS client (task p3-02): the URL, HTTP/1.1 as bytes, the transport over a TLS
       # socket, the roots and their loader, the client, the download in ranges; and the fetch of
       # a release and the self-update trial's TLS handshake over them.
       "net/url.cpp"
       "net/http1.cpp"
       "net/http_over_tls.cpp"
       # Which socket a connection uses and whose events it believes (task p8-03): the rule the
       # second HTTPS request of a wake failed on, with no modem in it.
       "net/socket_rotation.cpp"
       "net/trust.cpp"
       "net/client.cpp"
       "net/download.cpp"
       "update/fetch.cpp"
       "update/tls_check.cpp"
       # The settings (task p3-03): the table of keys and the store over interfaces/key_value.h.
       "settings/keys.cpp"
       "settings/settings.cpp"
       # The duty cycle (task p3-04): what the battery allows and how long until the next
       # check-in; what a wake means; the bound on a wake; and the one way out of one.
       "power/interval_policy.cpp"
       "cycle/wake_reason.cpp"
       "cycle/wake_guard.cpp"
       "cycle/sleep.cpp"
       "cycle/duty_cycle.cpp"
       # The wake cycle (task p3-05): the time of day once the network has given one, the
       # readings kept until somebody takes them, and the one pass over everything above.
       "cycle/network_clock.cpp"
       "cycle/telemetry_ring.cpp"
       "cycle/wake_cycle.cpp"
       # The Walter's own attention (task p4-04): the agent's 401 and a restart the library asked
       # for and did not get - a byte of the duty cycle's record, a letter in the wake record.
       "cycle/agent_attention.cpp"
  INCLUDE_DIRS "include"
  REQUIRES interfaces)

include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/ours.cmake)
twinoak_ours(${COMPONENT_LIB})

# The product version reaches core/ as a string literal (include/core/version.h).
idf_build_get_property(_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TWINOAK_VERSION="${_ver}")

# The vendored third-party code (third_party/*/README.md: origin, version, licence, hash). A
# system include path, so that a header's warnings are its own, as doctest's are in host_test/;
# the one .c file is held to our flags except -Wno-sign-compare, which its loops need
# (third_party/tweetnacl/README.md).
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/third_party/tweetnacl/tweetnacl_verify.c
  PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")

# The trust table (task p2-01): keys/release-keys.txt -> trust_keys.h, generated into the build
# directory by tools/gen_trust_table.py and included by image/trust.cpp alone. With
# -DTWINOAK_TRUST_TEST_KEY=1 the specification's public test key is appended, read from the
# fixture index and never written down a second time, and core/ is told so.
idf_build_get_property(_python PYTHON)
set(_repo ${CMAKE_CURRENT_LIST_DIR}/../..)
set(_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(_trust_header ${_generated}/trust_keys.h)
set(_trust_command ${_python} ${_repo}/tools/gen_trust_table.py ${_repo}/keys/release-keys.txt ${_trust_header})
set(_trust_depends ${_repo}/tools/gen_trust_table.py ${_repo}/keys/release-keys.txt
                   ${_repo}/tools/release/verify_release.py ${_repo}/spec/tools/manifestref.py)
if(TWINOAK_TRUST_TEST_KEY)
  list(APPEND _trust_command --test-key ${_repo}/spec/fixtures/manifest/index.json)
  list(APPEND _trust_depends ${_repo}/spec/fixtures/manifest/index.json)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE TWINOAK_TRUST_TEST_KEY=1)
endif()
add_custom_command(
  OUTPUT ${_trust_header}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_generated}
  COMMAND ${_trust_command}
  DEPENDS ${_trust_depends}
  COMMENT "the trust table: keys/release-keys.txt -> trust_keys.h"
  VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${_trust_header})
target_include_directories(${COMPONENT_LIB} PRIVATE ${_generated})

# The TLS roots (task p3-02): keys/tls/*.pem -> tls_roots.h, generated into the build directory
# by tools/gen_tls_roots.py - which holds every PEM's header (subject, fingerprint, expiry) to the
# certificate itself and refuses to write the table otherwise - and included by net/trust.cpp
# alone.
set(_tls_header ${_generated}/tls_roots.h)
file(GLOB _tls_pems ${_repo}/keys/tls/*.pem)
add_custom_command(
  OUTPUT ${_tls_header}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_generated}
  COMMAND ${_python} ${_repo}/tools/gen_tls_roots.py ${_repo}/keys/tls ${_tls_header}
  DEPENDS ${_repo}/tools/gen_tls_roots.py ${_tls_pems}
  COMMENT "the TLS roots: keys/tls/*.pem -> tls_roots.h"
  VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${_tls_header})

# Before core/ compiles, every one of its files is read for an ESP-IDF include
# (tools/cmake/no_idf_include.cmake); one is a failed build with a sentence.
add_custom_target(core_no_idf_include
  COMMAND ${CMAKE_COMMAND} -DCORE_DIR=${CMAKE_CURRENT_LIST_DIR} -P
          ${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/no_idf_include.cmake
  COMMENT "core has no ESP-IDF include"
  VERBATIM)
add_dependencies(${COMPONENT_LIB} core_no_idf_include)
