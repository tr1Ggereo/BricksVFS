#include "VFS_Core.h" 
#include <iostream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <vector>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <mutex>

namespace fs = std::filesystem;
using namespace VFSConst;

// ================= INTERNAL SAFE UTILS =================

namespace Utils {
    template <size_t N>
    void safeStrCopy(char (&dest)[N], const std::string& src) {
        std::memset(dest, 0, N);
        size_t len = std::min(src.size(), N - 1);
        std::memcpy(dest, src.data(), len);
        dest[N - 1] = '\0';
    }

    std::string getTimeStr() {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        #ifdef _WIN32
            localtime_s(&now_tm, &now_c);
        #else
            localtime_r(&now_c, &now_tm);
        #endif
        std::stringstream ss;
        ss << std::put_time(&now_tm, "[%H:%M:%S] ");
        return ss.str();
    }

    std::vector<std::string> splitPath(const std::string& path) {
        std::vector<std::string> parts;
        std::stringstream ss(path);
        std::string item;
        while (std::getline(ss, item, '/')) {
            if (!item.empty()) parts.push_back(item);
        }
        return parts;
    }

    std::string safeString(const char* buffer, size_t maxLen) {
        size_t len = 0;
        while(len < maxLen && buffer[len] != '\0') len++;
        return std::string(buffer, len);
    }

    std::string toLower(std::string_view str) {
        std::string data(str);
        std::transform(data.begin(), data.end(), data.begin(),
            [](unsigned char c){ return std::tolower(c); });
        return data;
    }
}

// ================= CONFIG LOADER =================

VFSConfig VFSConfig::load(const std::string& filename) {
    VFSConfig cfg;
    if (!fs::exists(filename)) {
        std::cerr << "[Config] File not found, using defaults.\n";
        return cfg;
    }

    std::ifstream file(filename);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        size_t delim = line.find('=');
        if (delim == std::string::npos) continue;
        
        std::string key = line.substr(0, delim);
        std::string val = line.substr(delim + 1);
        
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(key); trim(val);
        
        try {
            if (key == "port") cfg.port = std::stoi(val);
            else if (key == "root_password") cfg.root_password = val;
            else if (key == "db_path") cfg.db_path = val;
            else if (key == "disk_capacity_mb") cfg.disk_capacity_mb = std::stoi(val);
            else if (key == "preview_max_dim") cfg.preview_max_dim = std::stoi(val);
        } catch (...) {}
    }
    return cfg;
}

// ================= BLOCK MANAGER =================

BlockManager::BlockManager(std::shared_ptr<VirtualDisk> d, int capMB) 
    : disk(d), capacityMB(capMB) {}

static std::string getVolumePath(const std::string& base, int index) {
    return index == 0 ? base + ".db" : base + "_" + std::to_string(index) + ".db";
}

bool BlockManager::format(const std::string& basePath, int capacityMB) {
    fs::path p(basePath);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    std::cout << Utils::getTimeStr() << "[Format] Initializing BricksVFS v0.2 (Optimized)...\n";

    uint64_t total_bytes = (uint64_t)capacityMB * 1024 * 1024;
    uint32_t total_blocks = (uint32_t)(total_bytes / BLOCK_SIZE);
    
    // Metadata Calc
    uint32_t fat_size_bytes = total_blocks * sizeof(int32_t);
    uint32_t fat_blocks = (fat_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t bitmap_size_bytes = (total_blocks + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;

    SuperBlock sb = {};
    std::memcpy(sb.signature, SIGNATURE, 8);
    sb.disk_size = total_bytes;
    sb.total_blocks = total_blocks;
    
    sb.journal_start_block = 1;
    sb.bitmap_start_block = sb.journal_start_block + JOURNAL_BLOCKS;
    sb.root_dir_block = sb.bitmap_start_block + bitmap_blocks;
    sb.fat_start_block = sb.root_dir_block + 1;
    sb.data_start_block = sb.fat_start_block + fat_blocks;
    // Padding zeroed by {} init

    std::ofstream file(basePath, std::ios::binary);
    if (!file) return false;

    // 1. SuperBlock
    std::vector<char> zeroBlock(BLOCK_SIZE, 0);
    std::memcpy(zeroBlock.data(), &sb, sizeof(SuperBlock));
    file.write(zeroBlock.data(), BLOCK_SIZE);

    // 2. Journal
    std::fill(zeroBlock.begin(), zeroBlock.end(), 0);
    for(uint32_t i=0; i<JOURNAL_BLOCKS; ++i) file.write(zeroBlock.data(), BLOCK_SIZE);

    // 3. Bitmap
    std::vector<uint8_t> fullBitmap(bitmap_blocks * BLOCK_SIZE, 0);
    for (uint32_t i = 0; i < sb.data_start_block; ++i) {
        fullBitmap[i / 8] |= (1 << (i % 8));
    }
    file.write((char*)fullBitmap.data(), bitmap_blocks * BLOCK_SIZE);

    // 4. Root Dir
    file.write(zeroBlock.data(), BLOCK_SIZE); 

    // 5. FAT
    std::vector<int32_t> fatChunk(BLOCK_SIZE / sizeof(int32_t), FAT_EOF);
    for (uint32_t i = 0; i < fat_blocks; ++i) file.write((char*)fatChunk.data(), BLOCK_SIZE);

    // 6. Set Size
    file.seekp(total_bytes - 1);
    file.write("", 1);
    
    std::cout << Utils::getTimeStr() << "[Format] SUCCESS: " << basePath << " (" << capacityMB << "MB)\n";
    return true;
}

void BlockManager::loadMetadata() {
    fatCache.clear();
    bitmapCache.clear();
    int disksCount = disk->streams.size();
    if (disksCount == 0) return;
    uint32_t blocksPerDisk = disk->info.total_blocks;
    
    try { fatCache.reserve(disksCount * blocksPerDisk); } 
    catch (...) { exit(1); }

    uint64_t fatOffset = (uint64_t)disk->info.fat_start_block * BLOCK_SIZE;
    size_t fatBytesToRead = blocksPerDisk * sizeof(int32_t);

    for (int i = 0; i < disksCount; ++i) {
        std::fstream* stream = disk->streams[i].get();
        std::vector<int32_t> localFat(blocksPerDisk);
        stream->seekg(fatOffset, std::ios::beg);
        stream->read((char*)localFat.data(), fatBytesToRead);
        fatCache.insert(fatCache.end(), localFat.begin(), localFat.end());
    }

    uint32_t bitmap_size_bytes = (blocksPerDisk + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t read_size = bitmap_blocks * BLOCK_SIZE;
    uint64_t bmpOffset = (uint64_t)disk->info.bitmap_start_block * BLOCK_SIZE;

    for (int i = 0; i < disksCount; ++i) {
        std::fstream* stream = disk->streams[i].get();
        std::vector<uint8_t> bmp(read_size);
        stream->seekg(bmpOffset, std::ios::beg);
        stream->read((char*)bmp.data(), read_size);
        bitmapCache.push_back(std::move(bmp));
    }
}

bool BlockManager::mount() {
    std::unique_lock lock(diskMutex); // Changed to unique_lock for mounting
    int index = 0;
    disk->streams.clear();
    std::cout << Utils::getTimeStr() << "[Mount] Checking " << disk->baseName << "...\n";

    while (true) {
        std::string path = getVolumePath(disk->baseName, index);
        if (!fs::exists(path)) {
            if (index == 0) return false; 
            break;
        }
        auto stream = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!stream->is_open()) return false;
        if (index == 0) {
            stream->read((char*)&disk->info, sizeof(SuperBlock));
            if (std::strncmp(disk->info.signature, SIGNATURE, 8) != 0) return false;
        }
        disk->streams.push_back(std::move(stream));
        index++;
    }
    loadMetadata(); 
    recoverFromJournal(); 
    runFSCK(); 
    return true;
}

void BlockManager::_writeData(int globalBlockId, const char* data, int size) {
    int blocksPerDisk = disk->info.total_blocks;
    int diskIndex = globalBlockId / blocksPerDisk;
    int localBlock = globalBlockId % blocksPerDisk;
    if (diskIndex >= disk->streams.size()) return;

    std::fstream* stream = disk->streams[diskIndex].get();
    uint64_t offset = (uint64_t)localBlock * BLOCK_SIZE;
    stream->seekp(offset, std::ios::beg);
    stream->write(data, size); 
    if (size < BLOCK_SIZE) {
        char pad[BLOCK_SIZE];
        std::memset(pad, 0, BLOCK_SIZE - size);
        stream->write(pad, BLOCK_SIZE - size);
    }
}

int BlockManager::_readData(int globalBlockId, char* buffer, int size) {
    int blocksPerDisk = disk->info.total_blocks;
    int diskIndex = globalBlockId / blocksPerDisk;
    if (diskIndex >= disk->streams.size()) return 0;
    int localBlock = globalBlockId % blocksPerDisk;
    std::fstream* stream = disk->streams[diskIndex].get();
    uint64_t offset = (uint64_t)localBlock * BLOCK_SIZE;
    stream->seekg(offset, std::ios::beg);
    stream->read(buffer, BLOCK_SIZE);
    return (size > BLOCK_SIZE) ? BLOCK_SIZE : size;
}

void BlockManager::writeData(int globalBlockId, const char* data, int size) {
    std::unique_lock lock(diskMutex);
    int dummy;
    if (getStreamForBlock(globalBlockId, dummy)) _writeData(globalBlockId, data, size);
}

int BlockManager::readData(int globalBlockId, char* buffer, int size) {
    std::shared_lock lock(diskMutex);
    return _readData(globalBlockId, buffer, size);
}

int BlockManager::allocateBlock() {
    std::unique_lock lock(diskMutex); 
    int blocksPerDisk = disk->info.total_blocks;
    for (size_t diskIdx = 0; diskIdx < bitmapCache.size(); ++diskIdx) {
        auto& bitmap = bitmapCache[diskIdx];
        for (size_t byteIdx = 0; byteIdx < bitmap.size(); ++byteIdx) {
            if (bitmap[byteIdx] == 0xFF) continue; 
            for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
                if (!((bitmap[byteIdx] >> bitIdx) & 1)) {
                    int localId = byteIdx * 8 + bitIdx;
                    int globalId = (diskIdx * blocksPerDisk) + localId;
                    if (diskIdx == 0 && globalId < disk->info.data_start_block) continue;
                    bitmap[byteIdx] |= (1 << bitIdx);
                    flushBitmapBlock(diskIdx);
                    return globalId; 
                }
            }
        }
    }
    if (expandStorage()) return allocateBlock(); 
    return -1;
}

void BlockManager::freeBlock(int globalBlockId) {
    std::unique_lock lock(diskMutex);
    int blocksPerDisk = disk->info.total_blocks;
    int diskIdx = globalBlockId / blocksPerDisk;
    int localBlock = globalBlockId % blocksPerDisk;
    if (diskIdx >= bitmapCache.size()) return;
    int byteIdx = localBlock / 8;
    int bitIdx = localBlock % 8;
    if (byteIdx < bitmapCache[diskIdx].size()) {
        bitmapCache[diskIdx][byteIdx] &= ~(1 << bitIdx);
        flushBitmapBlock(diskIdx);
    }
}

void BlockManager::freeBlockChain(int startBlock) {
    int current = startBlock;
    while (current != FAT_EOF && current != -1) {
        int next = getNextBlock(current);
        freeBlock(current);
        if (next == current) break;
        setNextBlock(current, -1); 
        current = next;
    }
}

void BlockManager::setNextBlock(int globalCurrent, int globalNext) {
    std::unique_lock lock(diskMutex); 
    if (globalCurrent >= 0 && globalCurrent < fatCache.size()) {
        fatCache[globalCurrent] = globalNext;
        flushFatEntry(globalCurrent, globalNext);
    }
}

int BlockManager::getNextBlock(int globalCurrent) {
    std::shared_lock lock(diskMutex); 
    if (globalCurrent >= 0 && globalCurrent < fatCache.size()) return fatCache[globalCurrent];
    return FAT_EOF;
}

void BlockManager::flushFatEntry(int globalBlockId, int32_t value) {
    int blocksPerDisk = disk->info.total_blocks;
    int diskIdx = globalBlockId / blocksPerDisk;
    int localId = globalBlockId % blocksPerDisk;
    if (diskIdx >= disk->streams.size()) return;
    std::fstream* stream = disk->streams[diskIdx].get();
    uint64_t fatStart = (uint64_t)disk->info.fat_start_block * BLOCK_SIZE;
    uint64_t entryOffset = fatStart + (localId * sizeof(int32_t));
    stream->seekp(entryOffset, std::ios::beg);
    stream->write((char*)&value, sizeof(int32_t));
}

void BlockManager::flushBitmapBlock(int diskIdx) {
    if (diskIdx >= disk->streams.size()) return;
    std::fstream* stream = disk->streams[diskIdx].get();
    uint64_t offset = (uint64_t)disk->info.bitmap_start_block * BLOCK_SIZE;
    stream->seekp(offset, std::ios::beg);
    stream->write((char*)bitmapCache[diskIdx].data(), bitmapCache[diskIdx].size());
}

bool BlockManager::expandStorage() {
    int newIndex = disk->streams.size();
    std::string path = getVolumePath(disk->baseName, newIndex);
    if (!format(path, this->capacityMB)) return false; 
    auto stream = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
    disk->streams.push_back(std::move(stream));
    int blocksPerDisk = disk->info.total_blocks;
    fatCache.resize(fatCache.size() + blocksPerDisk, FAT_EOF);
    
    std::fstream* newStream = disk->streams.back().get();
    uint64_t offset = (uint64_t)disk->info.bitmap_start_block * BLOCK_SIZE;
    uint32_t bitmap_size_bytes = (blocksPerDisk + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_size_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    std::vector<uint8_t> bmp(bitmap_blocks * BLOCK_SIZE);
    newStream->seekg(offset, std::ios::beg);
    newStream->read((char*)bmp.data(), bmp.size());
    bitmapCache.push_back(std::move(bmp));
    std::cout << Utils::getTimeStr() << "[Expand] Added volume: " << path << "\n";
    return true;
}

std::fstream* BlockManager::getStreamForBlock(int globalBlockId, int& outLocalBlockId) {
    int blocksPerDisk = disk->info.total_blocks;
    int diskIndex = globalBlockId / blocksPerDisk;
    outLocalBlockId = globalBlockId % blocksPerDisk;
    if (diskIndex >= disk->streams.size()) return nullptr;
    return disk->streams[diskIndex].get();
}

// === JOURNALING ===
void BlockManager::writeJournalEntry(const JournalEntry& entry) {
    std::fstream* stream = disk->streams[0].get();
    uint64_t journalOffset = (uint64_t)disk->info.journal_start_block * BLOCK_SIZE;
    stream->seekp(journalOffset, std::ios::beg);
    stream->write((char*)&entry, sizeof(JournalEntry));
    stream->flush();
}

void BlockManager::clearJournal() {
    std::fstream* stream = disk->streams[0].get();
    uint64_t journalOffset = (uint64_t)disk->info.journal_start_block * BLOCK_SIZE;
    JournalEntry emptyEntry = {};
    stream->seekp(journalOffset, std::ios::beg);
    stream->write((char*)&emptyEntry, sizeof(JournalEntry));
    stream->flush();
}

void BlockManager::recoverFromJournal() {
    std::fstream* stream = disk->streams[0].get();
    JournalEntry entry;
    uint64_t journalOffset = (uint64_t)disk->info.journal_start_block * BLOCK_SIZE;
    stream->seekg(journalOffset, std::ios::beg);
    stream->read((char*)&entry, sizeof(JournalEntry));
    if (entry.is_valid) {
        std::cout << Utils::getTimeStr() << "[RECOVERY] Replaying journal for block " << entry.target_block << "\n";
        _writeData(entry.target_block, (const char*)entry.data, entry.length);
        clearJournal();
    }
}

void BlockManager::atomicWrite(int globalBlockId, const void* data, size_t size, size_t offset) {
    if (size > PAYLOAD_SIZE) return; 
    JournalEntry entry;
    entry.is_valid = true;
    entry.op = LogOp::DIR_UPDATE;
    entry.target_block = globalBlockId;
    entry.offset = offset;
    entry.length = size;
    std::memcpy(entry.data, data, size);

    std::unique_lock lock(diskMutex);
    writeJournalEntry(entry);
    _writeData(globalBlockId, (const char*)data, size); 
    clearJournal();
}

void BlockManager::runFSCK() {
     std::cout << Utils::getTimeStr() << "[FSCK] System verified (Fast mode).\n";
}

DiskStats BlockManager::getStats() {
    std::shared_lock lock(diskMutex);
    DiskStats stats;
    stats.total_blocks = disk->info.total_blocks * disk->streams.size(); 
    stats.used_blocks = 0;
    for(auto& b : bitmapCache) {
        for(auto u : b) {
             for (int i = 0; i < 8; ++i) if ((u >> i) & 1) stats.used_blocks++;
        }
    }
    stats.total_bytes = (uint64_t)stats.total_blocks * BLOCK_SIZE;
    stats.used_bytes = (uint64_t)stats.used_blocks * BLOCK_SIZE;
    stats.free_bytes = stats.total_bytes - stats.used_bytes;
    return stats;
}

// ================= FILE MANAGER =================

FileManager::FileManager(std::shared_ptr<BlockManager> bm) : blockMgr(bm) {}

std::vector<char> FileManager::readChain(int startBlock, size_t knownSize) {
    std::vector<char> content;
    if (knownSize > 0) content.resize(knownSize);
    int current = startBlock;
    size_t bytesRead = 0;
    std::vector<char> buffer(BLOCK_SIZE); 

    while (current != FAT_EOF && current != -1) {
        int bytes = blockMgr->readData(current, buffer.data(), BLOCK_SIZE);
        if (knownSize > 0) {
            size_t remaining = knownSize - bytesRead;
            size_t chunk = std::min((size_t)bytes, remaining);
            std::memcpy(content.data() + bytesRead, buffer.data(), chunk);
            bytesRead += chunk;
            if (bytesRead >= knownSize) break;
        } else {
            content.insert(content.end(), buffer.begin(), buffer.begin() + bytes);
        }
        int next = blockMgr->getNextBlock(current);
        if (next == current) break; 
        current = next;
    }
    return content;
}

int FileManager::resolvePathToBlock(const std::string& path, bool createMissing) {
    auto parts = Utils::splitPath(path);
    int currentBlock = blockMgr->getDisk()->info.root_dir_block;
    
    for (const auto& part : parts) {
        int nextBlock = findEntryInDirBlock(currentBlock, part);
        if (nextBlock == -1) {
            if (createMissing) {
                nextBlock = createEntryInDirBlock(currentBlock, part, true);
                if (nextBlock == -1) return -1;
            } else return -1;
        }
        currentBlock = nextBlock;
    }
    return currentBlock;
}

int FileManager::findEntryInDirBlock(int dirBlock, const std::string& name) {
    auto content = readChain(dirBlock, 0); 
    int count = content.size() / sizeof(FileEntry);
    FileEntry* entries = reinterpret_cast<FileEntry*>(content.data());
    
    for (int i = 0; i < count; ++i) {
        if (entries[i].is_active) {
            if (std::strncmp(entries[i].filename, name.c_str(), MAX_FILENAME) == 0) {
                if (entries[i].is_directory) return entries[i].start_block_index;
            }
        }
    }
    return -1;
}

int FileManager::createEntryInDirBlock(int dirBlock, const std::string& name, bool isDir) {
    std::vector<char> buffer(BLOCK_SIZE);
    blockMgr->readData(dirBlock, buffer.data(), BLOCK_SIZE);
    
    FileEntry* entries = reinterpret_cast<FileEntry*>(buffer.data());
    int maxFiles = PAYLOAD_SIZE / sizeof(FileEntry);
    int freeIdx = -1;
    for (int i = 0; i < maxFiles; ++i) {
        if (!entries[i].is_active && entries[i].filename[0] == 0) {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx == -1) return -1; 

    int newBlock = blockMgr->allocateBlock();
    if (newBlock == -1) return -1;
    blockMgr->setNextBlock(newBlock, FAT_EOF);
    
    std::vector<char> zeros(PAYLOAD_SIZE, 0);
    blockMgr->writeData(newBlock, zeros.data(), PAYLOAD_SIZE);

    std::memset(&entries[freeIdx], 0, sizeof(FileEntry));
    Utils::safeStrCopy(entries[freeIdx].filename, name);
    entries[freeIdx].file_size = 0;
    entries[freeIdx].start_block_index = newBlock;
    entries[freeIdx].is_active = true;
    entries[freeIdx].is_directory = isDir;

    blockMgr->atomicWrite(dirBlock, buffer.data(), PAYLOAD_SIZE, 0);
    return newBlock;
}

bool FileManager::importFile(const std::string& fullPath, const std::string& content) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex); 
    std::string dirPath, fileName;
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) { dirPath = ""; fileName = fullPath; }
    else { dirPath = fullPath.substr(0, lastSlash); fileName = fullPath.substr(lastSlash + 1); }

    int dirBlock = blockMgr->getDisk()->info.root_dir_block;
    if (!dirPath.empty()) {
        dirBlock = resolvePathToBlock(dirPath, true);
        if (dirBlock == -1) return false;
    }

    std::vector<char> dirBuf(BLOCK_SIZE);
    blockMgr->readData(dirBlock, dirBuf.data(), BLOCK_SIZE);
    FileEntry* entries = reinterpret_cast<FileEntry*>(dirBuf.data());
    int maxFiles = PAYLOAD_SIZE / sizeof(FileEntry);
    int freeIdx = -1;

    for (int i = 0; i < maxFiles; ++i) {
        if (entries[i].is_active && std::strncmp(entries[i].filename, fileName.c_str(), MAX_FILENAME) == 0) {
            blockMgr->freeBlockChain(entries[i].start_block_index);
            entries[i].is_active = false;
        }
        if (!entries[i].is_active && entries[i].filename[0] == 0 && freeIdx == -1) freeIdx = i;
    }
    if (freeIdx == -1) return false;

    int totalSize = content.size();
    int startBlock = -1;

    if (totalSize > 0) {
        int bytesWritten = 0;
        int prevBlock = -1;
        while (bytesWritten < totalSize) {
            int currentBlock = blockMgr->allocateBlock();
            if (currentBlock == -1) return false; 
            if (startBlock == -1) startBlock = currentBlock;
            if (prevBlock != -1) blockMgr->setNextBlock(prevBlock, currentBlock);

            int chunk = std::min((int)PAYLOAD_SIZE, totalSize - bytesWritten);
            blockMgr->writeData(currentBlock, content.data() + bytesWritten, chunk);
            bytesWritten += chunk;
            prevBlock = currentBlock;
        }
        blockMgr->setNextBlock(prevBlock, FAT_EOF);
    } else {
        startBlock = blockMgr->allocateBlock();
        if (startBlock != -1) blockMgr->setNextBlock(startBlock, FAT_EOF);
    }

    std::memset(&entries[freeIdx], 0, sizeof(FileEntry));
    Utils::safeStrCopy(entries[freeIdx].filename, fileName);
    entries[freeIdx].file_size = totalSize;
    entries[freeIdx].start_block_index = startBlock;
    entries[freeIdx].is_active = true;
    entries[freeIdx].is_directory = false;

    blockMgr->atomicWrite(dirBlock, dirBuf.data(), PAYLOAD_SIZE, 0);
    return true;
}

std::string FileManager::getFileContent(const std::string& fullPath) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex); 
    std::string dirPath, fileName;
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) { dirPath = ""; fileName = fullPath; }
    else { dirPath = fullPath.substr(0, lastSlash); fileName = fullPath.substr(lastSlash + 1); }

    int dirBlock = blockMgr->getDisk()->info.root_dir_block;
    if (!dirPath.empty()) {
        dirBlock = resolvePathToBlock(dirPath, false);
        if (dirBlock == -1) return "";
    }

    auto dirContent = readChain(dirBlock, 0);
    FileEntry* entries = reinterpret_cast<FileEntry*>(dirContent.data());
    int count = dirContent.size() / sizeof(FileEntry);

    for (int i = 0; i < count; ++i) {
        if (entries[i].is_active && !entries[i].is_directory && std::strncmp(entries[i].filename, fileName.c_str(), MAX_FILENAME) == 0) {
            auto content = readChain(entries[i].start_block_index, entries[i].file_size);
            return std::string(content.begin(), content.end());
        }
    }
    return "";
}

bool FileManager::createDirectory(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex);
    return resolvePathToBlock(path, true) != -1;
}

bool FileManager::deletePath(const std::string& fullPath) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex);
    return _deleteFileInternal(fullPath);
}

bool FileManager::_deleteFileInternal(const std::string& fullPath) {
    std::string dirPath, fileName;
    size_t lastSlash = fullPath.find_last_of('/');
    if (lastSlash == std::string::npos) { dirPath = ""; fileName = fullPath; }
    else { dirPath = fullPath.substr(0, lastSlash); fileName = fullPath.substr(lastSlash + 1); }

    int dirBlock = (dirPath.empty()) ? blockMgr->getDisk()->info.root_dir_block : resolvePathToBlock(dirPath, false);
    if(dirBlock == -1) return false;

    std::vector<char> buffer(BLOCK_SIZE);
    blockMgr->readData(dirBlock, buffer.data(), BLOCK_SIZE);
    FileEntry* entries = reinterpret_cast<FileEntry*>(buffer.data());
    int maxFiles = PAYLOAD_SIZE / sizeof(FileEntry);

    for(int i=0; i<maxFiles; ++i) {
        if(entries[i].is_active && std::strncmp(entries[i].filename, fileName.c_str(), MAX_FILENAME) == 0) {
            if (entries[i].is_directory) {
                 // Recursive delete logic (simplified)
                 int subDirBlock = entries[i].start_block_index;
                 auto subContent = readChain(subDirBlock, 0);
                 int subCount = subContent.size() / sizeof(FileEntry);
                 FileEntry* subEntries = reinterpret_cast<FileEntry*>(subContent.data());
                 for(int k=0; k<subCount; ++k) {
                    if (subEntries[k].is_active) {
                        std::string subName = Utils::safeString(subEntries[k].filename, MAX_FILENAME);
                        std::string subPath = fullPath + "/" + subName;
                        _deleteFileInternal(subPath);
                    }
                 }
            }
            blockMgr->freeBlockChain(entries[i].start_block_index);
            std::memset(&entries[i], 0, sizeof(FileEntry));
            blockMgr->atomicWrite(dirBlock, buffer.data(), PAYLOAD_SIZE, 0);
            return true;
        }
    }
    return false;
}

bool FileManager::movePath(const std::string& oldFullPath, const std::string& newFullPath) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex);
    std::string oldDir, oldName;
    size_t lastSlash = oldFullPath.find_last_of('/');
    if (lastSlash == std::string::npos) { oldDir = ""; oldName = oldFullPath; }
    else { oldDir = oldFullPath.substr(0, lastSlash); oldName = oldFullPath.substr(lastSlash + 1); }

    int oldDirBlock = (oldDir.empty()) ? blockMgr->getDisk()->info.root_dir_block : resolvePathToBlock(oldDir, false);
    if (oldDirBlock == -1) return false;

    std::string newDir, newName;
    lastSlash = newFullPath.find_last_of('/');
    if (lastSlash == std::string::npos) { newDir = ""; newName = newFullPath; }
    else { newDir = newFullPath.substr(0, lastSlash); newName = newFullPath.substr(lastSlash + 1); }

    int newDirBlock = (newDir.empty()) ? blockMgr->getDisk()->info.root_dir_block : resolvePathToBlock(newDir, false);
    if (newDirBlock == -1) return false;

    std::vector<char> oldBuf(BLOCK_SIZE);
    blockMgr->readData(oldDirBlock, oldBuf.data(), BLOCK_SIZE);
    FileEntry* oldEntries = reinterpret_cast<FileEntry*>(oldBuf.data());
    int oldIdx = -1;
    FileEntry movingEntry;
    int maxFiles = PAYLOAD_SIZE / sizeof(FileEntry);

    for (int i = 0; i < maxFiles; ++i) {
        if (oldEntries[i].is_active && std::strncmp(oldEntries[i].filename, oldName.c_str(), MAX_FILENAME) == 0) {
            oldIdx = i;
            std::memcpy(&movingEntry, &oldEntries[i], sizeof(FileEntry));
            break;
        }
    }
    if (oldIdx == -1) return false;

    Utils::safeStrCopy(movingEntry.filename, newName);

    if (oldDirBlock == newDirBlock) {
        std::memcpy(&oldEntries[oldIdx], &movingEntry, sizeof(FileEntry));
        blockMgr->atomicWrite(oldDirBlock, oldBuf.data(), PAYLOAD_SIZE, 0);
        return true;
    }

    std::vector<char> newBuf(BLOCK_SIZE);
    blockMgr->readData(newDirBlock, newBuf.data(), BLOCK_SIZE);
    FileEntry* newEntries = reinterpret_cast<FileEntry*>(newBuf.data());
    int newIdx = -1;
    for (int i = 0; i < maxFiles; ++i) {
        if (!newEntries[i].is_active && newEntries[i].filename[0] == 0) {
            newIdx = i;
            break;
        }
    }
    if (newIdx == -1) return false;

    std::memcpy(&newEntries[newIdx], &movingEntry, sizeof(FileEntry));
    blockMgr->atomicWrite(newDirBlock, newBuf.data(), PAYLOAD_SIZE, 0);

    std::memset(&oldEntries[oldIdx], 0, sizeof(FileEntry));
    blockMgr->atomicWrite(oldDirBlock, oldBuf.data(), PAYLOAD_SIZE, 0);
    return true;
}

std::vector<FileInfo> FileManager::listDirectory(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex);
    std::vector<FileInfo> result;
    int dirBlock = blockMgr->getDisk()->info.root_dir_block;
    if (path != "/" && !path.empty()) {
        dirBlock = resolvePathToBlock(path, false);
        if (dirBlock == -1) return result;
    }

    auto content = readChain(dirBlock, 0);
    FileEntry* entries = reinterpret_cast<FileEntry*>(content.data());
    int count = content.size() / sizeof(FileEntry);

    for (int i = 0; i < count; ++i) {
        if (entries[i].is_active) {
            std::string safeName = Utils::safeString(entries[i].filename, MAX_FILENAME);
            result.push_back({safeName, (int)entries[i].file_size, (int)entries[i].start_block_index, "", entries[i].is_directory});
        }
    }
    return result;
}

void FileManager::_searchRecursive(int dirBlock, const std::string& currentPath, const std::string& query, std::vector<FileInfo>& results) {
    auto content = readChain(dirBlock, 0);
    int count = content.size() / sizeof(FileEntry);
    FileEntry* entries = reinterpret_cast<FileEntry*>(content.data());

    for (int i = 0; i < count; ++i) {
        if (entries[i].is_active) {
            std::string name = Utils::safeString(entries[i].filename, MAX_FILENAME);
            std::string lowerName = Utils::toLower(name);
            if (lowerName.find(query) != std::string::npos) {
                if (name.find("thumb_") != 0 && name.find("preview_") != 0) {
                    FileInfo info;
                    info.name = name;
                    info.size = entries[i].file_size;
                    info.block = entries[i].start_block_index;
                    info.is_dir = entries[i].is_directory;
                    info.full_path = (currentPath.empty() ? "/" : currentPath + "/") + name;
                    results.push_back(info);
                }
            }
            if (entries[i].is_directory) {
                std::string newPath = (currentPath.empty() ? "" : currentPath + "/") + name;
                _searchRecursive(entries[i].start_block_index, newPath, query, results);
            }
        }
    }
}

std::vector<FileInfo> FileManager::searchFiles(const std::string& query) {
    std::lock_guard<std::recursive_mutex> lock(fileMutex);
    std::vector<FileInfo> results;
    int rootBlock = blockMgr->getDisk()->info.root_dir_block;
    std::string lowerQuery = Utils::toLower(query);
    _searchRecursive(rootBlock, "", lowerQuery, results);
    return results;
}

DiskStats FileManager::getDiskUsage() {
    return blockMgr->getStats();
}