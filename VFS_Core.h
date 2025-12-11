#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <fstream>
#include <shared_mutex>
#include <cstdint>
#include <set>
#include <cstring>
#include <map>

// --- Configuration Manager ---
struct VFSConfig {
    int port = 8080;
    std::string root_password = "root";
    std::string db_path = "DataBase/server_data";
    int disk_capacity_mb = 2048;
    int preview_max_dim = 1920;

    static VFSConfig load(const std::string& filename);
};

namespace VFSConst {
    const char SIGNATURE[8] = "BRVFS"; // v0.1.5 Signature
    const int BLOCK_SIZE = 4096;
    const int PAYLOAD_SIZE = BLOCK_SIZE; 
    
    const int MAX_FILENAME = 60;
    const int32_t FAT_EOF = -1;
    
    const uint32_t JOURNAL_BLOCKS = 256;
}

// --- File System Structures ---

struct SuperBlock {
    char signature[8];
    uint64_t disk_size;
    uint32_t total_blocks;
    
    uint32_t journal_start_block;
    uint32_t bitmap_start_block;
    uint32_t root_dir_block;
    uint32_t fat_start_block;
    uint32_t data_start_block;
    
    char padding[384]; 
};

enum class LogOp : uint8_t {
    NONE = 0,
    BITMAP_UPDATE = 1,
    FAT_UPDATE = 2,
    DIR_UPDATE = 3
};

struct JournalEntry {
    bool is_valid;
    LogOp op;
    uint32_t target_block;
    uint32_t offset;
    uint32_t length;
    uint8_t data[VFSConst::BLOCK_SIZE]; 
};

struct FileEntry {
    char filename[VFSConst::MAX_FILENAME];
    uint32_t file_size;          
    uint32_t start_block_index; 
    bool is_active;
    bool is_directory;          
};

struct FileInfo {
    std::string name;
    int size;
    int block;
    std::string db_source;
    bool is_dir; 
    std::string full_path; // For search results
};

struct DiskStats {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t total_blocks;
    uint32_t used_blocks;
};

// --- Classes ---

class VirtualDisk {
public:
    std::string baseName;
    SuperBlock info;
    std::vector<std::unique_ptr<std::fstream>> streams;
    
    VirtualDisk(const std::string& name) : baseName(name) {}
    ~VirtualDisk() = default; 
};

class BlockManager {
private:
    std::shared_ptr<VirtualDisk> disk;
    std::vector<int32_t> fatCache;       
    std::vector<std::vector<uint8_t>> bitmapCache;
    int capacityMB; 
    
    mutable std::shared_mutex diskMutex; 

    std::fstream* getStreamForBlock(int globalBlockId, int& outLocalBlockId);
    bool expandStorage(); 
    
    void _writeData(int globalBlockId, const char* data, int size);
    int _readData(int globalBlockId, char* buffer, int size);

    void writeJournalEntry(const JournalEntry& entry); 
    void clearJournal();
    void recoverFromJournal();

    void runFSCK();

    void flushFatEntry(int globalBlockId, int32_t value);
    void flushBitmapBlock(int diskIdx);
    void loadMetadata();

public:
    BlockManager(std::shared_ptr<VirtualDisk> d, int capMB);
    
    void atomicWrite(int globalBlockId, const void* data, size_t size, size_t offset);

    int allocateBlock();
    void freeBlock(int globalBlockId);
    void freeBlockChain(int startBlock); 
    
    void writeData(int globalBlockId, const char* data, int size);
    int readData(int globalBlockId, char* buffer, int size);
    
    void setNextBlock(int globalCurrent, int globalNext);
    int getNextBlock(int globalCurrent);
    
    bool mount();
    static bool format(const std::string& basePath, int capacityMB);
    std::shared_ptr<VirtualDisk> getDisk() const { return disk; }
    
    DiskStats getStats();
};

class FileManager {
private:
    std::shared_ptr<BlockManager> blockMgr;
    std::recursive_mutex fileMutex; 

    int resolvePathToBlock(const std::string& path, bool createMissing);
    int findEntryInDirBlock(int dirBlock, const std::string& name);
    int createEntryInDirBlock(int dirBlock, const std::string& name, bool isDir);
    bool _deleteFileInternal(const std::string& fullPath);
    
    void _searchRecursive(int dirBlock, const std::string& currentPath, const std::string& query, std::vector<FileInfo>& results);

    std::vector<char> readChain(int startBlock, size_t knownSize = 0); 

public:
    explicit FileManager(std::shared_ptr<BlockManager> bm);

    bool importFile(const std::string& path, const std::string& content);
    bool createDirectory(const std::string& path);
    std::string getFileContent(const std::string& path);
    
    bool deletePath(const std::string& path); 
    bool movePath(const std::string& oldPath, const std::string& newPath);
    
    std::vector<FileInfo> listDirectory(const std::string& path); 
    
    std::vector<FileInfo> searchFiles(const std::string& query);
    DiskStats getDiskUsage();
};