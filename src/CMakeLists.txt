borophene_add_library(borophene_common common/error.cpp)
add_library(borophene::common ALIAS borophene_common)

borophene_add_library(
  borophene_data
  data/column_vector.cpp
  data/data_chunk.cpp
  data/logical_type.cpp
  data/schema.cpp
)
target_link_libraries(borophene_data PUBLIC borophene::common)
add_library(borophene::data ALIAS borophene_data)

borophene_add_library(
  borophene_io
  io/csv_reader.cpp
  io/csv_writer.cpp
  io/file.cpp
)
target_link_libraries(borophene_io PUBLIC borophene::common)
add_library(borophene::io ALIAS borophene_io)

borophene_add_library(borophene_execution execution/pipeline.cpp)
target_link_libraries(borophene_execution PUBLIC borophene::data)
add_library(borophene::execution ALIAS borophene_execution)

borophene_add_library(
  borophene_storage
  storage/column_codec.cpp
  storage/columnar_format.cpp
  storage/columnar_reader.cpp
  storage/columnar_writer.cpp
)
target_link_libraries(borophene_storage PUBLIC borophene::data borophene::execution borophene::io)
add_library(borophene::storage ALIAS borophene_storage)

add_library(borophene INTERFACE)
target_link_libraries(
  borophene
  INTERFACE
    borophene::common
    borophene::data
    borophene::execution
    borophene::io
    borophene::storage
)
add_library(borophene::borophene ALIAS borophene)
