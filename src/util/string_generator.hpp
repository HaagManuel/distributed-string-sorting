// (c) 2018 Florian Kurpicz
// (c) 2019 Matthias Schimek
// (c) 2023 Pascal Mehnert
// This code is licensed under BSD 2-Clause License (see LICENSE for details)

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "mpi/allgather.hpp"
#include "mpi/environment.hpp"
#include "mpi/read_input.hpp"
#include "strings/stringcontainer.hpp"

namespace dss_schimek {

template <typename StringSet>
class PrefixNumberStringLcpContainer : public StringLcpContainer<StringSet> {
    using Char = typename StringSet::Char;

public:
    PrefixNumberStringLcpContainer(const size_t size, const Char prefix) {
        std::vector<Char> raw_string_data;
        for (size_t i = 1; i <= size; ++i) {
            raw_string_data.emplace_back(prefix);
            size_t number_to_print = i;
            while (number_to_print > 0) {
                Char last_digit = number_to_print % 10;
                raw_string_data.emplace_back(48 + last_digit);
                number_to_print /= 10;
            }
            raw_string_data.emplace_back(Char(0));
        }
        this->update(std::move(raw_string_data));
    }

    static std::string getName() { return "PrefixStringGenerator"; }
};

// todo this should NOT be using inheritance!
template <typename StringSet>
class FileDistributer : public StringLcpContainer<StringSet> {
public:
    FileDistributer(std::string const& path)
        : StringLcpContainer<StringSet>{distribute_strings(path)} {}

    static std::string getName() { return "FileDistributer"; }
};

template <typename StringSet>
class SuffixGenerator : public StringLcpContainer<StringSet> {
    using String = typename StringSet::String;

private:
    std::vector<unsigned char> readFile(std::string const& path) {
        using dss_schimek::RawStringsLines;
        RawStringsLines data;
        const size_t fileSize = get_file_size(path);
        std::ifstream in(path);
        std::vector<unsigned char>& rawStrings = data.rawStrings;
        rawStrings.reserve(1.5 * fileSize);

        std::string line;
        data.lines = 0u;
        while (std::getline(in, line)) {
            ++data.lines;
            for (unsigned char curChar: line)
                rawStrings.push_back(curChar);
        }
        rawStrings.push_back(0);
        in.close();
        return rawStrings;
    }

    auto distributeSuffixes(std::vector<unsigned char> const& text) {
        dss_schimek::mpi::environment env;

        const size_t textSize = text.size();
        const size_t estimatedTotalCharCount = textSize * (textSize + 1) / 2 + textSize;
        const size_t estimatedCharCount = estimatedTotalCharCount / env.size();
        const size_t globalSeed = 0;
        std::mt19937 randGen(globalSeed);
        std::uniform_int_distribution<size_t> dist(0, env.size() - 1);
        std::vector<unsigned char> rawStrings;
        rawStrings.reserve(estimatedCharCount);

        size_t numGenStrings = 0;
        for (size_t i = 0; i < textSize; ++i) {
            size_t PEIndex = dist(randGen);
            if (PEIndex == env.rank()) {
                // only create your own strings
                ++numGenStrings;
                std::copy(text.begin() + i, text.end(), std::back_inserter(rawStrings));
                // Assume that text is zero terminated
            }
        }
        rawStrings.shrink_to_fit();
        return std::make_pair(std::move(rawStrings), numGenStrings);
    }

public:
    SuffixGenerator(std::string const& path) {
        std::vector<unsigned char> text = readFile(path);
        auto [rawStrings, genStrings] = distributeSuffixes(text);
        this->update(std::move(rawStrings));
        String* begin = this->strings();
        std::random_device rand;
        std::mt19937 gen{rand()};
        std::shuffle(begin, begin + genStrings, gen);
    }

    static std::string getName() { return "SuffixGenerator"; }
};

template <typename StringSet>
class DNRatioGenerator : public StringLcpContainer<StringSet> {
    using Char = typename StringSet::Char;
    using String = typename StringSet::String;

public:
    std::vector<unsigned char>
    nextChar(std::vector<unsigned char> const& lastChar, const size_t min, const size_t max) {
        std::vector<unsigned char> nextChar(lastChar.size(), min);
        int64_t i = lastChar.size() - 1;
        for (; i >= 0; --i) {
            if (lastChar[i] < max) {
                nextChar[i] = lastChar[i] + 1;
                std::copy(lastChar.begin(), lastChar.begin() + i, nextChar.begin());
                break;
            }
        }
        return nextChar;
    }

    std::tuple<std::vector<unsigned char>, size_t, size_t> getRawStringsTimoStyle(
        size_t numStrings,
        size_t desiredStringLength,
        double dToN,
        dss_schimek::mpi::environment env
    ) {
        const size_t minInternChar = 65;
        const size_t maxInternChar = 90;

        const size_t numberInternChars = maxInternChar - minInternChar + 1;
        const size_t k = std::max(
            desiredStringLength * dToN,
            std::ceil(std::log(numStrings) / std::log(numberInternChars))
        );
        const size_t stringLength = std::max(desiredStringLength, k);
        std::vector<unsigned char> rawStrings; //(numStrings * (stringLength + 1), minInternChar);
        rawStrings.reserve(numStrings * (stringLength + 1) / env.size());

        const size_t globalSeed = getSameSeedGlobally(env);
        std::mt19937 randGen(globalSeed);
        const size_t randomChar = minInternChar + (randGen() % numberInternChars);
        std::uniform_int_distribution<size_t> dist(0, env.size() - 1);

        size_t numGenStrings = 0;
        size_t curOffset = 0;
        for (size_t i = 0; i < numStrings; ++i) {
            size_t PEIndex = dist(randGen);
            if (PEIndex == env.rank()) {
                // only create your own strings
                ++numGenStrings;
                size_t curIndex = i;
                for (size_t j = 0; j < k; ++j) {
                    rawStrings.push_back(minInternChar);
                }
                for (size_t j = 0; j < k; ++j) {
                    if (curIndex == 0)
                        break;
                    rawStrings[curOffset + k - 1 - j] =
                        minInternChar + (curIndex % numberInternChars);
                    curIndex /= numberInternChars;
                }
                for (size_t j = k; j < stringLength; ++j)
                    rawStrings.push_back(randomChar);
                rawStrings.push_back(0);
                curOffset += stringLength + 1;
            }
        }
        rawStrings.resize(curOffset);

        return make_tuple(rawStrings, numGenStrings, stringLength);
    }

    std::tuple<std::vector<unsigned char>, size_t, size_t> getRawStrings(
        size_t numStrings,
        size_t desiredStringLength,
        double dToN,
        dss_schimek::mpi::environment env = dss_schimek::mpi::environment()
    ) {
        std::vector<unsigned char> rawStrings;
        const size_t minInternChar = 65;
        const size_t maxInternChar = 90;
        const size_t numberInternChars = maxInternChar - minInternChar + 1;
        const size_t charLength = std::ceil(0.5 * std::log(numStrings) / log(numberInternChars));
        const size_t commonPrefixLength =
            std::max(static_cast<int64_t>(desiredStringLength * dToN - 2 * charLength), 0l);
        const size_t paddingLength = std::max(
            static_cast<int64_t>(desiredStringLength - (commonPrefixLength + 2 * charLength)),
            0l
        );
        const size_t stringLength = commonPrefixLength + 2 * charLength + paddingLength;
        const size_t wrap =
            std::pow(static_cast<double>(maxInternChar - minInternChar + 1), charLength);

        std::mt19937 randGen(getSameSeedGlobally());
        std::uniform_int_distribution<size_t> dist(0, env.size() - 1);

        std::vector<unsigned char> curFirstChar(charLength, minInternChar);
        std::vector<unsigned char> curSecondChar(charLength, minInternChar);
        size_t numGenStrings = 0;

        for (size_t i = 0; i < numStrings; ++i) {
            size_t PEIndex = dist(randGen);
            if (PEIndex == env.rank()) {
                ++numGenStrings;
                for (size_t j = 0; j < commonPrefixLength; ++j)
                    rawStrings.emplace_back(maxInternChar);

                for (size_t j = 0; j < charLength; ++j)
                    rawStrings.emplace_back(curFirstChar[j]);
                for (size_t j = 0; j < charLength; ++j)
                    rawStrings.emplace_back(curSecondChar[j]);

                for (size_t j = 0; j < paddingLength; ++j)
                    rawStrings.emplace_back(maxInternChar);
                rawStrings.emplace_back(0);
            }
            if ((i + 1) % wrap == 0)
                curFirstChar = nextChar(curFirstChar, minInternChar, maxInternChar);
            curSecondChar = nextChar(curSecondChar, minInternChar, maxInternChar);
        }
        return make_tuple(rawStrings, numGenStrings, stringLength);
    }

    size_t getSameSeedGlobally(dss_schimek::mpi::environment env) {
        size_t seed = 0;
        if (env.rank() == 0) {
            std::random_device rand_seed;
            seed = rand_seed();
        }
        return dss_schimek::mpi::broadcast(seed, env);
    }

public:
    DNRatioGenerator(size_t size, size_t stringLength = 40, double dToN = 0.5) {
        // todo pass as parameter
        mpi::environment env;
        size_t genStrings = 0;
        size_t genStringLength = 0;
        std::vector<Char> rawStrings;
        std::tie(rawStrings, genStrings, genStringLength) =
            getRawStringsTimoStyle(size, stringLength, dToN, env);
        this->update(std::move(rawStrings));
        String* begin = this->strings();
        std::random_device rand;
        std::mt19937 gen(rand());
        std::shuffle(begin, begin + genStrings, gen);
        this->orderRawStrings();
    }

    static std::string getName() { return "DNRatioGenerator"; }
};

template <typename StringSet>
class RandomStringLcpContainer : public StringLcpContainer<StringSet> {
    using Char = typename StringSet::Char;

public:
    RandomStringLcpContainer(
        const size_t size, const size_t min_length = 10, const size_t max_length = 20
    ) {
        dss_schimek::mpi::environment env;
        std::vector<Char> random_raw_string_data;
        std::random_device rand_seed;
        std::mt19937 rand_gen(rand_seed());
        std::uniform_int_distribution<Char> char_dis(65, 90);

        size_t effectiveSize = size / env.size();
        std::cout << "effective size: " << effectiveSize << std::endl;
        std::uniform_int_distribution<size_t> length_dis(min_length, max_length);
        random_raw_string_data.reserve(effectiveSize + 1);
        for (size_t i = 0; i < effectiveSize; ++i) {
            size_t length = length_dis(rand_gen);
            for (size_t j = 0; j < length; ++j)
                random_raw_string_data.emplace_back(char_dis(rand_gen));
            random_raw_string_data.emplace_back(Char(0));
        }
        this->update(std::move(random_raw_string_data));
    }

    static std::string getName() { return "RandomStringGenerator"; }
};

template <typename StringSet>
class SkewedRandomStringLcpContainer : public StringLcpContainer<StringSet> {
    using Char = typename StringSet::Char;

public:
    size_t getSameSeedGlobally(dss_schimek::mpi::environment env) {
        size_t seed = 0;
        if (env.rank() == 0) {
            std::random_device rand_seed;
            seed = rand_seed();
        }
        return dss_schimek::mpi::broadcast(seed, env);
    }
    SkewedRandomStringLcpContainer(
        const size_t size, const size_t min_length = 100, const size_t max_length = 200
    ) {
        std::vector<Char> random_raw_string_data;
        std::random_device rand_seed;
        dss_schimek::mpi::environment env;
        const size_t globalSeed = 0;       // getSameSeedGlobally();
        std::mt19937 rand_gen(globalSeed); // rand_seed());
        std::uniform_int_distribution<Char> small_char_dis(65, 70);
        std::uniform_int_distribution<Char> char_dis(65, 90);

        std::uniform_int_distribution<size_t> dist(0, env.size() - 1);
        std::uniform_int_distribution<size_t> normal_length_dis(min_length, max_length);
        std::uniform_int_distribution<size_t> large_length_dis(min_length + 100, max_length + 100);

        const size_t numLongStrings = size / 4;
        const size_t numSmallStrings = size - numLongStrings;
        std::size_t curChars = 0;

        random_raw_string_data.reserve(size + 1);
        for (size_t i = 0; i < numLongStrings; ++i) {
            const size_t PEIndex = dist(rand_gen);
            // std::cout << "rank: " << env.rank() << " PEIndex: " << PEIndex <<
            // std::endl;
            bool const takeValue = (PEIndex == env.rank());
            size_t length = large_length_dis(rand_gen);
            for (size_t j = 0; j < length; ++j) {
                unsigned char generatedChar = small_char_dis(rand_gen);
                if (takeValue) {
                    random_raw_string_data.push_back(generatedChar);
                }
            }
            if (takeValue) {
                // std::cout << "taken" << std::endl;
                random_raw_string_data.emplace_back(Char(0));
                curChars += length + 1;
            }
        }

        for (size_t i = 0; i < numSmallStrings; ++i) {
            const size_t PEIndex = dist(rand_gen);
            // std::cout << "rank: " << env.rank() << " PEIndex: " << PEIndex <<
            // std::endl;
            bool const takeValue = (PEIndex == env.rank());
            size_t length = normal_length_dis(rand_gen);
            for (size_t j = 0; j < length; ++j) {
                unsigned char const generatedChar = char_dis(rand_gen);
                if (takeValue) {
                    random_raw_string_data.push_back(generatedChar);
                }
            }
            if (takeValue) {
                // std::cout << "taken" << std::endl;
                random_raw_string_data.push_back(Char(0));
                curChars += length + 1;
            }
        }
        //});
        random_raw_string_data.resize(curChars);
        this->update(std::move(random_raw_string_data));
    }

    static std::string getName() { return "SkewedStringGenerator"; }
};

template <typename StringSet>
class SkewedDNRatioGenerator : public StringLcpContainer<StringSet> {
    using String = typename StringSet::String;
    std::tuple<std::vector<unsigned char>, size_t, size_t>

    getRawStringsTimoStyle(
        size_t numStrings,
        size_t desiredStringLength,
        double dToN,
        dss_schimek::mpi::environment env = dss_schimek::mpi::environment()
    ) {
        const size_t minInternChar = 65;
        const size_t maxInternChar = 90;

        const size_t numberInternChars = maxInternChar - minInternChar + 1;
        const size_t k = std::max(
            desiredStringLength * dToN,
            std::ceil(std::log(numStrings) / std::log(numberInternChars))
        );
        const size_t stringLength = std::max(desiredStringLength, k);
        std::vector<unsigned char> rawStrings; //(numStrings * (stringLength + 1), minInternChar);
        rawStrings.reserve(numStrings * (stringLength + 1) / env.size());

        const size_t globalSeed = 0;
        std::mt19937 randGen(globalSeed);
        const size_t randomChar = minInternChar + (randGen() % numberInternChars);
        std::uniform_int_distribution<size_t> dist(0, env.size() - 1);

        size_t numGenStrings = 0;
        size_t curOffset = 0;
        const size_t longStringMaxIndex = 0.2 * numStrings;
        const size_t longStringLength = stringLength * 3;
        for (size_t i = 0; i < numStrings; ++i) {
            size_t PEIndex = dist(randGen);
            if (PEIndex == env.rank()) {
                // only create your own strings
                ++numGenStrings;
                size_t curIndex = i;
                for (size_t j = 0; j < k; ++j) {
                    rawStrings.push_back(minInternChar);
                }
                for (size_t j = 0; j < k; ++j) {
                    if (curIndex == 0)
                        break;
                    rawStrings[curOffset + k - 1 - j] =
                        minInternChar + (curIndex % numberInternChars);
                    curIndex /= numberInternChars;
                }
                for (size_t j = k; j < stringLength; ++j)
                    rawStrings.push_back(randomChar);
                if (i < longStringMaxIndex) {
                    for (size_t j = k; j < longStringLength + k; ++j)
                        rawStrings.push_back(randomChar);
                    curOffset += longStringLength;
                }
                rawStrings.push_back(0);
                curOffset += stringLength + 1;
            }
        }
        rawStrings.resize(curOffset);

        return make_tuple(rawStrings, numGenStrings, stringLength);
    }

public:
    SkewedDNRatioGenerator(
        const size_t size, const size_t stringLength = 40, double const dToN = 0.5
    ) {
        size_t genStrings = 0;
        size_t genStringLength = 0;
        std::vector<unsigned char> rawStrings;
        std::tie(rawStrings, genStrings, genStringLength) =
            getRawStringsTimoStyle(size, stringLength, dToN);
        this->update(std::move(rawStrings));
        String* begin = this->strings();
        std::random_device rand;
        std::mt19937 gen(rand());
        std::shuffle(begin, begin + genStrings, gen);
        this->orderRawStrings();
    }

    static std::string getName() { return "SkewedDNRatioGenerator"; }
};
} // namespace dss_schimek