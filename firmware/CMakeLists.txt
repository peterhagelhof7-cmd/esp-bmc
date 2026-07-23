cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# wolfssl/wolfssl setzt WOLFSSL_USER_SETTINGS zwar in seiner eigenen
# CMakeLists.txt, das propagiert aber nicht in die separate
# wolfssl/wolfssh-Komponente (eigenes Component-Target, eigene
# Compile-Flags) - deshalb hier projektweit vor project() gesetzt, damit
# JEDE Komponente (auch wolfssh) denselben Header-Zweig
# (wolfssl/wolfcrypt/settings.h statt des autoconf-generierten
# wolfssl/options.h) sieht. Das mitgelieferte
# managed_components/wolfssl__wolfssl/include/user_settings.h deaktiviert
# RSA bereits standardmaessig (WOLFSSH_NO_RSA/NO_RSA) - passt zu unserem
# Plan, den SSH-Host-Key nur mit ECC zu erzeugen, siehe
# docs/entscheidungen.md "SSH-Server (P7)".
#
# NO_RSA zusaetzlich noetig (nicht nur WOLFSSH_NO_RSA, das das Template
# bereits selbst setzt): ohne NO_RSA bleibt wolfCrypts RSA-Primitive
# aktiv und ein reines #warning ("RSA may be difficult with less than
# 10KB Stack") wird durch das projektweite -Werror zum harten
# Build-Fehler - unabhaengig davon, dass wolfSSH RSA gar nicht nutzt.
# NO_ESP32_CRYPT: die ESP32-S3-Hardwarebeschleunigungs-Ports von wolfSSL
# (esp32_aes.c/esp32_sha.c) referenzieren ESP-IDF-interne Symbole
# (PERIPH_AES_MODULE, hal/clk_gate_ll.h), die in unserer ESP-IDF-Version
# (6.0.1) umbenannt/entfernt wurden - reines Software-Crypto ist fuer
# eine von Hand bedienten SSH-Konsole (kein Hochdurchsatz-TLS) voellig
# ausreichend, daher komplett deaktiviert statt den Espressif-HW-Port zu
# patchen. NO_ESP32_CRYPT alleine reicht nicht - manche Stellen (z.B.
# wolfssl/openssl/sha.h) pruefen stattdessen nur die feingranularen
# Einzel-Flags, deshalb zusaetzlich explizit gesetzt.
#
# Ed25519-Aktivierung: HAVE_ED25519 ist im user_settings.h-Template bereits an
# (ESP32-S3), aber die vier Sub-Features (SIGN/VERIFY/KEY_IMPORT/KEY_EXPORT)
# werden dort nicht abgeleitet - ohne KEY_IMPORT+KEY_EXPORT deaktiviert wolfSSH
# ed25519 komplett (internal.h -> WOLFSSH_NO_ED25519), weshalb der Server
# bislang nur ECDSA anbot. Sie werden direkt in user_settings.h neben
# HAVE_ED25519 ergaenzt (idempotenter Patch nach project() unten) statt per
# -D-Flag: so sehen wolfSSL UND wolfSSH sie garantiert im selben
# Config-Kontext ueber die ohnehin eingebundene user_settings.h. Siehe
# docs/entscheidungen.md.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DWOLFSSL_USER_SETTINGS -DNO_RSA -DNO_ESP32_CRYPT \
-DNO_WOLFSSL_ESP32_CRYPT_HASH -DNO_WOLFSSL_ESP32_CRYPT_AES -DNO_WOLFSSL_ESP32_CRYPT_RSA_PRI")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DWOLFSSL_USER_SETTINGS -DNO_RSA -DNO_ESP32_CRYPT \
-DNO_WOLFSSL_ESP32_CRYPT_HASH -DNO_WOLFSSL_ESP32_CRYPT_AES -DNO_WOLFSSL_ESP32_CRYPT_RSA_PRI")

project(esp-bmc)

# wolfssl/wolfssl liefert wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c
# mit einem Enum-Member namens "thread_local" aus - unter unserem
# Toolchain (GCC 15.2.0, C23-Modus) ist das ein echtes reserviertes
# Schluesselwort, kein Makro (ein "#undef thread_local" wirkt deshalb
# NICHT, das wurde ausprobiert). Reine Diagnose-/Speicher-
# Introspektionsdatei, fuer SSH-Funktionalitaet irrelevant - siehe
# docs/entscheidungen.md "SSH-Server (P7)".
#
# Der ESP-IDF Component-Manager stellt managed_components bei jeder
# Neukonfiguration aus seinem Cache wieder her und wuerfe einen
# einmaligen Hand-Patch damit wieder um - deshalb hier als idempotenter
# CMake-Schritt bei JEDER Konfiguration erneut angewendet (laeuft nach
# project(), also nachdem die Komponente sicher heruntergeladen ist,
# aber vor dem eigentlichen Ninja-Compile-Schritt).
set(WOLFSSL_MEM_LIB_FILE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/wolfssl__wolfssl/wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c")
if(EXISTS "${WOLFSSL_MEM_LIB_FILE}")
    file(READ "${WOLFSSL_MEM_LIB_FILE}" WOLFSSL_MEM_LIB_CONTENT)
    string(FIND "${WOLFSSL_MEM_LIB_CONTENT}" "wc_thread_local" ALREADY_PATCHED)
    if(ALREADY_PATCHED EQUAL -1)
        string(REPLACE "    thread_local," "    wc_thread_local," WOLFSSL_MEM_LIB_CONTENT "${WOLFSSL_MEM_LIB_CONTENT}")
        string(REPLACE "sdk_log_meminfo(thread_local," "sdk_log_meminfo(wc_thread_local,"
               WOLFSSL_MEM_LIB_CONTENT "${WOLFSSL_MEM_LIB_CONTENT}")
        file(WRITE "${WOLFSSL_MEM_LIB_FILE}" "${WOLFSSL_MEM_LIB_CONTENT}")
        message(STATUS "esp-bmc: wolfssl esp_sdk_mem_lib.c thread_local-Patch angewendet")
    endif()
endif()

# espressif/led_strip 2.5.5 nutzt in src/led_strip_spi_dev.c
# MALLOC_CAP_DEFAULT/MALLOC_CAP_INTERNAL/MALLOC_CAP_DMA/heap_caps_calloc,
# ohne esp_heap_caps.h einzubinden - baut nicht gegen unsere Toolchain
# (GCC 15.2.0), obwohl die eigene CMakeLists.txt der Komponente kein
# Kconfig-Flag zum Abschalten des SPI-Backends anbietet (wir nutzen
# ohnehin nur RMT fuer WS2812, GPIO48 - siehe watchdog_manager). Echter
# Bibliotheksfehler, kein Konfigurationsproblem unsererseits - siehe
# docs/entscheidungen.md "Watchdog-LED (RGB, GPIO48)". Gleiches
# Patch-Muster wie beim wolfssl-thread_local-Fix oben: idempotenter
# CMake-Schritt statt Hand-Patch, da der Component-Manager
# managed_components bei jeder Neukonfiguration aus dem Cache
# wiederherstellt.
set(LED_STRIP_SPI_DEV_FILE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__led_strip/src/led_strip_spi_dev.c")
if(EXISTS "${LED_STRIP_SPI_DEV_FILE}")
    file(READ "${LED_STRIP_SPI_DEV_FILE}" LED_STRIP_SPI_DEV_CONTENT)
    string(FIND "${LED_STRIP_SPI_DEV_CONTENT}" "esp_heap_caps.h" ALREADY_PATCHED)
    if(ALREADY_PATCHED EQUAL -1)
        string(REPLACE "#include \"esp_log.h\"" "#include \"esp_log.h\"\n#include \"esp_heap_caps.h\""
               LED_STRIP_SPI_DEV_CONTENT "${LED_STRIP_SPI_DEV_CONTENT}")
        file(WRITE "${LED_STRIP_SPI_DEV_FILE}" "${LED_STRIP_SPI_DEV_CONTENT}")
        message(STATUS "esp-bmc: led_strip led_strip_spi_dev.c esp_heap_caps.h-Patch angewendet")
    endif()
endif()

# Ed25519-Pubkey-Auth in wolfSSH aktivieren. wolfSSL setzt bei HAVE_ED25519
# alle noetigen Sub-Features (SIGN/VERIFY/KEY_IMPORT/KEY_EXPORT) automatisch
# (settings.h), ABER wolfSSHs Gate (internal.h) verlangt zusaetzlich
# WOLFSSL_ED25519_STREAMING_VERIFY - ein separates Opt-in, das per Default aus
# ist; ohne es setzt wolfSSH intern WOLFSSH_NO_ED25519 und bietet nur ECDSA in
# server-sig-algs an (ed25519-Client-Keys werden abgelehnt). Genau dieses eine
# Flag hier neben dem vorhandenen "#define HAVE_ED25519" in user_settings.h
# ergaenzen, damit wolfSSL UND wolfSSH es konsistent im selben Config-Kontext
# sehen (settings.h leitet daraus auch WOLFSSL_ED25519_PERSISTENT_SHA ab). Das
# Flag wird nirgends sonst definiert -> keine Redefinition. Idempotentes
# Muster wie die Patches oben. Siehe docs/entscheidungen.md.
set(WOLFSSL_USER_SETTINGS_FILE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/wolfssl__wolfssl/include/user_settings.h")
if(EXISTS "${WOLFSSL_USER_SETTINGS_FILE}")
    file(READ "${WOLFSSL_USER_SETTINGS_FILE}" WOLFSSL_US_CONTENT)
    string(FIND "${WOLFSSL_US_CONTENT}" "WOLFSSL_ED25519_STREAMING_VERIFY" ALREADY_PATCHED)
    if(ALREADY_PATCHED EQUAL -1)
        string(REPLACE
            "    /* ED25519 requires SHA512 */\n    #define HAVE_ED25519\n"
            "    /* ED25519 requires SHA512 */\n    #define HAVE_ED25519\n    /* esp-bmc: von wolfSSH fuer ed25519-Pubkey-Auth verlangt (internal.h-Gate) */\n    #define WOLFSSL_ED25519_STREAMING_VERIFY\n"
            WOLFSSL_US_CONTENT "${WOLFSSL_US_CONTENT}")
        file(WRITE "${WOLFSSL_USER_SETTINGS_FILE}" "${WOLFSSL_US_CONTENT}")
        message(STATUS "esp-bmc: wolfssl user_settings.h WOLFSSL_ED25519_STREAMING_VERIFY-Patch angewendet")
    endif()
endif()

# droscy/esp_wireguard nutzt unter mbedTLS 4.x (IDF 6.0.1) in
# src/wireguard-platform.c den PSA-Crypto-Pfad: wireguard_platform_init()
# ruft psa_crypto_init() auf und zieht Zufallsbytes ueber
# psa_generate_random(). Genau dieses psa_crypto_init() stuerzt auf echter
# Hardware reproduzierbar mit LoadProhibited ab (mbedTLS-4.x/tf-psa-crypto
# <-> esp_wireguard-Fork, siehe docs/entscheidungen.md). WireGuards
# eigentliche Krypto (BLAKE2s/ChaCha20Poly1305 via crypto/refc/, X25519 via
# libsodium) nutzt PSA gar nicht - PSA wird dort ausschliesslich fuer
# Zufallsbytes gebraucht. Deshalb denselben Weg gehen wie die ESP8266-/
# LibreTiny-Zweige derselben Datei: den ESP32-Hardware-RNG esp_fill_random()
# nutzen (deklariert via esp_system.h, im ESP-IDF-Zweig der Datei bereits
# eingebunden) und psa_crypto_init() ganz weglassen. Zwei chirurgische
# Ein-Zeilen-Ersetzungen. Gleiches idempotente Muster wie die wolfssl-/
# led_strip-Patches oben - PlatformIO holt lib_deps bei Bedarf neu und
# wuerfe einen Hand-Patch damit um. Ueber alle Umgebungen per GLOB, da
# .pio/libdeps/<env>/ pro PlatformIO-Umgebung getrennt existiert.
file(GLOB WG_PLATFORM_FILES
     "${CMAKE_CURRENT_LIST_DIR}/.pio/libdeps/*/esp_wireguard/src/wireguard-platform.c")
foreach(WG_PLATFORM_FILE ${WG_PLATFORM_FILES})
    file(READ "${WG_PLATFORM_FILE}" WG_PLATFORM_CONTENT)
    string(FIND "${WG_PLATFORM_CONTENT}" "esp-bmc-no-psa" ALREADY_PATCHED)
    if(ALREADY_PATCHED EQUAL -1)
        string(REPLACE "psa_status_t status = psa_crypto_init();"
               "psa_status_t status = PSA_SUCCESS; /* esp-bmc-no-psa: psa_crypto_init() umgangen, RNG via esp_fill_random */"
               WG_PLATFORM_CONTENT "${WG_PLATFORM_CONTENT}")
        string(REPLACE "psa_generate_random((uint8_t *) bytes, size);"
               "esp_fill_random(bytes, size); /* esp-bmc-no-psa */"
               WG_PLATFORM_CONTENT "${WG_PLATFORM_CONTENT}")
        file(WRITE "${WG_PLATFORM_FILE}" "${WG_PLATFORM_CONTENT}")
        message(STATUS "esp-bmc: esp_wireguard wireguard-platform.c PSA->esp_fill_random-Patch angewendet (${WG_PLATFORM_FILE})")
    endif()
endforeach()
