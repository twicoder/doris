// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <atomic>
#include <memory>

#include "common/status.h"
#include "util/threadpool.h"
#include "vec/exec/scan/vscanner.h"

namespace doris {
class ExecEnv;

namespace vectorized {
class VScanner;
} // namespace vectorized

template <typename T>
class BlockingQueue;
} // namespace doris

namespace doris::vectorized {
class ScannerDelegate;
class ScanTask;
class ScannerContext;

// Responsible for the scheduling and execution of all Scanners of a BE node.
// Execution thread pool
//     When a ScannerContext is launched, it will submit the running scanners to this scheduler.
//     The scheduling thread will submit the running scanner and its ScannerContext
//     to the execution thread pool to do the actual scan task.
//     Each Scanner will act as a producer, read the next block and put it into
//     the corresponding block queue.
//     The corresponding ScanNode will act as a consumer to consume blocks from the block queue.
//     After the block is consumed, the unfinished scanner will resubmit to this scheduler.
class ScannerScheduler {
public:
    ScannerScheduler();
    ~ScannerScheduler();

    [[nodiscard]] Status init(ExecEnv* env);

    void submit(std::shared_ptr<ScannerContext> ctx, std::shared_ptr<ScanTask> scan_task);

    void stop();

    std::unique_ptr<ThreadPoolToken> new_limited_scan_pool_token(ThreadPool::ExecutionMode mode,
                                                                 int max_concurrency);

    int remote_thread_pool_max_size() const { return _remote_thread_pool_max_size; }

private:
    static void _scanner_scan(std::shared_ptr<ScannerContext> ctx,
                              std::shared_ptr<ScanTask> scan_task);

    void _register_metrics();

    static void _deregister_metrics();

    // execution thread pool
    // _local_scan_thread_pool is for local scan task(typically, olap scanner)
    // _remote_scan_thread_pool is for remote scan task(cold data on s3, hdfs, etc.)
    // _limited_scan_thread_pool is a special pool for queries with resource limit
    std::unique_ptr<PriorityThreadPool> _local_scan_thread_pool;
    std::unique_ptr<PriorityThreadPool> _remote_scan_thread_pool;
    std::unique_ptr<ThreadPool> _limited_scan_thread_pool;

    // true is the scheduler is closed.
    std::atomic_bool _is_closed = {false};
    bool _is_init = false;
    int _remote_thread_pool_max_size;
};

struct SimplifiedScanTask {
    SimplifiedScanTask() = default;
    SimplifiedScanTask(std::function<void()> scan_func,
                       std::shared_ptr<vectorized::ScannerContext> scanner_context) {
        this->scan_func = scan_func;
        this->scanner_context = scanner_context;
    }

    std::function<void()> scan_func;
    std::shared_ptr<vectorized::ScannerContext> scanner_context = nullptr;
};

// used for cpu hard limit
class SimplifiedScanScheduler {
public:
    SimplifiedScanScheduler(std::string wg_name, CgroupCpuCtl* cgroup_cpu_ctl) {
        _scan_task_queue = std::make_unique<BlockingQueue<SimplifiedScanTask>>(
                config::doris_scanner_thread_pool_queue_size);
        _is_stop.store(false);
        _cgroup_cpu_ctl = cgroup_cpu_ctl;
        _wg_name = wg_name;
    }

    ~SimplifiedScanScheduler() {
        stop();
        LOG(INFO) << "Scanner sche " << _wg_name << " shutdown";
    }

    void stop() {
        _is_stop.store(true);
        _scan_task_queue->shutdown();
        _scan_thread_pool->shutdown();
        _scan_thread_pool->wait();
    }

    Status start() {
        RETURN_IF_ERROR(ThreadPoolBuilder("Scan_" + _wg_name)
                                .set_min_threads(config::doris_scanner_thread_pool_thread_num)
                                .set_max_threads(config::doris_scanner_thread_pool_thread_num)
                                .set_cgroup_cpu_ctl(_cgroup_cpu_ctl)
                                .build(&_scan_thread_pool));

        for (int i = 0; i < config::doris_scanner_thread_pool_thread_num; i++) {
            RETURN_IF_ERROR(_scan_thread_pool->submit_func([this] { this->_work(); }));
        }
        return Status::OK();
    }

    BlockingQueue<SimplifiedScanTask>* get_scan_queue() { return _scan_task_queue.get(); }

private:
    void _work() {
        while (!_is_stop.load()) {
            SimplifiedScanTask scan_task;
            if (_scan_task_queue->blocking_get(&scan_task)) {
                scan_task.scan_func();
            };
        }
    }

    std::unique_ptr<ThreadPool> _scan_thread_pool;
    std::unique_ptr<BlockingQueue<SimplifiedScanTask>> _scan_task_queue;
    std::atomic<bool> _is_stop;
    CgroupCpuCtl* _cgroup_cpu_ctl = nullptr;
    std::string _wg_name;
};

} // namespace doris::vectorized
