# Production source is unchanged; enable its include for this private compile.
file(READ "${SOURCE}" shader)
string(REPLACE "#version 450 core" "#version 450 core\n#extension GL_GOOGLE_include_directive : require" shader "${shader}")
file(WRITE "${OUTPUT}.comp" "${shader}")
execute_process(COMMAND "${COMPILER}" -V "-I${INCLUDE_DIR}"
    -Dddgi_update_irradiance=main --source-entrypoint main -e ddgi_update_irradiance
    "${OUTPUT}.comp" -o "${OUTPUT}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Private actual irradiance compile failed: ${status}")
endif()
