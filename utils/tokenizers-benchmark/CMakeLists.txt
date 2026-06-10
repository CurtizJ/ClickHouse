clickhouse_add_executable (tokenizers-benchmark tokenizers_benchmark.cpp)

# Tokenizer implementations (ITokenizer.cpp, sparseGramsImpl, ...) are compiled into `dbms`.
# `forEachToken` for SplitByNonAlpha is header-inline and gets the build's SSE baseline here.
target_link_libraries (tokenizers-benchmark PRIVATE dbms clickhouse_common_io)
