// (c) 2023 Pascal Mehnert
// This code is licensed under BSD 2-Clause License (see LICENSE for details)

#pragma once

#include <cstddef>
#include <ostream>
#include <vector>

class InputPermutation {
public:
    using size_type = std::size_t;

    using rank_type = std::size_t;
    using index_type = std::size_t;

    explicit InputPermutation() = default;

    template <typename StringSet>
    explicit InputPermutation(StringSet const& ss) : ranks_(ss.size()),
                                                     strings_(ss.size()) {
        size_type i = 0;
        for (auto it = ss.begin(); it != ss.end(); ++it, ++i) {
            ranks_[i] = ss[it].getPEIndex();
            strings_[i] = ss[it].getStringIndex();
        }
    }

    size_type size() const { return ranks_.size(); }
    bool empty() const { return ranks_.empty(); }

    void reserve(size_type const count) {
        ranks_.reserve(count);
        strings_.reserve(count);
    }

    rank_type rank(size_type const n) const { return ranks_[n]; }
    index_type string(size_type const n) const { return strings_[n]; }

    std::vector<rank_type> const& ranks() const { return ranks_; }
    std::vector<index_type> const& strings() const { return strings_; }

    void append(InputPermutation const& other) {
        ranks_.insert(ranks_.end(), other.ranks_.begin(), other.ranks_.end());
        strings_.insert(strings_.end(), other.strings_.begin(), other.strings_.end());
    }

private:
    std::vector<rank_type> ranks_;
    std::vector<index_type> strings_;
};

inline std::ostream& operator<<(std::ostream& stream, InputPermutation const& permutation) {
    for (size_t i = 0; i != permutation.size(); ++i) {
        stream << "{" << permutation.rank(i) << ", " << permutation.string(i) << "}, ";
    }
    return stream;
}