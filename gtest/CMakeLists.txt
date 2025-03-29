add_library(gtest 
  src/gtest-all.cc
)
target_compile_options(gtest PRIVATE -target aarch64-unknown-linux-gnu)
target_link_options(gtest PRIVATE -target aarch64-unknown-linux-gnu)

target_include_directories(gtest PUBLIC 
                           ".")

