cmake_minimum_required(VERSION 3.16)
project(AIDepthMap CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GLEW REQUIRED)

# ── FFGL SDK ──────────────────────────────────────────────────────────────────
set(FFGL_DIR "${CMAKE_SOURCE_DIR}/../ffgl" CACHE PATH "FFGL SDK root")
set(FFGL_LIB "${FFGL_DIR}/source/lib")
file(GLOB_RECURSE FFGL_SRCS
    "${FFGL_LIB}/ffgl/*.cpp"
    "${FFGL_LIB}/ffglex/*.cpp"
)

# ── ONNX Runtime ─────────────────────────────────────────────────────────────
set(ORT_DIR  "${CMAKE_SOURCE_DIR}/onnxruntime" CACHE PATH "ONNX Runtime root")
set(ORT_INC  "${ORT_DIR}/include")
set(ORT_LIBD "${ORT_DIR}/lib")
set(ORT_BIND "${ORT_DIR}/lib")

# ── Plugin DLL ───────────────────────────────────────────────────────────────
add_library(AIDepthMap SHARED
    Plugin.cpp
    Plugin.h
    ${FFGL_SRCS}
)

target_include_directories(AIDepthMap PRIVATE
    ${FFGL_LIB}
    ${FFGL_LIB}/ffgl
    ${FFGL_LIB}/ffglex
    ${ORT_INC}
)

target_link_libraries(AIDepthMap PRIVATE
    GLEW::GLEW
    opengl32
    "${ORT_LIBD}/onnxruntime.lib"
)

set_target_properties(AIDepthMap PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# Enable DirectML execution provider if the header is present
if(EXISTS "${ORT_INC}/dml_provider_factory.h")
    message(STATUS "DirectML header found – enabling GPU inference")
    target_compile_definitions(AIDepthMap PRIVATE ORT_DML_AVAILABLE=1)
else()
    message(STATUS "DirectML header not found – CPU inference only")
    target_compile_definitions(AIDepthMap PRIVATE ORT_DML_AVAILABLE=0)
endif()

# Copy ONNX Runtime DLL(s) to output folder
add_custom_command(TARGET AIDepthMap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORT_BIND}/onnxruntime.dll"
        "$<TARGET_FILE_DIR:AIDepthMap>"
    COMMENT "Staging onnxruntime.dll"
)
if(EXISTS "${ORT_BIND}/DirectML.dll")
    add_custom_command(TARGET AIDepthMap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_BIND}/DirectML.dll"
            "$<TARGET_FILE_DIR:AIDepthMap>"
        COMMENT "Staging DirectML.dll"
    )
endif()
