// Sample cache implementation with random hit return
// TODO: Modify this file to model an LRU cache as in the project description

#include "cache.h"
#include <random>

using namespace std;
// std::vector<Set> sets;

// Random generator for cache hit/miss simulation
static std::mt19937 generator(42); // Fixed seed for deterministic results
std::uniform_real_distribution<double> distribution(0.0, 1.0);

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType) : hits(0), misses(0), type(cacheType), config(configParam){    // Here you can initialize other cache-specific attributes
    // For instance, if you had cache tables or other structures, initialize them here
    uint64_t numSets = (config.cacheSize / config.blockSize) / config.ways;
    sets.resize(numSets);
    for (uint64_t i = 0; i < numSets; i++) {
        sets[i].ways.resize(config.ways);
        for (uint64_t j = 0; j < config.ways; j++) {
            sets[i].ways[j].isValid = false;
            sets[i].ways[j].tag = 0;
            sets[i].ways[j].lru = j;
        }
    }
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite)
{
    bool hit = false; // Angel changed to true for testing pipeline

    // setting bits to extract fields from address
    uint64_t index_bits = log2((config.cacheSize / config.blockSize) / config.ways);
    uint64_t offset_bits = log2(config.blockSize);
    // uint64_t tag_bits = 64 - index_bits - offset_bits;

    uint64_t index = (address >> offset_bits) & ((0x1 << index_bits) - 1);
    uint64_t tag = (address >> (offset_bits + index_bits));
    if (address == 0x20) {
        std::cout << "[DEBUG] addr: " << address << std::endl;
        std::cout << "[DEBUG] tag: " << tag << std::endl;
        std::cout << "[DEBUG] index: " << index << std::endl;
    }


    // search through set for block that contains tag from address
    Set& currentSet = sets[index];
    uint64_t oldLRU = 0;
    uint64_t hitIndex = config.ways - 1;
    for (uint64_t i = 0; i < config.ways; i++)
    {
        Block& currBlock = currentSet.ways[i];
        if (currBlock.isValid && currBlock.tag == tag)
        {
            hit = true;
            hitIndex = i;
            oldLRU = currBlock.lru;
            if (address == 0x20) {
                std::cout << "[DEBUG] hit: " << address << std::endl;
            }
            break;
        }
    }

    // if tag is found, update lru values
    if (hit)
    {
        for (uint64_t i = 0; i < config.ways; i++)
        {
            Block& currBlock = currentSet.ways[i];
            if (i == hitIndex)
                currBlock.lru = 0;
            else if (currBlock.lru < oldLRU)
                currBlock.lru++;
        }
    }
    // if tag is not found, update lru values
    else
    {
         if (address == 0x20) {
                std::cout << "[DEBUG] miss: " << address << std::endl;
        }
        // Find the LRU block
        uint64_t maxLRU = 0;
        uint64_t victimIndex = 0;
        for (uint64_t i = 0; i < config.ways; i++)
        {
            Block& currBlock = currentSet.ways[i];
            if (!currBlock.isValid) {
                // Use invalid block first
                victimIndex = i;
                maxLRU = currBlock.lru;
                break;
            }
            if (currBlock.lru >= maxLRU)
            {
                maxLRU = currBlock.lru;
                victimIndex = i;
            }
        }

        // Update LRU values
        for (uint64_t i = 0; i < config.ways; i++)
        {
            Block& currBlock = currentSet.ways[i];
            if (i == victimIndex)
            {
                currBlock.lru = 0;
                currBlock.isValid = true;
                currBlock.tag = tag;
            }
            else if (currBlock.lru < maxLRU)
                currBlock.lru++;
        }
    }
    hits += hit;
    misses += !hit;
    return hit;
}

bool Cache::incrementHits()
{
    hits = hits + 1;
    return true;
}

bool Cache::incrementMisses()
{
    misses = misses + 1;
    return true;
}
// debug: dump information as you needed, here are some examples
Status Cache::dump(const std::string &base_output_name)
{
    ofstream cache_out(base_output_name + "_cache_state.out");
    if (cache_out)
    {
        cache_out << "---------------------" << endl;
        cache_out << "Begin Register Values" << endl;
        cache_out << "---------------------" << endl;
        cache_out << "Cache Configuration:" << std::endl;
        cache_out << "Size: " << config.cacheSize << " bytes" << std::endl;
        cache_out << "Block Size: " << config.blockSize << " bytes" << std::endl;
        cache_out << "Ways: " << (config.ways == 1) << std::endl;
        cache_out << "Miss Latency: " << config.missLatency << " cycles" << std::endl;
        cache_out << "---------------------" << endl;
        cache_out << "End Register Values" << endl;
        cache_out << "---------------------" << endl;
        return SUCCESS;
    }
    else
    {
        cerr << LOG_ERROR << "Could not create cache state dump file" << endl;
        return ERROR;
    }
}
