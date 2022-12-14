// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.hardware.block/cpp/wire.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fit/defer.h>
#include <lib/sync/completion.h>
#include <lib/zircon-internal/xorshiftrand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/device/block.h>
#include <zircon/syscalls.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <atomic>

#include <perftest/results.h>

static uint64_t number(const char* str) {
  char* end;
  uint64_t n = strtoull(str, &end, 10);

  uint64_t m = 1;
  switch (*end) {
    case 'G':
    case 'g':
      m = 1024 * 1024 * 1024;
      break;
    case 'M':
    case 'm':
      m = 1024 * 1024;
      break;
    case 'K':
    case 'k':
      m = 1024;
      break;
  }
  return m * n;
}

static void bytes_per_second(uint64_t bytes, uint64_t nanos) {
  double s = (static_cast<double>(nanos)) / (static_cast<double>(1000000000));
  double rate = (static_cast<double>(bytes)) / s;

  const char* unit = "B";
  if (rate > 1024 * 1024) {
    unit = "MB";
    rate /= 1024 * 1024;
  } else if (rate > 1024) {
    unit = "KB";
    rate /= 1024;
  }
  fprintf(stderr, "%g %s/s\n", rate, unit);
}

static void ops_per_second(uint64_t count, uint64_t nanos) {
  double s = (static_cast<double>(nanos)) / (static_cast<double>(1000000000));
  double rate = (static_cast<double>(count)) / s;
  fprintf(stderr, "%g %s/s\n", rate, "ops");
}

using blkdev_t = struct {
  int fd;
  zx::vmo vmo;
  zx::fifo fifo;
  fidl::ClientEnd<fuchsia_hardware_block::Session> session;
  reqid_t reqid;
  fuchsia_hardware_block::wire::VmoId vmoid;
  size_t bufsz;
  fuchsia_hardware_block::wire::BlockInfo info;
};

static void blkdev_close(blkdev_t* blk) {
  if (blk->fd >= 0) {
    close(blk->fd);
  }
  blk->vmo.reset();
  blk->fifo.reset();
  blk->session.reset();
  memset(blk, 0, sizeof(blkdev_t));
  blk->fd = -1;
}

static zx_status_t blkdev_open(int fd, const char* dev, size_t bufsz, blkdev_t* blk) {
  memset(blk, 0, sizeof(blkdev_t));
  blk->fd = fd;
  blk->bufsz = bufsz;

  auto cleanup = fit::defer([blk]() { blkdev_close(blk); });

  fdio_cpp::UnownedFdioCaller caller(fd);
  fidl::UnownedClientEnd channel = caller.borrow_as<fuchsia_hardware_block::Block>();

  {
    const fidl::WireResult result = fidl::WireCall(channel)->GetInfo();
    if (!result.ok()) {
      fprintf(stderr, "error: cannot get block device info for '%s': %s\n", dev,
              result.FormatDescription().c_str());
      return result.status();
    }
    const fit::result response = result.value();
    if (response.is_error()) {
      fprintf(stderr, "error: cannot get block device info for '%s':%s\n", dev,
              zx_status_get_string(response.error_value()));
      return response.error_value();
    }
    blk->info = response.value()->info;
  }

  {
    zx::result server = fidl::CreateEndpoints(&blk->session);
    if (server.is_error()) {
      fprintf(stderr, "error: cannot create server for '%s': %s\n", dev, server.status_string());
      return server.status_value();
    }
    if (fidl::Status result = fidl::WireCall(channel)->OpenSession(std::move(server.value()));
        !result.ok()) {
      fprintf(stderr, "error: cannot open session for '%s': %s\n", dev,
              result.FormatDescription().c_str());
      return result.status();
    }
  }

  {
    const fidl::WireResult result = fidl::WireCall(blk->session)->GetFifo();
    if (!result.ok()) {
      fprintf(stderr, "error: cannot get fifo for '%s':%s\n", dev,
              result.FormatDescription().c_str());
      return result.status();
    }
    const fit::result response = result.value();
    if (response.is_error()) {
      fprintf(stderr, "error: cannot get fifo for '%s':%s\n", dev,
              zx_status_get_string(response.error_value()));
      return response.error_value();
    }
    blk->fifo = std::move(response.value()->fifo);
  }

  if (zx_status_t status = zx::vmo::create(bufsz, 0, &blk->vmo); status != ZX_OK) {
    fprintf(stderr, "error: out of memory: %s\n", zx_status_get_string(status));
    return status;
  }

  zx::vmo dup;
  if (zx_status_t status = blk->vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &dup); status != ZX_OK) {
    fprintf(stderr, "error: cannot duplicate handle: %s\n", zx_status_get_string(status));
    return status;
  }

  const fidl::WireResult result = fidl::WireCall(blk->session)->AttachVmo(std::move(dup));
  if (!result.ok()) {
    fprintf(stderr, "error: cannot attach vmo for '%s':%s\n", dev,
            result.FormatDescription().c_str());
    return result.status();
  }
  const fit::result response = result.value();
  if (response.is_error()) {
    fprintf(stderr, "error: cannot attach vmo for '%s':%s\n", dev,
            zx_status_get_string(response.error_value()));
    return response.error_value();
  }
  blk->vmoid = response.value()->vmoid;

  cleanup.cancel();
  return ZX_OK;
}

using bio_random_args_t = struct {
  blkdev_t* blk;
  size_t count;
  size_t xfer;
  uint64_t seed;
  int max_pending;
  bool write;
  bool linear;

  std::atomic<int> pending;
  sync_completion_t signal;
};

std::atomic<reqid_t> next_reqid(0);

static int bio_random_thread(void* arg) {
  auto* a = reinterpret_cast<bio_random_args_t*>(arg);

  size_t off = 0;
  size_t count = a->count;
  size_t xfer = a->xfer;

  size_t blksize = a->blk->info.block_size;
  size_t blkcount = ((count * xfer) / blksize) - (xfer / blksize);

  rand64_t r64 = RAND63SEED(a->seed);

  zx::fifo& fifo = a->blk->fifo;
  size_t dev_off = 0;

  while (count > 0) {
    while (a->pending.load() == a->max_pending) {
      sync_completion_wait(&a->signal, ZX_TIME_INFINITE);
      sync_completion_reset(&a->signal);
    }

    block_fifo_request_t req = {};
    req.reqid = next_reqid.fetch_add(1);
    req.vmoid = a->blk->vmoid.id;
    req.opcode = a->write ? BLOCKIO_WRITE : BLOCKIO_READ;
    req.length = static_cast<uint32_t>(xfer);
    req.vmo_offset = off;

    if (a->linear) {
      req.dev_offset = dev_off;
      dev_off += xfer;
    } else {
      req.dev_offset = (rand64(&r64) % blkcount) * blksize;
    }
    off += xfer;
    if ((off + xfer) > a->blk->bufsz) {
      off = 0;
    }

    req.length /= static_cast<uint32_t>(blksize);
    req.dev_offset /= blksize;
    req.vmo_offset /= blksize;

#if 0
        fprintf(stderr, "IO tid=%u vid=%u op=%x len=%zu vof=%zu dof=%zu\n",
                req.reqid, req.vmoid.id, req.opcode, req.length, req.vmo_offset, req.dev_offset);
#endif
    zx_status_t status = fifo.write(sizeof(req), &req, 1, nullptr);
    if (status == ZX_ERR_SHOULD_WAIT) {
      status = fifo.wait_one(ZX_FIFO_WRITABLE | ZX_FIFO_PEER_CLOSED, zx::time::infinite(), nullptr);
      if (status != ZX_OK) {
        fprintf(stderr, "failed waiting for fifo: %s\n", zx_status_get_string(status));
        fifo.reset();
        return -1;
      }
      continue;
    }
    if (status != ZX_OK) {
      fprintf(stderr, "error: failed writing to fifo: %s\n", zx_status_get_string(status));
      fifo.reset();
      return -1;
    }

    a->pending.fetch_add(1);
    count--;
  }
  return 0;
}

static zx_status_t bio_random(bio_random_args_t* a, uint64_t* _total, zx_duration_t* _res) {
  thrd_t t;
  int r;

  size_t count = a->count;
  zx::fifo& fifo = a->blk->fifo;

  zx_time_t t0 = zx_clock_get_monotonic();
  thrd_create(&t, bio_random_thread, a);

  auto cleanup = fit::defer([&fifo, &t]() {
    int r;
    fifo.reset();
    thrd_join(t, &r);
  });

  while (count > 0) {
    block_fifo_response_t resp;
    zx_status_t status = fifo.read(sizeof(resp), &resp, 1, nullptr);
    if (status == ZX_ERR_SHOULD_WAIT) {
      status = fifo.wait_one(ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED, zx::time::infinite(), nullptr);
      if (status != ZX_OK) {
        fprintf(stderr, "failed waiting for fifo: %s\n", zx_status_get_string(status));
        return status;
      }
      continue;
    }
    if (status != ZX_OK) {
      fprintf(stderr, "error: failed reading fifo: %s\n", zx_status_get_string(status));
      return status;
    }
    if (resp.status != ZX_OK) {
      fprintf(stderr, "error: io txn failed %s (%zu remaining)\n",
              zx_status_get_string(resp.status), count);
      return resp.status;
    }
    count--;
    if (a->pending.fetch_sub(1) == a->max_pending) {
      sync_completion_signal(&a->signal);
    }
  }

  cleanup.cancel();

  zx_time_t t1;
  t1 = zx_clock_get_monotonic();

  fprintf(stderr, "waiting for thread to exit...\n");
  thrd_join(t, &r);

  *_res = zx_time_sub_time(t1, t0);
  *_total = a->count * a->xfer;
  return ZX_OK;
}

void usage() {
  fprintf(stderr,
          "usage: biotime <option>* <device>\n"
          "\n"
          "args:  -bs <num>     transfer block size (multiple of 4K)\n"
          "       -total-bytes-to-transfer <num>  total amount to read or write\n"
          "       -iter <num-iterations> total number of iterations (0 stands for infinite)\n"
          "       -mo <num>     maximum outstanding ops (1..128)\n"
          "       -read         test reading from the block device (default)\n"
          "       -write        test writing to the block device\n"
          "       -live-dangerously  required if using \"-write\"\n"
          "       -linear       transfers in linear order (default)\n"
          "       -random       random transfers across total range\n"
          "       -output-file <filename>  destination file for "
          "writing results in JSON format\n");
}

#define needparam()                                                      \
  do {                                                                   \
    argc--;                                                              \
    argv++;                                                              \
    if (argc == 0) {                                                     \
      fprintf(stderr, "error: option %s needs a parameter\n", argv[-1]); \
      return -1;                                                         \
    }                                                                    \
  } while (0)
#define nextarg() \
  do {            \
    argc--;       \
    argv++;       \
  } while (0)
#define error(x...)     \
  do {                  \
    fprintf(stderr, x); \
    usage();            \
    return -1;          \
  } while (0)

int main(int argc, char** argv) {
  blkdev_t blk;

  bool live_dangerously = false;
  bio_random_args_t a = {};
  bool opt_write = false;
  bool opt_linear = true;
  int opt_max_pending = 128;
  size_t opt_xfer_size = 32768;
  uint64_t opt_num_iter = 1;
  bool loop_forever = false;

  a.blk = &blk;
  a.seed = 7891263897612ULL;
  const char* output_file = nullptr;

  size_t total = 0;

  nextarg();
  while (argc > 0) {
    if (argv[0][0] != '-') {
      break;
    }
    if (!strcmp(argv[0], "-bs")) {
      needparam();
      opt_xfer_size = number(argv[0]);
      if ((opt_xfer_size == 0) || (opt_xfer_size % 4096)) {
        error("error: block size must be multiple of 4K\n");
      }
    } else if (!strcmp(argv[0], "-total-bytes-to-transfer")) {
      needparam();
      total = number(argv[0]);
    } else if (!strcmp(argv[0], "-mo")) {
      needparam();
      size_t n = number(argv[0]);
      if ((n < 1) || (n > 128)) {
        error("error: max pending must be between 1 and 128\n");
      }
      opt_max_pending = static_cast<int>(n);
    } else if (!strcmp(argv[0], "-read")) {
      opt_write = false;
    } else if (!strcmp(argv[0], "-write")) {
      opt_write = true;
    } else if (!strcmp(argv[0], "-live-dangerously")) {
      live_dangerously = true;
    } else if (!strcmp(argv[0], "-linear")) {
      opt_linear = true;
    } else if (!strcmp(argv[0], "-random")) {
      opt_linear = false;
    } else if (!strcmp(argv[0], "-output-file")) {
      needparam();
      output_file = argv[0];
    } else if (!strcmp(argv[0], "-h")) {
      usage();
      return 0;
    } else if (!strcmp(argv[0], "-iter")) {
      needparam();
      opt_num_iter = strtoull(argv[0], nullptr, 10);
      if (opt_num_iter == 0)
        loop_forever = true;
    } else {
      error("error: unknown option: %s\n", argv[0]);
    }
    nextarg();
  }
  if (argc == 0) {
    error("error: no device specified\n");
  }
  if (argc > 1) {
    error("error: unexpected arguments\n");
  }
  if (a.write && !live_dangerously) {
    error(
        "error: the option \"-live-dangerously\" is required when using"
        " \"-write\"\n");
  }
  const char* device_filename = argv[0];

  do {
    a.xfer = opt_xfer_size;
    a.max_pending = opt_max_pending;
    a.write = opt_write;
    a.linear = opt_linear;
    int fd = open(device_filename, O_RDONLY);
    if (fd < 0) {
      fprintf(stderr, "error: cannot open '%s'\n", device_filename);
      return -1;
    }
    if (blkdev_open(fd, device_filename, 8 * 1024 * 1024, &blk) != ZX_OK) {
      return -1;
    }

    size_t devtotal = blk.info.block_count * blk.info.block_size;

    // default to entire device
    if ((total == 0) || (total > devtotal)) {
      total = devtotal;
    }
    a.count = total / a.xfer;

    zx_duration_t res = 0;
    total = 0;
    if (bio_random(&a, &total, &res) != ZX_OK) {
      return -1;
    }

    fprintf(stderr, "%zu bytes in %zu ns: ", total, res);
    bytes_per_second(total, res);
    fprintf(stderr, "%zu ops in %zu ns: ", a.count, res);
    ops_per_second(a.count, res);

    if (output_file) {
      perftest::ResultsSet results;
      auto* test_case =
          results.AddTestCase("fuchsia.zircon", "BlockDeviceThroughput", "bytes/second");
      double time_in_seconds = static_cast<double>(res) / 1e9;
      test_case->AppendValue(static_cast<double>(total) / time_in_seconds);
      if (!results.WriteJSONFile(output_file)) {
        return 1;
      }
    }
    blkdev_close(&blk);
  } while (loop_forever || (--opt_num_iter > 0));

  return 0;
}
