/** @file SequenceSet.h
 * @brief Fixed-size linked blocks and their highest-key primary index.
 */
#ifndef ZIP_SEQUENCE_SET_H
#define ZIP_SEQUENCE_SET_H
#include "Buffer.h"
#include <filesystem>
#include <functional>
#include <map>

/** @brief Convert between a length-indicated record and a record object. */
class RecordBuffer {
public:
    /** @brief Pack a record using the Project 2 writer. */
    static std::string pack(const ZipCodeRecord& record);
    /** @brief Read a length prefix and unpack one record from a block. */
    static ZipCodeRecord unpack(std::istream& input);
};

/** @brief One active block or one available-list node. */
struct Block {
    long long previous = -1; ///< Previous active RBN, or -1.
    long long next = -1; ///< Next active or available RBN, or -1.
    std::vector<ZipCodeRecord> records; ///< Sorted records for this block only.
};

/** @brief Metadata for a version 3 blocked sequence set. */
struct BlockHeader {
    std::size_t blockSize = 512; ///< Total bytes per block.
    unsigned minimum = 50; ///< Minimum payload occupancy percentage.
    unsigned long long records = 0; ///< Live records.
    long long blocks = 0; ///< Physical blocks including available blocks.
    long long head = -1; ///< First active block.
    long long available = -1; ///< First available block.
    bool stale = false; ///< True while an update is incomplete.
    std::string indexName; ///< Companion filename in the data directory.
};

/** @brief Read and write complete fixed-size blocks. */
class BlockBuffer {
public:
    static constexpr std::size_t metadataSize = 64; ///< Reserved ASCII block metadata.
    /** @brief Return the total serialized record bytes in a block. */
    static std::size_t used(const Block& block);
    /** @brief Read a block and validate its ordered records. */
    static Block read(std::istream& file, const BlockHeader& header, long long rbn);
    /** @brief Write metadata, records, and blank padding to one block. */
    static void write(std::ostream& file, const BlockHeader& header, long long rbn, const Block& block);
};

/** @brief Search and modify a disk sequence set while keeping only its index in RAM.
 * Updates use a temporary copy and commit after validation. Run one writer at a time.
 */
class SequenceSet {
    std::filesystem::path path; ///< Data filename.
    std::fstream file; ///< Binary data stream.
    BlockHeader header; ///< Header metadata.
    std::map<std::string, long long> index; ///< Highest ZIP to RBN.
    /** @brief Persist the current metadata and index. */
    void save();
    /** @brief Get an available block or append a new one. */
    long long allocate();
    /** @brief Replace the index entry for one active block. */
    void indexBlock(long long rbn, const Block& block);
    /** @brief Choose the block which could contain a key. */
    long long locate(const std::string& zip) const;
    /** @brief Split records at a boundary that satisfies both minimum occupancies. */
    std::size_t partition(const Block& combined) const;
    /** @brief Redistribute or merge an underfull block with a neighbor. */
    void rebalance(long long rbn, Block block);
public:
    /** @brief Open an existing file and load its saved simple index. */
    explicit SequenceSet(const std::filesystem::path& filename);
    /** @brief Generate a sequence set from a Project 2 data file. */
    static void build(const std::filesystem::path& source, const std::filesystem::path& destination,
                      std::size_t blockSize = 512, unsigned minimum = 50);
    /** @brief Search one indexed block and return whether the key was found. */
    bool find(const std::string& zip, ZipCodeRecord& result);
    /** @brief Visit active records in logical order, one block at a time. */
    void each(const std::function<void(const ZipCodeRecord&)>& visit);
    /** @brief Print physical blocks, logical blocks, or the primary index. */
    void dump(const std::string& order);
    /** @brief Insert a unique record, splitting and reusing available blocks as needed. */
    void add(const ZipCodeRecord& record);
    /** @brief Delete a key, redistributing or merging as needed. */
    void remove(const std::string& zip);
    /** @brief Validate lists, index entries, counts, and occupancy against the disk. */
    void validate();
    /** @brief Flush completed modifications and clear the stale flag. */
    void finish();
    /** @brief Apply a file of additions or deletions to a temporary copy and commit it. */
    static void update(const std::filesystem::path& data, const std::filesystem::path& input, bool additions);
};
#endif
