// SPDX-License-Identifier: GPL-3.0-or-later
#include "SegmentBackendInProcess.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>

namespace scvb
{

namespace
{
// 64 字节对齐的字节缓冲(vector<unsigned char> 只有 1 字节对齐,无法承载段内 alignas(64) 结构)。
// 内核零初始化语义(等价 CreateFileMapping 的 PAGE_READWRITE 零填充)。
struct AlignedBuffer
{
    unsigned char* data = nullptr;
    std::size_t size = 0;

    explicit AlignedBuffer(std::size_t n)
        : size(n), data(static_cast<unsigned char*>(::operator new(n, std::align_val_t(64))))
    {
        std::memset(data, 0, n);
    }
    ~AlignedBuffer() { ::operator delete(data, std::align_val_t(64)); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
};

std::mutex& segmentsMutex()
{
    static std::mutex m;
    return m;
}

std::map<std::wstring, std::shared_ptr<AlignedBuffer>>& segments()
{
    static std::map<std::wstring, std::shared_ptr<AlignedBuffer>> m;
    return m;
}
} // namespace

InitResult SegmentBackendInProcess::createOrOpen(const std::wstring& name, std::size_t size, SegmentView& view)
{
    view.reset();
    if (size == 0)
    {
        return InitResult::kFailed;
    }

    std::lock_guard<std::mutex> lock(segmentsMutex());
    auto& map = segments();
    auto it = map.find(name);
    if (it != map.end())
    {
        // v6 语义:同名已存在 → 复用既有缓冲,忽略请求大小,回填真实段大小。
        view.base = it->second->data;
        view.size = it->second->size;
        view.created = false;
        return InitResult::kOk;
    }

    auto buf = std::make_shared<AlignedBuffer>(size);
    map[name] = buf;
    view.base = buf->data;
    view.size = size;
    view.created = true;
    return InitResult::kOk;
}

InitResult SegmentBackendInProcess::openExisting(const std::wstring& name, SegmentView& view)
{
    view.reset();
    std::lock_guard<std::mutex> lock(segmentsMutex());
    auto& map = segments();
    auto it = map.find(name);
    if (it == map.end())
    {
        return InitResult::kFailed;
    }
    view.base = it->second->data;
    view.size = it->second->size;
    view.created = false;
    return InitResult::kOk;
}

void SegmentBackendInProcess::resetAll()
{
    std::lock_guard<std::mutex> lock(segmentsMutex());
    segments().clear();
}

} // namespace scvb
