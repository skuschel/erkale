add_executable(erkale_casida main.cpp casida.cpp casida_grid.cpp)
set_target_properties(erkale_casida PROPERTIES OUTPUT_NAME "erkale_casida${SUFFIX}")

target_link_libraries(erkale_casida liberkale)
target_link_libraries(erkale_casida liberkale_xrs)
target_link_libraries(erkale_casida liberkale_emd)
# Link libraries
if(BUILD_SHARED_LIBS)
else()
 if(UNIX AND NOT APPLE)
  target_link_libraries(erkale_casida -lrt)
 endif()
 target_link_libraries(erkale_casida ${GSL_LIBRARIES})
 target_link_libraries(erkale_casida ${LAPACK_LIBRARIES})
 target_link_libraries(erkale_casida ${LIBXC_LIBRARIES})
 target_link_libraries(erkale_casida ${LIBINT_LIBRARIES})
 target_link_libraries(erkale_casida ${HDF5_LIBRARIES})
endif()

install (TARGETS erkale_casida DESTINATION bin)
# Install headers
file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
install(FILES ${headers} DESTINATION include/erkale/casida)
