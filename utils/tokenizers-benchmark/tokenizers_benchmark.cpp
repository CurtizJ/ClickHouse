/** Throughput benchmark for all ClickHouse tokenizers (see src/Interpreters/ITokenizer.h).
  *
  * For every tokenizer type known to `TokenizerFactory` it measures how fast the
  * tokenizer chews through the input text using the production hot path `forEachToken`.
  *
  * Usage:
  *     tokenizers-benchmark [--runs=N] [--limit-mb=M] [file ...]
  *
  *     --runs=N       Number of passes over each file; the best (minimal) time is reported. Default: 3.
  *     --limit-mb=M   Read at most M megabytes from the start of each file (0 = whole file). Default: 64.
  *     --tokenizer=S  Benchmark only tokenizers whose label contains substring S (useful under `perf record`).
  *     --verify-mb=M  Instead of timing, read M megabytes and compare the push path (forEachToken)
  *                    against the pull path (nextInString) token-by-token, including order.
  *     file ...       Input files. If omitted, defaults to $HOME/fineweb.csv and $HOME/logs.csv.
  *
  * The input is loaded into a single buffer that is right-padded with zero bytes, because
  * `SplitByNonAlphaTokenizer` reads 16-byte SSE chunks and assumes at least 15 bytes of
  * readable padding after the data (production `ColumnString` provides the same guarantee).
  * The buffer is split into "documents" on newline boundaries; each document is tokenized
  * separately, which mirrors how a text index tokenizes one column value per row and therefore
  * captures the per-call dispatch overhead of `forEachToken`.
  */

#include <Interpreters/ITokenizer.h>
#include <Interpreters/TokenizerFactory.h>

#include <Common/Stopwatch.h>
#include <base/types.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace DB;

/// SSE path of SplitByNonAlphaTokenizer over-reads up to 15 bytes past a token; keep a safety margin.
constexpr size_t PADDING_BYTES = 64;

/// A single document is a view into the loaded file buffer.
struct Document
{
    const char * data;
    size_t length;
};

/// Reads the whole file (or the first `limit_bytes`) into a buffer that is zero-padded on the right.
std::vector<char> readFileWithPadding(const std::string & path, size_t limit_bytes)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("Cannot open file '" + path + "': " + std::strerror(errno));

    struct stat st;
    if (::fstat(fd, &st) != 0)
    {
        ::close(fd);
        throw std::runtime_error("Cannot stat file '" + path + "': " + std::strerror(errno));
    }

    const size_t file_size = static_cast<size_t>(st.st_size);
    const size_t to_read = (limit_bytes != 0) ? std::min(limit_bytes, file_size) : file_size;

    std::vector<char> buffer(to_read + PADDING_BYTES, '\0');

    size_t offset = 0;
    constexpr size_t chunk = size_t(256) << 20; /// read() can be capped near 2 GiB, so loop in chunks.
    while (offset < to_read)
    {
        const ssize_t n = ::pread(fd, buffer.data() + offset, std::min(chunk, to_read - offset), offset);
        if (n < 0)
        {
            ::close(fd);
            throw std::runtime_error("Read error on '" + path + "': " + std::strerror(errno));
        }
        if (n == 0)
            break; /// EOF earlier than expected.
        offset += static_cast<size_t>(n);
    }
    ::close(fd);

    /// Keep exactly the bytes we read plus the zero padding (bytes [offset, offset + PADDING) are already zero).
    buffer.resize(offset + PADDING_BYTES);
    return buffer;
}

/// Splits the buffer into documents on '\n' boundaries (newlines are not part of any document).
std::vector<Document> splitIntoDocuments(const std::vector<char> & buffer)
{
    const size_t data_length = buffer.size() - PADDING_BYTES;
    const char * base = buffer.data();

    std::vector<Document> documents;
    size_t start = 0;
    for (size_t i = 0; i < data_length; ++i)
    {
        if (buffer[i] == '\n')
        {
            if (i > start)
                documents.push_back({base + start, i - start});
            start = i + 1;
        }
    }
    if (start < data_length)
        documents.push_back({base + start, data_length - start});

    return documents;
}

struct BenchmarkResult
{
    double best_seconds = std::numeric_limits<double>::max();
    size_t token_count = 0;
    size_t token_bytes = 0;
    UInt64 checksum = 0;
};

/// Runs `runs` passes over all documents, keeping the fastest. The checksum forces the
/// compiler to keep the callback body so the tokenization is not optimized away.
BenchmarkResult benchmarkTokenizer(const ITokenizer & tokenizer, const std::vector<Document> & documents, size_t runs)
{
    BenchmarkResult result;
    for (size_t run = 0; run < runs; ++run)
    {
        size_t token_count = 0;
        size_t token_bytes = 0;
        UInt64 checksum = 0;

        Stopwatch watch;
        for (const auto & document : documents)
        {
            forEachToken(tokenizer, document.data, document.length, [&](const char * token_data, size_t token_length) -> bool
            {
                ++token_count;
                token_bytes += token_length;
                /// Order-sensitive chain (FNV-style), so reordered token streams produce different values.
                checksum = (checksum * 0x100000001b3ULL) ^ token_length
                    ^ (static_cast<UInt64>(token_length ? static_cast<UInt8>(token_data[0]) : 0) << 32);
                return false; /// Never stop early: visit every token.
            });
        }
        const double seconds = watch.elapsedSeconds();

        result.best_seconds = std::min(result.best_seconds, seconds);
        result.token_count = token_count;
        result.token_bytes = token_bytes;
        result.checksum = checksum;
    }
    return result;
}

/// Tokenizes every document through both the push path (forEachToken) and the pull path
/// (the virtual nextInString) and compares the token streams exactly, including order.
/// Returns the number of mismatching documents (0 = the two interfaces agree).
size_t verifyPushAgainstPull(const ITokenizer & tokenizer, const std::vector<Document> & documents)
{
    size_t mismatched_documents = 0;
    std::vector<std::pair<size_t, size_t>> push_tokens;
    std::vector<std::pair<size_t, size_t>> pull_tokens;

    for (size_t doc_index = 0; doc_index < documents.size(); ++doc_index)
    {
        const auto & document = documents[doc_index];
        push_tokens.clear();
        pull_tokens.clear();

        forEachToken(tokenizer, document.data, document.length, [&](const char * token_data, size_t token_length) -> bool
        {
            push_tokens.emplace_back(token_data - document.data, token_length);
            return false;
        });

        size_t cur = 0;
        size_t token_start = 0;
        size_t token_length = 0;
        while (cur < document.length && tokenizer.nextInString(document.data, document.length, cur, token_start, token_length))
            pull_tokens.emplace_back(token_start, token_length);

        if (push_tokens != pull_tokens)
        {
            ++mismatched_documents;
            if (mismatched_documents <= 3)
            {
                std::fprintf(stderr, "MISMATCH in document %zu (%zu bytes): push %zu tokens, pull %zu tokens\n",
                    doc_index, document.length, push_tokens.size(), pull_tokens.size());
                for (size_t i = 0; i < std::max(push_tokens.size(), pull_tokens.size()) && i < 16; ++i)
                {
                    auto fmt = [&](const std::vector<std::pair<size_t, size_t>> & tokens) -> std::string
                    {
                        if (i >= tokens.size())
                            return "<none>";
                        return "[" + std::to_string(tokens[i].first) + "," + std::to_string(tokens[i].second) + ") '"
                            + std::string(document.data + tokens[i].first, tokens[i].second) + "'";
                    };
                    if (i >= push_tokens.size() || i >= pull_tokens.size() || push_tokens[i] != pull_tokens[i])
                        std::fprintf(stderr, "  token %zu: push %s vs pull %s\n", i, fmt(push_tokens).c_str(), fmt(pull_tokens).c_str());
                }
            }
        }
    }
    return mismatched_documents;
}

}

int main(int argc, char ** argv)
{
    try
    {
        size_t runs = 3;
        size_t limit_mb = 64;
        size_t verify_mb = 0;
        std::string tokenizer_filter;
        std::vector<std::string> files;

        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg = argv[i];
            if (arg.starts_with("--runs="))
                runs = std::strtoull(arg.data() + std::strlen("--runs="), nullptr, 10);
            else if (arg.starts_with("--limit-mb="))
                limit_mb = std::strtoull(arg.data() + std::strlen("--limit-mb="), nullptr, 10);
            else if (arg.starts_with("--verify-mb="))
                verify_mb = std::strtoull(arg.data() + std::strlen("--verify-mb="), nullptr, 10);
            else if (arg.starts_with("--tokenizer="))
                tokenizer_filter = arg.substr(std::strlen("--tokenizer="));
            else
                files.emplace_back(arg);
        }

        if (verify_mb != 0)
            limit_mb = verify_mb;

        if (runs == 0)
            runs = 1;

        if (files.empty())
        {
            const char * home = std::getenv("HOME");
            const std::string home_dir = home ? home : ".";
            files.push_back(home_dir + "/fineweb.csv");
            files.push_back(home_dir + "/logs.csv");
        }

        const size_t limit_bytes = limit_mb * (size_t(1) << 20);

        /// The definition string is parsed by TokenizerFactory like a tokenizer argument of a text index.
        const std::vector<std::pair<std::string, std::string>> tokenizer_defs =
        {
            {"ngrams(3)",        "ngrams(3)"},
            {"splitByNonAlpha",  "splitByNonAlpha"},
            {"sparseGrams(3,8)", "sparseGrams(3, 8)"},
            {"splitByString",    "splitByString([' ', '::'])"},
        };

        auto & factory = TokenizerFactory::instance();
        std::vector<std::pair<std::string, std::unique_ptr<ITokenizer>>> tokenizers;
        tokenizers.reserve(tokenizer_defs.size());
        for (const auto & [label, definition] : tokenizer_defs)
        {
            if (!tokenizer_filter.empty() && label.find(tokenizer_filter) == std::string::npos)
                continue;
            tokenizers.emplace_back(label, factory.get(definition));
        }

        std::printf("Tokenizer throughput benchmark (runs=%zu, best of run reported)\n", runs);

        for (const auto & file : files)
        {
            const std::vector<char> buffer = readFileWithPadding(file, limit_bytes);
            const std::vector<Document> documents = splitIntoDocuments(buffer);

            size_t total_text_bytes = 0;
            for (const auto & document : documents)
                total_text_bytes += document.length;

            std::printf("\nFile: %s\n", file.c_str());
            std::printf("  size: %.1f MB (%.1f MiB) of text in %zu documents\n",
                static_cast<double>(total_text_bytes) / 1e6,
                static_cast<double>(total_text_bytes) / static_cast<double>(size_t(1) << 20),
                documents.size());

            if (verify_mb != 0)
            {
                for (const auto & [label, tokenizer] : tokenizers)
                {
                    const size_t mismatched = verifyPushAgainstPull(*tokenizer, documents);
                    std::printf("  %-18s push vs pull: %s (%zu mismatched documents)\n",
                        label.c_str(), mismatched == 0 ? "OK" : "MISMATCH", mismatched);
                }
                continue;
            }

            std::printf("  %-18s %10s %12s %12s %16s %10s %18s\n",
                "tokenizer", "time, s", "input MB/s", "Mtokens/s", "tokens", "avg len", "checksum");
            std::printf("  %-18s %10s %12s %12s %16s %10s %18s\n",
                "------------------", "----------", "------------", "------------", "----------------", "----------", "------------------");

            for (const auto & [label, tokenizer] : tokenizers)
            {
                const BenchmarkResult result = benchmarkTokenizer(*tokenizer, documents, runs);

                const double mbps = static_cast<double>(total_text_bytes) / result.best_seconds / 1e6;
                const double mtps = static_cast<double>(result.token_count) / result.best_seconds / 1e6;
                const double avg_len = result.token_count
                    ? static_cast<double>(result.token_bytes) / static_cast<double>(result.token_count)
                    : 0.0;

                std::printf("  %-18s %10.3f %12.1f %12.2f %16zu %10.2f %#18llx\n",
                    label.c_str(), result.best_seconds, mbps, mtps, result.token_count, avg_len,
                    static_cast<unsigned long long>(result.checksum));
            }
        }

        return 0;
    }
    catch (const std::exception & e)
    {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
