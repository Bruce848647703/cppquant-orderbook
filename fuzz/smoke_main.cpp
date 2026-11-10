#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);

namespace {

struct Options {
  int seconds{60};
  std::uint64_t seed{42};
  std::string corpus;
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--seconds" && index + 1 < argc) {
      options.seconds = std::stoi(argv[++index]);
    } else if (argument == "--seed" && index + 1 < argc) {
      options.seed = std::stoull(argv[++index]);
    } else if (argument == "--help" || argument == "-h") {
      std::printf("usage: %s [corpus_dir] [--seconds N] [--seed N]\n", argv[0]);
      std::exit(0);
    } else if (!argument.empty() && argument[0] != '-') {
      options.corpus = argument;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      std::exit(2);
    }
  }
  if (options.seconds <= 0) {
    std::fprintf(stderr, "seconds must be positive\n");
    std::exit(2);
  }
  return options;
}

std::vector<std::vector<std::uint8_t>> read_corpus(const std::string& path) {
  std::vector<std::vector<std::uint8_t>> seeds;
  if (path.empty()) {
    return seeds;
  }
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
    if (error || !entry.is_regular_file()) {
      continue;
    }
    std::ifstream file(entry.path(), std::ios::binary);
    if (!file) {
      continue;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    if (!bytes.empty()) {
      seeds.push_back(std::move(bytes));
    }
  }
  return seeds;
}

void mutate(std::vector<std::uint8_t>& input, std::mt19937_64& rng) {
  const int operations = 1 + static_cast<int>(rng() % 8);
  for (int operation = 0; operation < operations; ++operation) {
    if (input.empty() || rng() % 5 == 0) {
      input.insert(input.begin() +
                       static_cast<std::ptrdiff_t>(rng() % (input.size() + 1)),
                   static_cast<std::uint8_t>(rng()));
      continue;
    }
    const std::size_t index = static_cast<std::size_t>(rng() % input.size());
    switch (rng() % 4) {
    case 0:
      input[index] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
      break;
    case 1:
      input[index] = static_cast<std::uint8_t>(rng());
      break;
    case 2:
      input.erase(input.begin() + static_cast<std::ptrdiff_t>(index));
      break;
    default:
      if (input.size() < 8'192) {
        input.insert(input.begin() + static_cast<std::ptrdiff_t>(index),
                     static_cast<std::uint8_t>(rng()));
      }
      break;
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  const auto seeds = read_corpus(options.corpus);
  std::mt19937_64 rng(options.seed);

  for (const auto& seed : seeds) {
    LLVMFuzzerTestOneInput(seed.data(), seed.size());
  }

  const auto start = std::chrono::steady_clock::now();
  std::size_t iterations = 0;
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
             .count() < options.seconds) {
    std::vector<std::uint8_t> input;
    if (!seeds.empty() && rng() % 4 != 0) {
      input = seeds[static_cast<std::size_t>(rng() % seeds.size())];
      mutate(input, rng);
    } else {
      const std::size_t size = static_cast<std::size_t>(rng() % 1'024);
      input.resize(size);
      for (auto& byte : input) {
        byte = static_cast<std::uint8_t>(rng());
      }
    }
    LLVMFuzzerTestOneInput(input.data(), input.size());
    ++iterations;
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  std::printf("fuzz smoke ok: %zu iterations in %.1f s, %zu seeds\n",
              iterations, seconds, seeds.size());
  return 0;
}