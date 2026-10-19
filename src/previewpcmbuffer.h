// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

namespace Fooyin::Headphones {

// One generation, one producer, one consumer. Only the producer may wait.
// pop() copies under a short lock; output writes never hold that lock.
class PreviewPcmBuffer
{
public:
    PreviewPcmBuffer(size_t capacity, size_t frameBytes);
    bool push(std::span<const std::byte> data);
    [[nodiscard]] std::vector<std::byte> pop(size_t maxBytes);
    void finish();
    void cancel();
    [[nodiscard]] bool finished() const;
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t capacity() const;

private:
    const size_t m_frameBytes;
    std::vector<std::byte> m_storage;
    mutable std::mutex m_mutex;
    std::condition_variable m_space;
    size_t m_head {0};
    size_t m_size {0};
    bool m_finished {false};
    bool m_cancelled {false};
};

} // namespace Fooyin::Headphones
