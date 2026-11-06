#include <getopt.h>
#include <signal.h>

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "payload.h"
#include "progress_renderer.h"
#include "util.h"

namespace {

void on_sigint(int) { pd::request_stop(); }

void usage(const char* argv0) {
  fprintf(stderr, "Usage: %s [options] [inputfile]\n", argv0);
  fprintf(stderr,
          "\nInput can be a payload.bin or an OTA zip containing payload.bin.\n");
  fprintf(stderr, "\nOptions:\n");
  fprintf(stderr, "  -c, -concurrency N     Workers to extract concurrently (default: CPUs)\n");
  fprintf(stderr, "  -l, -list              Show list of partitions in payload.bin\n");
  fprintf(stderr, "  -o, -output DIR        Set output directory\n");
  fprintf(stderr, "  -p, -partitions LIST   Dump only selected partitions (comma-separated)\n");
  fprintf(stderr, "  -old DIR               Directory with base images for incremental OTA\n");
  fprintf(stderr, "  -q, -quiet             Suppress non-essential output\n");
  fprintf(stderr, "  -m, -machine-readable  Machine-readable output format\n");
  fprintf(stderr, "  -no-verify             Skip sha256 verification\n");
  fprintf(stderr, "  -no-fec                Skip dm-verity FEC generation\n");
  fprintf(stderr, "  -h, -help              Show this help\n");
  fprintf(stderr, "\nExamples:\n");
  fprintf(stderr, "  %s ota.zip                       # Extract all partitions\n", argv0);
  fprintf(stderr, "  %s -l payload.bin                # List partitions\n", argv0);
  fprintf(stderr, "  %s -p boot,system payload.bin    # Extract selected partitions\n", argv0);
  fprintf(stderr, "  %s -old base_images delta.zip    # Apply an incremental OTA\n", argv0);
}

void print_partition_list(const std::vector<pd::Partition>& parts,
                          bool machine_readable) {
  if (machine_readable) {
    for (const auto& part : parts) {
      printf("%s:%llu\n", part.name.c_str(),
             static_cast<unsigned long long>(part.new_size / 1024));
    }
    return;
  }
  printf("Found partitions:\n");
  std::string line;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) line += ", ";
    line += parts[i].name + " (" + pd::human_bytes(parts[i].new_size);
    if (parts[i].needs_source) line += ", delta";
    line += ")";
  }
  printf("%s\n", line.c_str());
}

std::vector<std::string> split_commas(const std::string& value) {
  std::vector<std::string> out;
  if (value.empty()) return out;
  size_t start = 0;
  for (;;) {
    size_t comma = value.find(',', start);
    if (comma == std::string::npos) {
      out.push_back(value.substr(start));
      break;
    }
    out.push_back(value.substr(start, comma - start));
    start = comma + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  bool list = false;
  bool quiet = false;
  bool machine = false;
  bool no_verify = false;
  bool no_fec = false;
  std::string partitions_arg;
  std::string output_dir;
  std::string old_dir;
  int concurrency = 0;

  static const struct option kLongOptions[] = {
      {"concurrency", required_argument, nullptr, 'c'},
      {"list", no_argument, nullptr, 'l'},
      {"output", required_argument, nullptr, 'o'},
      {"partitions", required_argument, nullptr, 'p'},
      {"old", required_argument, nullptr, 1000},
      {"quiet", no_argument, nullptr, 'q'},
      {"machine-readable", no_argument, nullptr, 'm'},
      {"no-verify", no_argument, nullptr, 1001},
      {"no-fec", no_argument, nullptr, 1002},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  optind = 1;
  for (;;) {
    int option = getopt_long_only(argc, argv, "c:lo:p:qmh", kLongOptions,
                                  nullptr);
    if (option == -1) break;
    switch (option) {
      case 'c':
        concurrency = atoi(optarg);
        break;
      case 'l':
        list = true;
        break;
      case 'o':
        output_dir = optarg;
        break;
      case 'p':
        partitions_arg = optarg;
        break;
      case 1000:
        old_dir = optarg;
        break;
      case 'q':
        quiet = true;
        break;
      case 'm':
        machine = true;
        break;
      case 1001:
        no_verify = true;
        break;
      case 1002:
        no_fec = true;
        break;
      case 'h':
        usage(argv[0]);
        return 0;
      default:
        usage(argv[0]);
        return 2;
    }
  }

  if (optind >= argc) {
    usage(argv[0]);
    return 2;
  }
  if (optind + 1 < argc) {
    fprintf(stderr, "Error: unexpected extra argument %s\n", argv[optind + 1]);
    usage(argv[0]);
    return 2;
  }
  const std::string input = argv[optind];

  signal(SIGINT, on_sigint);

  try {
    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<pd::Payload> payload = pd::Payload::open(input);
    const auto& header = payload->header();
    const auto& all_parts = payload->partitions();
    bool needs_source = false;
    for (const auto& part : all_parts) {
      if (part.needs_source) needs_source = true;
    }

    const bool verbose = !quiet && !machine;
    if (verbose) {
      printf("payload.bin: %s\n", input.c_str());
      printf("Payload Version: %llu\n",
             static_cast<unsigned long long>(header.version));
      printf("Payload Manifest Length: %llu\n",
             static_cast<unsigned long long>(header.manifest_len));
      printf("Payload Manifest Signature Length: %u\n",
             header.metadata_signature_len);
      if (needs_source) {
        printf("Delta payload: minor version %u (source images required, pass "
               "-old)\n",
               payload->minor_version());
      } else if (payload->is_delta()) {
        printf("Partial update payload: minor version %u (no source images "
               "required)\n",
               payload->minor_version());
      }
    }

    if (list) {
      print_partition_list(all_parts, machine);
      return 0;
    }
    if (verbose) print_partition_list(all_parts, false);

    std::string target_dir = output_dir;
    if (target_dir.empty()) {
      target_dir = "extracted_" + pd::format_timestamp();
    }
    std::vector<std::string> selected_names = split_commas(partitions_arg);
    if (verbose) {
      int workers = concurrency;
      if (workers <= 0) {
        workers = static_cast<int>(std::thread::hardware_concurrency());
        if (workers <= 0) workers = 1;
      }
      printf("Number of workers: %d\n", workers);
      fflush(stdout);
    }

    pd::ProgressRenderer::Mode mode = pd::ProgressRenderer::Mode::kBar;
    if (quiet || machine) {
      mode = machine ? pd::ProgressRenderer::Mode::kMachine
                     : pd::ProgressRenderer::Mode::kQuiet;
    }
    std::vector<std::pair<std::string, uint64_t>> progress_parts;
    for (const auto& part : all_parts) {
      bool wanted = selected_names.empty();
      for (const auto& name : selected_names) {
        if (name == part.name) wanted = true;
      }
      if (wanted) progress_parts.emplace_back(part.name, part.new_size);
    }
    pd::ProgressRenderer renderer(mode, progress_parts);
    renderer.start();

    pd::ExtractOptions options;
    options.output_dir = target_dir;
    options.partitions = selected_names;
    options.source_dir = old_dir;
    options.concurrency = concurrency;
    options.skip_verify = no_verify;
    options.no_fec = no_fec;
    options.progress = quiet && !machine ? nullptr : &renderer;
    options.quiet = quiet;

    try {
      payload->extract(options);
    } catch (...) {
      renderer.stop();
      throw;
    }
    renderer.stop();
    if (verbose) {
      const double elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - started).count();
      printf("Extracted %zu partition(s) to %s\n", progress_parts.size(),
             target_dir.c_str());
      printf("Elapsed time: %.3f s\n", elapsed);
    }
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "Error: %s\n", e.what());
    if (old_dir.empty() && strstr(e.what(), "source images are required") != nullptr) {
      fprintf(stderr,
              "Hint: extract the previous (base) full OTA first, then pass its "
              "output directory via -old, e.g.:\n");
      fprintf(stderr, "  %s -o base_images base_ota.zip\n", argv[0]);
      fprintf(stderr, "  %s -old base_images -o new_images %s\n", argv[0],
              input.c_str());
    }
    return 1;
  }
}
