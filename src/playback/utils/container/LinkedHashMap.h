#pragma once

#include <cstddef>
#include <initializer_list>
#include <list>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace playback::utils::container {

template <class Key, class Value>
class LinkedHashMap {
private:
    using Entry     = std::pair<Key, Value>;
    using EntryList = std::list<Entry>;
    using ListIter  = typename EntryList::iterator;
    using IndexMap  = std::unordered_map<Key, ListIter>;

public:
    using key_type        = Key;
    using mapped_type     = Value;
    using value_type      = Entry;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using iterator        = typename EntryList::iterator;
    using const_iterator  = typename EntryList::const_iterator;

private:
    EntryList mEntries;
    IndexMap  mIndex;

public:
    LinkedHashMap() = default;

    LinkedHashMap(std::initializer_list<value_type> values) {
        for (auto const& [key, value] : values) {
            insert_or_assign(key, value);
        }
    }

    [[nodiscard]] bool empty() const noexcept { return mEntries.empty(); }

    [[nodiscard]] size_type size() const noexcept { return mEntries.size(); }

    void clear() noexcept {
        mIndex.clear();
        mEntries.clear();
    }

    [[nodiscard]] bool contains(Key const& key) const { return mIndex.find(key) != mIndex.end(); }

    Value& at(Key const& key) {
        auto iter = mIndex.find(key);
        if (iter == mIndex.end()) {
            throw std::out_of_range("LinkedHashMap key not found");
        }
        return iter->second->second;
    }

    Value const& at(Key const& key) const {
        auto iter = mIndex.find(key);
        if (iter == mIndex.end()) {
            throw std::out_of_range("LinkedHashMap key not found");
        }
        return iter->second->second;
    }

    Value* find(Key const& key) {
        auto iter = mIndex.find(key);
        return iter == mIndex.end() ? nullptr : &iter->second->second;
    }

    Value const* find(Key const& key) const {
        auto iter = mIndex.find(key);
        return iter == mIndex.end() ? nullptr : &iter->second->second;
    }

    Value& operator[](Key const& key) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            return iter->second->second;
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(key, listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, Value{});
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return listIter->second;
    }

    Value& operator[](Key&& key) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            return iter->second->second;
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(std::move(key), listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, Value{});
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return listIter->second;
    }

    std::pair<iterator, bool> insert(Key const& key, Value const& value) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            return {iter->second, false};
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(key, listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, value);
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return {listIter, true};
    }

    std::pair<iterator, bool> insert(Key&& key, Value&& value) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            return {iter->second, false};
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(std::move(key), listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, std::move(value));
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return {listIter, true};
    }

    std::pair<iterator, bool> insert_or_assign(Key const& key, Value const& value) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            iter->second->second = value;
            return {iter->second, false};
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(key, listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, value);
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return {listIter, true};
    }

    std::pair<iterator, bool> insert_or_assign(Key&& key, Value&& value) {
        auto iter = mIndex.find(key);
        if (iter != mIndex.end()) {
            iter->second->second = std::move(value);
            return {iter->second, false};
        }

        ListIter listIter{};
        auto [mapIter, inserted] = mIndex.try_emplace(std::move(key), listIter);
        if (inserted) {
            mEntries.emplace_back(mapIter->first, std::move(value));
            mapIter->second = std::prev(mEntries.end());
        }
        listIter = mapIter->second;
        return {listIter, true};
    }

    bool erase(Key const& key) {
        auto iter = mIndex.find(key);
        if (iter == mIndex.end()) {
            return false;
        }

        mEntries.erase(iter->second);
        mIndex.erase(iter);
        return true;
    }

    iterator erase(iterator pos) {
        mIndex.erase(pos->first);
        return mEntries.erase(pos);
    }

    iterator begin() noexcept { return mEntries.begin(); }

    iterator end() noexcept { return mEntries.end(); }

    const_iterator begin() const noexcept { return mEntries.begin(); }

    const_iterator end() const noexcept { return mEntries.end(); }

    const_iterator cbegin() const noexcept { return mEntries.cbegin(); }

    const_iterator cend() const noexcept { return mEntries.cend(); }

    Key const& firstKey() const {
        if (mEntries.empty()) {
            throw std::out_of_range("LinkedHashMap is empty");
        }
        return mEntries.front().first;
    }

    Key const& lastKey() const {
        if (mEntries.empty()) {
            throw std::out_of_range("LinkedHashMap is empty");
        }
        return mEntries.back().first;
    }
};

} // namespace playback::utils::container
