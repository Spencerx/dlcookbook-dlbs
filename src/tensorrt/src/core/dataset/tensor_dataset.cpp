/*
 (c) Copyright [2017] Hewlett Packard Enterprise Development LP
 
  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at
 
     http://www.apache.org/licenses/LICENSE-2.0
 
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "core/dataset/tensor_dataset.hpp"
#include <sstream>

#define _XOPEN_SOURCE 700
#include <unistd.h>
#include <fcntl.h>


void tensor_dataset::prefetcher_func(tensor_dataset* myself,
                                     const size_t prefetcher_id, const size_t num_prefetchers) {
    sharded_vector<std::string> my_files(myself->file_names_, myself->prefetchers_.size(), prefetcher_id);
    std::ostringstream oss;
    oss << my_files;
    myself->logger_.log_info(fmt("[prefetcher       %02d/%02d]: %s", prefetcher_id, num_prefetchers, oss.str().c_str()));
    myself->logger_.log_info(fmt("[prefetcher       %02d/%02d]: image read strategy - low level C IO api with POSIX_FADV_DONTNEED", prefetcher_id, num_prefetchers));
        
    const int height(static_cast<int>(myself->opts_.height_)),
              width(static_cast<int>(myself->opts_.width_));
    const size_t img_size = 3 * height * width;
    running_average fetch, load, submit;
    
    inference_msg *output(nullptr);
    size_t num_images_in_batch = 0;
    int file_descriptor(-1);
    try {
        timer clock;
        clock.restart();
        while (!myself->stop_) {
            // Get inference request if we do not have one.
            if (!output) {
                timer fetch_clock;
                output = myself->inference_msg_pool_->get();
                fetch.update(fetch_clock.ms_elapsed());
            }
            // If we have read all images, submit them
            if (num_images_in_batch == output->batch_size()) {
                // Submit inference request
                load.update(clock.ms_elapsed());
                timer submit_clock;
                myself->request_queue_->push(output);
                submit.update(submit_clock.ms_elapsed());
                num_images_in_batch = 0;
                output = nullptr;
                clock.restart();
                continue;
            }
            // Check if we need to open next image file
            if (file_descriptor == -1) {
                const auto fname = my_files.next();
                file_descriptor = open(fname.c_str(), O_RDONLY);
                fdatasync(file_descriptor);
            }
            // Try to read as many images in one read call as we need
            const size_t need_images_to_load = output->batch_size() - num_images_in_batch;
            const ssize_t num_bytes_read = read(
                file_descriptor,
                (void*)(output->input().data()+img_size*num_images_in_batch), 
                need_images_to_load*sizeof(float)*img_size
            );
            // If nothing has been loaded, go to next file
            if (num_bytes_read <=0) {
                posix_fadvise(file_descriptor, 0, 0, POSIX_FADV_DONTNEED);
                close(file_descriptor);
                file_descriptor = -1;
                continue;
            }
            // How many images have we just loaded?
            const size_t num_images_loaded = num_bytes_read / (sizeof(float)*img_size);
            num_images_in_batch += num_images_loaded;
        }
    } catch(queue_closed) {
    }
    if (file_descriptor > 0) {
        posix_fadvise(file_descriptor, 0, 0, POSIX_FADV_DONTNEED);
        close(file_descriptor);
        file_descriptor = -1;
    }
    myself->logger_.log_info(fmt(
        "[prefetcher       %02d/%02d]: {fetch:%.5f}-->--[load:%.5f]-->--{submit:%.5f}",
        prefetcher_id, num_prefetchers, fetch.value(), load.value(), submit.value()
    ));
}

tensor_dataset::tensor_dataset(const dataset_opts& opts, inference_msg_pool* pool,
                               abstract_queue<inference_msg*>* request_queue, logger_impl& logger)
: dataset(pool, request_queue), opts_(opts), logger_(logger) {

    fs_utils::initialize_dataset(opts_.data_dir_, file_names_);
    if (opts.shuffle_files_) {
        std::random_shuffle(file_names_.begin(), file_names_.end());
    }
    prefetchers_.resize(opts_.num_prefetchers_, nullptr);
}
    
void tensor_dataset::run() {
    // Run prefetch workers
    for (size_t i=0; i<prefetchers_.size(); ++i) {
        prefetchers_[i] = new std::thread(&(tensor_dataset::prefetcher_func), this, i, prefetchers_.size());
    }
    // Wait
    for (auto& prefetcher : prefetchers_) {
        if (prefetcher->joinable()) prefetcher->join();
        delete prefetcher;
    }
}

float tensor_dataset::benchmark(const std::string dir, const size_t batch_size, const size_t img_size,
                                const size_t num_prefetches, const size_t num_infer_msgs,
                                const int num_warmup_batches, const int num_batches) {
    logger_impl logger;
    dataset_opts opts;
    opts.data_dir_ = dir;
    opts.num_prefetchers_ = num_prefetches;
    opts.prefetch_batch_size_=batch_size;
    opts.height_ = img_size;
    opts.width_ = img_size;
    opts.shuffle_files_ = true;
        
    inference_msg_pool pool(num_infer_msgs, opts.prefetch_batch_size_, 3*opts.height_*opts.width_, 1000);
    thread_safe_queue<inference_msg*> request_queue;
    tensor_dataset data(opts, &pool, &request_queue, logger);
        
    const float throughput = dataset::benchmark(&data, num_warmup_batches, num_batches, logger);
    logger.log_info(fmt("[benchmarks            ]: num_readers=%d, throughput=%.2f", opts.num_prefetchers_, throughput));
    data.join();
    return throughput;
}
