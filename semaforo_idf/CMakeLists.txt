cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ESP-IDF 6.x marca o driver TWAI legado (driver/twai.h) como deprecated com
# #warning, e o projeto compila com -Werror. Mantem como aviso, nao erro.
# (Migrar pra esp_twai.h e uma tarefa separada.)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)

project(semaforo_idf)
