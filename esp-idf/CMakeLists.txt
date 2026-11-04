include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
file(GLOB PARTYLINE_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
idf_component_register(
    SRCS
        ${PARTYLINE_SRCS}
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        ${SPANGAP_REQUIRES}
        json
        esp_timer
)
