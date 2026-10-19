// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewpcmbuffer.h"

#include <algorithm>
#include <stdexcept>

namespace Fooyin::Headphones {

PreviewPcmBuffer::PreviewPcmBuffer(size_t capacity, size_t frameBytes)
    : m_frameBytes {frameBytes}
{
    if(frameBytes == 0 || capacity < frameBytes || capacity > 8 * 1024 * 1024) {
        throw std::invalid_argument {"Invalid preview PCM capacity"};
    }
    m_storage.resize(capacity - capacity % frameBytes);
}

bool PreviewPcmBuffer::push(std::span<const std::byte> data)
{
    if(data.size() % m_frameBytes != 0) {
        throw std::invalid_argument {"Unaligned preview PCM"};
    }
    while(!data.empty()) {
        std::unique_lock lock {m_mutex};
        m_space.wait(lock, [this]() { return m_cancelled || m_finished || m_size < m_storage.size(); });
        if(m_cancelled || m_finished) {
            return false;
        }
        const auto tail  = (m_head + m_size) % m_storage.size();
        const auto count = std::min({data.size(), m_storage.size() - m_size, m_storage.size() - tail});
        std::copy_n(data.begin(), count, m_storage.begin() + static_cast<ptrdiff_t>(tail));
        m_size += count;
        data = data.subspan(count);
    }
    return true;
}

std::vector<std::byte> PreviewPcmBuffer::pop(size_t maxBytes)
{
    std::unique_lock lock {m_mutex};
    const auto count = std::min(m_size, maxBytes - maxBytes % m_frameBytes);
    std::vector<std::byte> data(count);
    const auto first = std::min(count, m_storage.size() - m_head);
    std::copy_n(m_storage.begin() + static_cast<ptrdiff_t>(m_head), first, data.begin());
    std::copy_n(m_storage.begin(), count - first, data.begin() + static_cast<ptrdiff_t>(first));
    m_head = (m_head + count) % m_storage.size();
    m_size -= count;
    lock.unlock();
    m_space.notify_one();
    return data;
}

void PreviewPcmBuffer::finish()
{
    const std::lock_guard lock {m_mutex};
    m_finished = true;
    m_space.notify_all();
}

void PreviewPcmBuffer::cancel()
{
    const std::lock_guard lock {m_mutex};
    m_cancelled = true;
    m_size      = 0;
    m_space.notify_all();
}

bool PreviewPcmBuffer::finished() const
{
    const std::lock_guard lock {m_mutex};
    return m_finished;
}

size_t PreviewPcmBuffer::size() const
{
    const std::lock_guard lock {m_mutex};
    return m_size;
}

size_t PreviewPcmBuffer::capacity() const
{
    return m_storage.size();
}

} // namespace Fooyin::Headphones
