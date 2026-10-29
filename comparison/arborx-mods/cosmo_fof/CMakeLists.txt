add_executable(ArborX_CosmoFoF.exe cosmo_fof.cpp)
target_include_directories(ArborX_CosmoFoF.exe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${COSMOIO_INCLUDE_DIR})
target_link_libraries(ArborX_CosmoFoF.exe ArborX::ArborX)

# HACC-style arm: ArborX's SINGLE-RANK dbscan kernel plus our own regular
# brick decomposition, overload halo and distributed stitch. See the header
# comment of cosmo_fof_hacc.cpp for why the distributed wrapper is not used.
add_executable(ArborX_CosmoFoF_HACC.exe cosmo_fof_hacc.cpp)
target_include_directories(ArborX_CosmoFoF_HACC.exe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${COSMOIO_INCLUDE_DIR})
target_link_libraries(ArborX_CosmoFoF_HACC.exe ArborX::ArborX)
