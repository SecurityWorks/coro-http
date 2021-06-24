set(EMBED_RESOURCE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(embed_resource)
    include(embed_resource_header)
    cmake_parse_arguments(EMBED "" "NAMESPACE;OUTPUT;TARGET" "INPUT" ${ARGN})
    add_custom_command(
        OUTPUT ${EMBED_OUTPUT}.cc
        COMMAND
            ${CMAKE_COMMAND}
                "-DNAMESPACE=${EMBED_NAMESPACE}"
                "-DOUTPUT=${EMBED_OUTPUT}"
                "-DASSETS=${EMBED_INPUT}"
                "-DCMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}"
                -P "${EMBED_RESOURCE_DIR}/embed_resource_impl.cmake"
        DEPENDS
            ${EMBED_INPUT}
        WORKING_DIRECTORY
            ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
    )
    embed_resource_header(
        OUTPUT ${EMBED_OUTPUT}.h
        NAMESPACE ${EMBED_NAMESPACE}
        INPUT ${EMBED_INPUT}
    )
    target_sources(${EMBED_TARGET} PRIVATE
        ${EMBED_OUTPUT}.h
        ${EMBED_OUTPUT}.cc
    )
endfunction()
