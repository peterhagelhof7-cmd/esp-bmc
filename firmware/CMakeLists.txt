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
