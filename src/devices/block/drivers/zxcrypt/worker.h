// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_WORKER_H_
#define SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_WORKER_H_

#include <lib/zx/port.h>
#include <stdint.h>
#include <threads.h>
#include <zircon/syscalls/port.h>
#include <zircon/types.h>

#include <atomic>

#include "src/devices/block/drivers/zxcrypt/extra.h"
#include "src/devices/block/drivers/zxcrypt/queue.h"
#include "src/security/lib/fcrypto/cipher.h"
#include "src/security/lib/zxcrypt/ddk-volume.h"

namespace zxcrypt {

class Device;

// |zxcrypt::Worker| represents a thread performing cryptographic transformations on block I/O data.
// Since these operations may have significant and asymmetric costs between encrypting and
// decrypting, they are performed asynchronously on separate threads.  The |zxcrypt::Device| may
// spin up multiple workers pulling from a shared queues to optimize the throughput.
class Worker final {
 public:
  Worker();
  ~Worker();

  // Starts the worker, which will service requests sent from the given |device| on the given
  // |queue|.  Cryptographic operations will use the key material from the given |volume|.
  zx_status_t Start(Device* device, const DdkVolume& volume, Queue<block_op_t*>& queue);

  // Asks the worker to stop.  This call blocks until the worker has finished processing the
  // currently queued operations and exits.
  zx_status_t Stop();

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Worker);

  // Loop thread.  Reads an I/O request from the |port_| and dispatches it between |EncryptWrite|
  // and |DecryptRead|.
  static int WorkerRun(void* arg) { return static_cast<Worker*>(arg)->Run(); }
  zx_status_t Run();

  // Copies the plaintext data to be written to the write buffer location given in |block|'s extra
  // information, and encrypts it before sending it to the parent device.
  zx_status_t EncryptWrite(block_op_t* block);

  // Maps the ciphertext data in |block|, and decrypts it in place before completing the block op.
  zx_status_t DecryptRead(block_op_t* block);

  // The cipher objects used to perform cryptographic.  See notes on "random access" in
  // crypto/cipher.h.
  crypto::Cipher encrypt_;
  crypto::Cipher decrypt_;

  // The device associated with this worker.
  Device* device_;

  // The executing thread for this worker
  thrd_t thrd_;

  // Indicates if a thread was created for this worker
  std::atomic_bool started_;

  // The queue from which requests are serviced.
  Queue<block_op_t*>* queue_ = nullptr;
};

}  // namespace zxcrypt

#endif  // SRC_DEVICES_BLOCK_DRIVERS_ZXCRYPT_WORKER_H_
