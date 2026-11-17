/** @file SequenceSet.cpp
 * @brief Block serialization, index maintenance, and sequence set operations.
 */
#include "SequenceSet.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <limits>

namespace fs = std::filesystem;

namespace {
/** @brief Throw a descriptive error when a file invariant fails. */
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
/** @brief Compare records by their primary key. */
bool before(const ZipCodeRecord& left, const ZipCodeRecord& right) {
    return left.zip < right.zip;
}
/** @brief Obtain the companion index path. */
fs::path indexPath(const fs::path& data) {
    return data.parent_path() / (data.filename().string() + "_index.txt");
}
/** @brief Calculate the minimum occupied payload bytes. */
std::size_t minimumBytes(const BlockHeader& h) {
    return static_cast<std::size_t>(std::ceil((h.blockSize - BlockBuffer::metadataSize) * h.minimum / 100.0));
}
/** @brief Produce the exact header schema used by version 3. */
std::string headerText(const BlockHeader& h) {
    std::ostringstream out;
    out << "ZIP-BLOCKED 3\nheader_bytes=1024\nrecord_size_bytes=10\nsize_format=ASCII\n"
        << "size_includes_prefix=false\nblock_metadata_bytes=64\n"
        << "block_size=" << h.blockSize << "\nminimum_percent=" << h.minimum
        << "\nindex=" << std::quoted(h.indexName)
        << "\nindex_schema=highest_zip,zero_based_RBN;ASCII;one_entry_per_line"
        << "\nrecord_count=" << h.records << "\nblock_count=" << h.blocks
        << "\nfield_count=6\nfields=Zip Code:text(5),Place Name:CSV text,State:CSV text(2),"
           "County:CSV text,Lat:ASCII double,Long:ASCII double\nprimary_key_field=1"
        << "\navail_head=" << h.available << "\nactive_head=" << h.head
        << "\nstale=" << h.stale << '\n';
    return out.str();
}
}

void HeaderBuffer::writeBlocked(std::ostream& output, const BlockHeader& h) {
    const auto text = headerText(h);
    require(text.size() <= blockedHeaderSize, "Blocked header exceeds reserved bytes");
    output.seekp(0);
    output << text << std::string(blockedHeaderSize - text.size(), ' ');
    require(bool(output), "Cannot write blocked header");
}

BlockHeader HeaderBuffer::readBlocked(std::istream& input) {
    input.seekg(0);
    std::string bytes(blockedHeaderSize, ' ');
    require(bool(input.read(bytes.data(), bytes.size())), "Truncated blocked header");
    std::istringstream lines(bytes);
    std::string line;
    std::getline(lines, line);
    require(line == "ZIP-BLOCKED 3", "Unsupported blocked file version");
    std::map<std::string, std::string> fields;
    while (std::getline(lines, line)) {
        const auto equal = line.find('=');
        if (equal != std::string::npos) fields[line.substr(0, equal)] = line.substr(equal + 1);
    }
    BlockHeader h;
    h.blockSize = std::stoull(fields.at("block_size"));
    h.minimum = std::stoul(fields.at("minimum_percent"));
    h.records = std::stoull(fields.at("record_count"));
    h.blocks = std::stoll(fields.at("block_count"));
    h.head = std::stoll(fields.at("active_head"));
    h.available = std::stoll(fields.at("avail_head"));
    h.stale = fields.at("stale") == "1";
    std::istringstream name(fields.at("index"));
    name >> std::quoted(h.indexName);
    require(h.blockSize > BlockBuffer::metadataSize && h.blockSize <= 1048576 &&
            h.minimum > 0 && h.minimum <= 50 && h.blocks >= 0, "Invalid block settings");
    require(fs::path(h.indexName).filename() == h.indexName && !h.indexName.empty(),
            "Index filename must be a local filename");
    const auto expected = headerText(h);
    require(expected.size() <= blockedHeaderSize, "Oversized header");
    require(bytes == expected + std::string(blockedHeaderSize - expected.size(), ' '),
            "Invalid blocked header schema");
    return h;
}

std::string RecordBuffer::pack(const ZipCodeRecord& record) {
    std::ostringstream output;
    Buffer::writeRecord(output, record);
    return output.str();
}

ZipCodeRecord RecordBuffer::unpack(std::istream& input) {
    const auto size = HeaderBuffer::readSize(input, HeaderBuffer::recordSizeWidth);
    require(size > 0 && size <= 1048576, "Invalid record length inside block");
    std::string payload(size, ' ');
    require(bool(input.read(payload.data(), payload.size())), "Truncated block record");
    return Buffer::unpack(payload);
}

std::size_t BlockBuffer::used(const Block& block) {
    std::size_t total = 0;
    for (const auto& record : block.records) total += RecordBuffer::pack(record).size();
    return total;
}

Block BlockBuffer::read(std::istream& file, const BlockHeader& h, long long rbn) {
    require(rbn >= 0 && rbn < h.blocks, "Block number outside file");
    file.clear();
    file.seekg(HeaderBuffer::blockedHeaderSize + static_cast<unsigned long long>(rbn) * h.blockSize);
    std::string bytes(h.blockSize, ' ');
    require(bool(file.read(bytes.data(), bytes.size())), "Truncated block");
    std::istringstream metadata(bytes.substr(0, metadataSize));
    std::size_t count;
    Block block;
    require(bool(metadata >> count >> block.previous >> block.next), "Invalid block metadata");
    require(count <= (h.blockSize - metadataSize) / 10, "Invalid block record count");
    std::istringstream payload(bytes.substr(metadataSize));
    for (std::size_t i = 0; i < count; ++i) {
        auto record = RecordBuffer::unpack(payload);
        require(block.records.empty() || block.records.back().zip < record.zip, "Block keys out of order");
        block.records.push_back(std::move(record));
    }
    char c;
    while (payload.get(c)) require(c == ' ', "Nonblank unused block bytes");
    return block;
}

void BlockBuffer::write(std::ostream& file, const BlockHeader& h, long long rbn, const Block& block) {
    std::ostringstream metadata;
    metadata << block.records.size() << ' ' << block.previous << ' ' << block.next << '\n';
    std::string bytes = metadata.str();
    require(bytes.size() <= metadataSize, "Block metadata overflow");
    bytes.resize(metadataSize, ' ');
    for (const auto& record : block.records) bytes += RecordBuffer::pack(record);
    require(bytes.size() <= h.blockSize, "Records exceed block capacity");
    bytes.resize(h.blockSize, ' ');
    file.clear();
    file.seekp(HeaderBuffer::blockedHeaderSize + static_cast<unsigned long long>(rbn) * h.blockSize);
    file.write(bytes.data(), bytes.size());
    require(bool(file), "Cannot write block");
}

SequenceSet::SequenceSet(const fs::path& filename) : path(filename), file(filename, std::ios::in | std::ios::out | std::ios::binary) {
    require(bool(file), "Cannot open blocked data file");
    header = HeaderBuffer::readBlocked(file);
    require(!header.stale, "Data has an incomplete update; restore the saved .bak pair");
    std::ifstream input(path.parent_path() / header.indexName);
    require(bool(input), "Cannot open blocked index");
    std::string line, previous;
    while (std::getline(input, line)) {
        const auto comma = line.find(',');
        require(comma == 5, "Invalid blocked index entry");
        const auto key = line.substr(0, 5);
        std::size_t consumed;
        const auto rbn = std::stoll(line.substr(6), &consumed);
        require(consumed == line.size() - 6 && rbn >= 0 && rbn < header.blocks &&
                (previous.empty() || previous < key), "Invalid blocked index order or RBN");
        index.emplace(key, rbn);
        previous = key;
    }
    require(!input.bad(), "Failed reading blocked index");
}

void SequenceSet::save() {
    HeaderBuffer::writeBlocked(file, header);
    file.flush();
    require(bool(file), "Could not flush blocked data");
    std::ofstream output(path.parent_path() / header.indexName);
    require(bool(output), "Cannot write blocked index");
    for (const auto& entry : index) output << entry.first << ',' << entry.second << '\n';
    output.close();
    require(bool(output), "Could not finish blocked index");
}

long long SequenceSet::allocate() {
    if (header.available == -1) return header.blocks++;
    const auto rbn = header.available;
    const auto free = BlockBuffer::read(file, header, rbn);
    require(free.records.empty(), "Available list points to active block");
    header.available = free.next;
    std::cout << "Reuse available block " << rbn << "; avail head " << header.available << '\n';
    return rbn;
}

void SequenceSet::indexBlock(long long rbn, const Block& block) {
    auto old = std::find_if(index.begin(), index.end(), [rbn](const auto& entry) { return entry.second == rbn; });
    const std::string key = block.records.empty() ? "" : block.records.back().zip;
    if (old != index.end() && old->first == key) return;
    if (old != index.end()) index.erase(old);
    if (!key.empty()) index[key] = rbn;
    std::cout << "Index updated for block " << rbn << '\n';
}

long long SequenceSet::locate(const std::string& zip) const {
    if (index.empty()) return -1;
    auto entry = index.lower_bound(zip);
    if (entry == index.end()) entry = std::prev(index.end());
    return entry->second;
}

std::size_t SequenceSet::partition(const Block& combined) const {
    const auto total = BlockBuffer::used(combined);
    const auto capacity = header.blockSize - BlockBuffer::metadataSize;
    const auto minimum = minimumBytes(header);
    std::size_t left = 0, best = 0, difference = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 1; i < combined.records.size(); ++i) {
        left += RecordBuffer::pack(combined.records[i - 1]).size();
        const auto right = total - left;
        if (left > capacity || right > capacity || left < minimum || right < minimum) continue;
        const auto balance = left > right ? left - right : right - left;
        if (balance < difference) { best = i; difference = balance; }
    }
    return best;
}

void SequenceSet::add(const ZipCodeRecord& record) {
    require(RecordBuffer::pack(record).size() <= header.blockSize - BlockBuffer::metadataSize, "Record cannot fit in a block");
    auto rbn = locate(record.zip);
    Block block;
    if (rbn == -1) { rbn = allocate(); header.head = rbn; }
    else block = BlockBuffer::read(file, header, rbn);
    auto position = std::lower_bound(block.records.begin(), block.records.end(), record, before);
    require(position == block.records.end() || position->zip != record.zip, "Duplicate ZIP " + record.zip);
    block.records.insert(position, record);
    if (BlockBuffer::used(block) <= header.blockSize - BlockBuffer::metadataSize) {
        BlockBuffer::write(file, header, rbn, block);
        indexBlock(rbn, block);
    } else {
        const auto cut = partition(block);
        require(cut != 0, "Variable record sizes cannot meet the requested minimum; use a lower minimum percentage");
        Block right;
        right.records.assign(block.records.begin() + cut, block.records.end());
        block.records.erase(block.records.begin() + cut, block.records.end());
        const auto newRbn = allocate();
        right.previous = rbn;
        right.next = block.next;
        if (right.next != -1) {
            auto neighbor = BlockBuffer::read(file, header, right.next);
            neighbor.previous = newRbn;
            BlockBuffer::write(file, header, right.next, neighbor);
        }
        block.next = newRbn;
        BlockBuffer::write(file, header, rbn, block);
        BlockBuffer::write(file, header, newRbn, right);
        std::cout << "Split block " << rbn << " into " << rbn << " and " << newRbn << '\n';
        indexBlock(rbn, block);
        indexBlock(newRbn, right);
    }
    ++header.records;
}

void SequenceSet::rebalance(long long rbn, Block block) {
    if (block.next == -1 && !block.records.empty()) return;
    const auto neighborRbn = block.next != -1 ? block.next : block.previous;
    if (neighborRbn == -1) {
        header.head = -1;
        block.previous = -1;
        block.next = header.available;
        header.available = rbn;
        BlockBuffer::write(file, header, rbn, block);
        indexBlock(rbn, Block{});
        return;
    }
    const auto leftRbn = block.next != -1 ? rbn : neighborRbn;
    const auto rightRbn = block.next != -1 ? neighborRbn : rbn;
    Block left = leftRbn == rbn ? block : BlockBuffer::read(file, header, leftRbn);
    Block right = rightRbn == rbn ? block : BlockBuffer::read(file, header, rightRbn);
    left.records.insert(left.records.end(), right.records.begin(), right.records.end());
    if (BlockBuffer::used(left) <= header.blockSize - BlockBuffer::metadataSize) {
        left.next = right.next;
        if (right.next != -1) {
            auto after = BlockBuffer::read(file, header, right.next);
            after.previous = leftRbn;
            BlockBuffer::write(file, header, right.next, after);
        }
        Block available;
        available.next = header.available;
        header.available = rightRbn;
        BlockBuffer::write(file, header, rightRbn, available);
        std::cout << "Merge blocks " << leftRbn << " and " << rightRbn << "; freed " << rightRbn << '\n';
        indexBlock(rightRbn, Block{});
    } else {
        const auto cut = partition(left);
        require(cut != 0, "Cannot redistribute at the requested minimum percentage");
        right.records.assign(left.records.begin() + cut, left.records.end());
        left.records.erase(left.records.begin() + cut, left.records.end());
        BlockBuffer::write(file, header, rightRbn, right);
        std::cout << "Redistribute blocks " << leftRbn << " and " << rightRbn << '\n';
        indexBlock(rightRbn, right);
    }
    BlockBuffer::write(file, header, leftRbn, left);
    indexBlock(leftRbn, left);
}

void SequenceSet::remove(const std::string& zip) {
    const auto rbn = locate(zip);
    if (rbn == -1) { std::cout << "ZIP Code " << zip << " was not found.\n"; return; }
    auto block = BlockBuffer::read(file, header, rbn);
    auto position = std::find_if(block.records.begin(), block.records.end(), [&](const auto& r) { return r.zip == zip; });
    if (position == block.records.end()) { std::cout << "ZIP Code " << zip << " was not found.\n"; return; }
    block.records.erase(position);
    --header.records;
    BlockBuffer::write(file, header, rbn, block);
    indexBlock(rbn, block);
    if (block.records.empty() || BlockBuffer::used(block) < minimumBytes(header)) rebalance(rbn, block);
}

bool SequenceSet::find(const std::string& zip, ZipCodeRecord& result) {
    const auto rbn = locate(zip);
    if (rbn == -1) return false;
    const auto block = BlockBuffer::read(file, header, rbn);
    for (const auto& record : block.records) if (record.zip == zip) { result = record; return true; }
    return false;
}

void SequenceSet::each(const std::function<void(const ZipCodeRecord&)>& visit) {
    auto rbn = header.head;
    long long visited = 0;
    while (rbn != -1) {
        require(++visited <= header.blocks, "Cycle in active list");
        const auto block = BlockBuffer::read(file, header, rbn);
        for (const auto& record : block.records) visit(record);
        rbn = block.next;
    }
}

void SequenceSet::dump(const std::string& order) {
    require(order == "index" || order == "physical" || order == "logical", "Dump must be index, physical, or logical");
    if (order == "index") {
        for (const auto& entry : index) std::cout << entry.first << ',' << entry.second << '\n';
        return;
    }
    std::cout << "List Head: " << header.head << "\nAvail Head: " << header.available << '\n';
    long long rbn = order == "physical" ? 0 : header.head, visited = 0;
    while (rbn != -1 && rbn < header.blocks) {
        require(++visited <= header.blocks, "Cycle in block list");
        const auto block = BlockBuffer::read(file, header, rbn);
        std::cout << "Block " << rbn << " | " << block.previous << " | ";
        if (block.records.empty()) std::cout << "*available* ";
        for (const auto& record : block.records) std::cout << record.zip << ' ';
        std::cout << "| " << block.next << '\n';
        rbn = order == "physical" ? rbn + 1 : block.next;
    }
}

void SequenceSet::validate() {
    require(fs::file_size(path) == HeaderBuffer::blockedHeaderSize +
            static_cast<unsigned long long>(header.blocks) * header.blockSize, "Wrong data file size");
    std::set<long long> visited;
    unsigned long long records = 0;
    std::string lastKey;
    long long previous = -1, rbn = header.head;
    std::size_t active = 0;
    while (rbn != -1) {
        require(visited.insert(rbn).second, "Cycle in active list");
        const auto block = BlockBuffer::read(file, header, rbn);
        require(!block.records.empty() && block.previous == previous, "Broken active block link");
        require(lastKey.empty() || lastKey < block.records.front().zip, "Blocks out of key order");
        lastKey = block.records.back().zip;
        const auto entry = index.find(lastKey);
        require(entry != index.end() && entry->second == rbn, "Index does not match block");
        require(block.next == -1 || BlockBuffer::used(block) >= minimumBytes(header), "Underfull active block");
        records += block.records.size();
        ++active;
        previous = rbn;
        rbn = block.next;
    }
    require(records == header.records && active == index.size(), "Record count or index size mismatch");
    rbn = header.available;
    while (rbn != -1) {
        require(visited.insert(rbn).second, "Cycle or overlap in available list");
        const auto block = BlockBuffer::read(file, header, rbn);
        require(block.records.empty() && block.previous == -1, "Invalid available block");
        rbn = block.next;
    }
    require(visited.size() == static_cast<std::size_t>(header.blocks), "Unlinked physical block");
}

void SequenceSet::finish() {
    file.flush();
    validate();
    header.stale = false;
    save();
}

void SequenceSet::build(const fs::path& source, const fs::path& destination, std::size_t size, unsigned minimum) {
    require(!fs::exists(destination) && !fs::exists(indexPath(destination)), "Output exists; choose a new filename");
    require(size > BlockBuffer::metadataSize && size <= 1048576 && minimum > 0 && minimum <= 50, "Invalid block size or minimum");
    DataHeader oldHeader;
    Buffer input(source.string(), oldHeader);
    BlockHeader h;
    h.blockSize = size;
    h.minimum = minimum;
    h.indexName = indexPath(destination).filename().string();
    {
        std::ofstream data(destination, std::ios::binary);
        HeaderBuffer::writeBlocked(data, h);
        std::ofstream indexFile(indexPath(destination));
    }
    try {
        SequenceSet target(destination);
        // Build sequentially in source key order. Project 2 sources may have arbitrary order.
        // Keep keys and offsets only, then revisit each source record in sorted order.
        std::map<std::string, std::streamoff> offsets;
        std::ifstream raw(source, std::ios::binary);
        raw.seekg(oldHeader.headerSize);
        ZipCodeRecord record;
        while (input.next(record)) {
            const auto offset = raw.tellg();
            const auto length = HeaderBuffer::readSize(raw, HeaderBuffer::recordSizeWidth);
            raw.seekg(length, std::ios::cur);
            require(offsets.emplace(record.zip, offset).second, "Duplicate source ZIP");
        }
        // Pack full blocks directly so initial physical and logical orders are identical.
        Block block;
        long long rbn = 0;
        for (const auto& entry : offsets) {
            input.readAt(entry.second, record);
            const auto bytes = RecordBuffer::pack(record).size();
            require(bytes <= size - BlockBuffer::metadataSize, "Record exceeds block payload");
            if (!block.records.empty() && BlockBuffer::used(block) + bytes > size - BlockBuffer::metadataSize) {
                require(BlockBuffer::used(block) >= minimumBytes(h), "Record sizes cannot meet minimum capacity");
                block.next = rbn + 1;
                ++target.header.blocks;
                BlockBuffer::write(target.file, target.header, rbn, block);
                target.index[block.records.back().zip] = rbn;
                block = Block{};
                block.previous = rbn++;
            }
            block.records.push_back(record);
            ++target.header.records;
        }
        if (!block.records.empty()) {
            ++target.header.blocks;
            target.header.head = 0;
            BlockBuffer::write(target.file, target.header, rbn, block);
            target.index[block.records.back().zip] = rbn;
        }
        target.finish();
        std::cout << "Built " << target.header.records << " records in " << target.header.blocks << " blocks\n";
    } catch (...) {
        fs::remove(destination);
        fs::remove(indexPath(destination));
        throw;
    }
}

void SequenceSet::update(const fs::path& data, const fs::path& input, bool additions) {
    const fs::path temporary = data.string() + ".work";
    const fs::path backup = data.string() + ".bak";
    require(!fs::exists(temporary) && !fs::exists(indexPath(temporary)) &&
            !fs::exists(backup) && !fs::exists(indexPath(backup)), "Temporary or backup files already exist");
    // Copy streams on disk. Only the bounded stream buffers are resident in memory.
    fs::copy_file(data, temporary);
    fs::copy_file(indexPath(data), indexPath(temporary));
    try {
        {
            std::fstream temp(temporary, std::ios::in | std::ios::out | std::ios::binary);
            auto h = HeaderBuffer::readBlocked(temp);
            h.indexName = indexPath(temporary).filename().string();
            HeaderBuffer::writeBlocked(temp, h);
        }
        {
            SequenceSet set(temporary);
            set.validate();
            set.header.stale = true;
            HeaderBuffer::writeBlocked(set.file, set.header);
            if (additions) {
                Buffer records(input.string());
                ZipCodeRecord record;
                while (records.next(record)) set.add(record);
            } else {
                std::ifstream keys(input);
                require(bool(keys), "Cannot open deletion keys");
                std::string key;
                while (keys >> key) {
                    require(key.size() == 5 && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; }), "Deletion key must have five digits");
                    set.remove(key);
                }
                require(!keys.bad(), "Cannot read deletion keys");
            }
            set.finish();
            set.header.indexName = indexPath(data).filename().string();
            HeaderBuffer::writeBlocked(set.file, set.header);
        }
        fs::rename(data, backup);
        try {
            fs::rename(indexPath(data), indexPath(backup));
            fs::rename(temporary, data);
            fs::rename(indexPath(temporary), indexPath(data));
        } catch (...) {
            if (fs::exists(data)) fs::remove(data);
            fs::rename(backup, data);
            if (fs::exists(indexPath(backup))) {
                if (fs::exists(indexPath(data))) fs::remove(indexPath(data));
                fs::rename(indexPath(backup), indexPath(data));
            }
            throw;
        }
        fs::remove(backup);
        fs::remove(indexPath(backup));
    } catch (...) {
        fs::remove(temporary);
        fs::remove(indexPath(temporary));
        throw;
    }
}
