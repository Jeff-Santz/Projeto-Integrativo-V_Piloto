cmake_minimum_required(VERSION 3.16)

# Reaproveita os componentes do firmware oficial (semaforo_idf) sem copiar
# codigo: so a variante RGB (components/semaforo_rgb) e o main.cpp sao daqui.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../semaforo_idf/components/semaforo"
    "${CMAKE_CURRENT_LIST_DIR}/../semaforo_idf/components/can_bus"
    "${CMAKE_CURRENT_LIST_DIR}/../semaforo_idf/components/protocolo"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(semaforo_idf_rgb)
