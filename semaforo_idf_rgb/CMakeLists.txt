cmake_minimum_required(VERSION 3.16)

# Reaproveita os componentes do firmware oficial (semaforo_idf) sem copiar
# codigo: so a variante RGB (components/semaforo_rgb) e o main.cpp sao daqui.
#
# Por padrao espera a pasta oficial AO LADO desta, com o nome "semaforo_idf"
# (mesmo layout do repo). Se sua pasta tem outro nome/lugar, mude a linha
# abaixo ou defina a variavel de ambiente SEMAFORO_IDF_DIR.
set(SEMAFORO_IDF_DIR "${CMAKE_CURRENT_LIST_DIR}/../semaforo_idf")
if(DEFINED ENV{SEMAFORO_IDF_DIR})
    set(SEMAFORO_IDF_DIR "$ENV{SEMAFORO_IDF_DIR}")
endif()

if(NOT EXISTS "${SEMAFORO_IDF_DIR}/components/semaforo")
    message(FATAL_ERROR
        "Nao achei os componentes do firmware oficial em:\n"
        "  ${SEMAFORO_IDF_DIR}/components/semaforo\n"
        "Coloque a pasta semaforo_idf ao lado de semaforo_idf_rgb, ou ajuste "
        "SEMAFORO_IDF_DIR (aqui no CMakeLists.txt ou como variavel de ambiente).")
endif()

set(EXTRA_COMPONENT_DIRS
    "${SEMAFORO_IDF_DIR}/components/semaforo"
    "${SEMAFORO_IDF_DIR}/components/can_bus"
    "${SEMAFORO_IDF_DIR}/components/protocolo"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF 6.x marca o driver TWAI legado (driver/twai.h) como deprecated com
# #warning, e o projeto compila com -Werror. Mantem como aviso, nao erro.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)

project(semaforo_idf_rgb)

# espressif/led_strip 2.5.x usa heap_caps_calloc/MALLOC_CAP_* em
# led_strip_spi_dev.c sem incluir esp_heap_caps.h; no ESP-IDF 6.x esse header
# nao vem mais "de carona" e a compilacao quebra. Forca o include so nesse
# componente (workaround; remover quando o led_strip corrigir).
idf_component_get_property(_led_strip_lib espressif__led_strip COMPONENT_LIB)
target_compile_options(${_led_strip_lib} PRIVATE -include esp_heap_caps.h)
