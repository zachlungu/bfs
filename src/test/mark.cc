// Copyright (c) 2016, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#include <gflags/gflags.h>
#include <vector>
#include <string>
#include <iostream>
#include <common/string_util.h>
#include <common/thread_pool.h>
#include <boost/bind.hpp>

#include "mark.h"

DECLARE_string(flagfile);
DECLARE_string(nameserver_nodes);

DEFINE_string(mode, "put", "[put | read]");
DEFINE_int64(count, 0, "put/read/delete file count");
DEFINE_int32(thread, 5, "thread num");
DEFINE_int32(seed, 301, "random seet");
DEFINE_int32(file_size, 1024, "file size in KB");

namespace baidu {
namespace bfs {

// borrowed from LevelDB
class Random {
private:
    uint32_t seed_;
public:
    explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) { }
    uint32_t Next() {
        static const uint32_t M = 2147483647L;
        static const uint64_t A = 16807;
        uint64_t product = seed_ * A;
        seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
        if (seed_ > M) {
            seed_ -= M;
        }
    return seed_;
    }
    uint32_t Uniform(int n) { return Next() % n; }
};

Mark::Mark() : fs_(NULL), file_size_(FLAGS_file_size << 10), exit_(false) {
    if (!FS::OpenFileSystem(FLAGS_nameserver_nodes.c_str(), &fs_)) {
        fprintf(stderr, "Open filesytem %s fail\n", FLAGS_nameserver_nodes.c_str());
        assert(0);
    }

    thread_pool_ = new common::ThreadPool(FLAGS_thread + 1);
    rand_ = new Random*[FLAGS_thread];
    for (int i = 0; i < FLAGS_thread; i++) {
      rand_[i] = new Random(FLAGS_seed + i);
    }
}

void Mark::Put(const std::string& filename, const std::string& base, int thread_id) {
    File* file;
    if (!fs_->OpenFile(filename.c_str(), O_WRONLY | O_TRUNC, 664, -1, &file)) {
        assert(0);
    }
    uint64_t len = 0;
    uint64_t buf_size = 1 << 20;
    uint64_t buf_base_size = buf_size / 2;
    while (len < file_size_) {
        uint32_t w = buf_base_size + rand_[thread_id]->Uniform(buf_base_size);
        if (file_size_ - len < w) {
            file->Write(base.c_str(), file_size_ - len);
            break;
        }
        int write_len = file->Write(base.c_str(), w);
        if ((uint64_t)write_len != w) {
            assert(0);
        }
        len += write_len;
    }
    if (!file->Close()) {
        assert(0);
    }
    delete file;
    put_counter_.Inc();
}

void Mark::Read(const std::string& filename, const std::string& base, int thread_id) {
    File* file;
    if (!fs_->OpenFile(filename.c_str(), O_RDONLY, &file)) {
        assert(0);
    }
    uint64_t buf_size = 1 << 20;
    uint64_t buf_base_size = buf_size / 2;
    char buf[buf_size];
    uint64_t bytes = 0;
    int32_t len = 0;
    while (1) {
        uint32_t r = buf_base_size + rand_[thread_id]->Uniform(buf_base_size);
        len = file->Read(buf, r);
        assert(len >= 0);
        if (len == 0) {
            break;
        }
        assert(base.substr(0, r) == std::string(buf, len));
        bytes += len;
    }
    assert(bytes == file_size_);
    delete file;
    read_counter_.Inc();
}

void Mark::Delete(const std::string& filename) {
    if(!fs_->DeleteFile(filename.c_str())) {
        assert(0);
    }
    del_counter_.Dec();
}

void Mark::PutWrapper(int thread_id) {
    std::string prefix = common::NumToString(thread_id);
    int name_id = 0;
    int64_t count = 0;
    std::string base;
    RandomString(&base, 1<<20, thread_id);
    while (FLAGS_count == 0 || count != FLAGS_count) {
        std::string filename = "/" + prefix + "/" + common::NumToString(name_id);
        Put(filename, base, thread_id);
        ++name_id;
        ++count;
    }
    exit_ = true;
}

void Mark::ReadWrapper(int thread_id) {
    std::string prefix = common::NumToString(thread_id);
    int name_id = 0;
    int64_t count = 0;
    std::string base;
    RandomString(&base, 1<<20, thread_id);
    while (FLAGS_count == 0 || count != FLAGS_count) {
        std::string filename = "/" + prefix + "/" + common::NumToString(name_id);
        Read(filename, base, thread_id);
        ++name_id;
        ++count;
    }
    exit_ = true;
}

void Mark::PrintStat() {
    std::cout << "Put\t" << put_counter_.Get() << "\tDel\t" << del_counter_.Get()
              << "\tRead\t" << read_counter_.Get() << std::endl;
    put_counter_.Set(0);
    del_counter_.Set(0);
    read_counter_.Set(0);
    thread_pool_->DelayTask(1000, boost::bind(&Mark::PrintStat, this));
}

void Mark::Run() {
    PrintStat();
    if (FLAGS_mode == "put") {
        for (int i = 0; i < FLAGS_thread; ++i) {
            thread_pool_->AddTask(boost::bind(&Mark::PutWrapper, this, i));
        }
    } else if (FLAGS_mode == "read") {
        for (int i = 0; i < FLAGS_thread; ++i) {
            thread_pool_->AddTask(boost::bind(&Mark::ReadWrapper, this, i));
        }
    }
    while (!exit_) {
        sleep(1);
    }
    thread_pool_->Stop(true);
    if (FLAGS_count != 0) {
        std::cout << "Total Put " << FLAGS_count * FLAGS_thread << std::endl;
    }
    if (FLAGS_count != 0) {
        std::cout << "Total Read " << FLAGS_count * FLAGS_thread << std::endl;
    }
}

void Mark::RandomString(std::string* out, int size, int rand_index) {
    out->resize(size);
    for (int i = 0; i < size; i++) {
        (*out)[i] = static_cast<char>(' ' + rand_[rand_index]->Next());
    }
}

} // namespace bfs
} // namespace baidu

int main(int argc, char* argv[]) {
    FLAGS_flagfile = "bfs.flag";
    ::google::ParseCommandLineFlags(&argc, &argv, false);
    baidu::bfs::Mark mark;
    mark.Run();
    return 0;
}
