
add_executable(axe-bin
  Options.h
  Options.cpp
  main.cpp)
target_link_libraries(axe-bin axe)
set_target_properties(axe-bin PROPERTIES OUTPUT_NAME axe)

install(TARGETS axe-bin DESTINATION bin)
