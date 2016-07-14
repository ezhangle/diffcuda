#include "diffcuda.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <immintrin.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "xxhash.h"

namespace diffcuda {

Lines preprocess(const uint8_t *data, size_t size) noexcept {
  assert((reinterpret_cast<intptr_t>(data) & 0x1F) == 0);
  std::vector<uint32_t> lines;
  lines.reserve(size / 40);
  std::vector<uint64_t> hashes;
  hashes.reserve(size / 40);
  uint32_t ppos = 0;
  const __m256i newline = _mm256_set1_epi8('\n');
  for (uint32_t j = 0; j < size; j += 32) {
    __m256i buffer = _mm256_load_si256(
        reinterpret_cast<const __m256i *>(data + j));
    buffer = _mm256_cmpeq_epi8(buffer, newline);
    uint32_t mask = _mm256_movemask_epi8(buffer);
    if (mask != 0) {
      int left = __builtin_ctz(mask);
      int right = 32 - __builtin_clz(mask);
      if (right - left < 8) {
        auto pos = j + left + 1;
        hashes.push_back(XXH64(data + ppos, pos - ppos, 0));
        lines.push_back(pos);
        ppos = pos;
        for (int k = left + 1; k < right; k++) {
          if (mask & (1 << k)) {
            pos = j + k + 1;
            hashes.push_back(XXH64(data + ppos, pos - ppos, 0));
            lines.push_back(pos);
            ppos = pos;
          }
        }
      } else {
        uint32_t lm = mask & 0xFFFF;
        uint32_t rm = mask >> 16;
        for (int k = __builtin_ctz(lm); k < 32 - __builtin_clz(lm); k++) {
          if (lm & (1 << k)) {
            auto pos = j + k + 1;
            hashes.push_back(XXH64(data + ppos, pos - ppos, 0));
            lines.push_back(pos);
            ppos = pos;
          }
        }
        for (int k = __builtin_ctz(rm); k < 32 - __builtin_clz(rm); k++) {
          if (rm & (1 << k)) {
            auto pos = j + k + 16 + 1;
            hashes.push_back(XXH64(data + ppos, pos - ppos, 0));
            lines.push_back(pos);
            ppos = pos;
          }
        }
      }
    }
  }
  return Lines(std::move(lines), std::move(hashes));
}

Script diff(const uint8_t *old, size_t old_size, const uint8_t *now,
            size_t now_size) noexcept {
  std::vector<uint32_t> deletions;
  std::vector<Insertion> insertions;
  return Script(std::move(deletions), std::move(insertions));
}

}  // namespace diffcuda

int main(int argc, const char **argv) {
  size_t size;
  {
    struct stat sstat;
    stat(argv[1], &sstat);
    size = static_cast<size_t>(sstat.st_size);
  }
  std::unique_ptr<uint8_t []> contents(new uint8_t[size + BLOCK_SIZE + 32]);
  auto data = contents.get();
  data += BLOCK_SIZE - (reinterpret_cast<intptr_t>(data) & (BLOCK_SIZE - 1));
  auto file = open(argv[1], O_RDONLY | O_DIRECT);
  auto size_read = read(file, data, size + BLOCK_SIZE - (size & (BLOCK_SIZE - 1)));
  if (size_read != static_cast<ssize_t>(size)) {
    fprintf(stderr, "Failed to read %s", argv[1]);
    return 1;
  }
  close(file);
  memset(data + size, 0, 32);

  auto result = diffcuda::preprocess(data, size);
  std::vector<uint32_t> &&lines(std::move(std::get<0>(result)));
  std::vector<uint64_t> &&hashes(std::move(std::get<1>(result)));
  printf("%p %p\n", lines.data(), hashes.data());

  return 0;
}