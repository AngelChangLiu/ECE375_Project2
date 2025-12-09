// Sample cache implementation with random hit return
// TODO: Modify this file to model an LRU cache as in the project description

#include "cache.h"
#include <random>

using namespace std;
std::vector<Set> sets;

// Random generator for cache hit/miss simulation
static std::mt19937 generator(42); // Fixed seed for deterministic results
std::uniform_real_distribution<double> distribution(0.0, 1.0);

// Constructor definition
Cache::Cache(CacheConfig configParam, CacheDataType cacheType) : config(configParam)
{
    // Here you can initialize other cache-specific attributes
    // For instance, if you had cache tables or other structures, initialize them here
    sets.resize((config.cacheSize / config.blockSize) / config.ways);
}

// Access method definition
bool Cache::access(uint64_t address, CacheOperation readWrite)
{
    bool hit = false; // Angel changed to true for testing pipeline

    // setting bits to extract fields from address
    uint64_t index_bits = log2((config.cacheSize / config.blockSize) / config.ways);
    uint64_t offset_bits = log2(config.blockSize);
    uint64_t tag_bits = 64 - index_bits - offset_bits;

    uint64_t index = (address >> offset_bits) & ((0x1 << index_bits) - 1);
    uint64_t tag = (address >> offset_bits + index_bits);

    // search through set for block that contains tag from address
    Set currentSet = sets[index];
    uint64_t oldLRU = 0;
    uint64_t hitIndex = config.ways - 1;
    for (int i = 0; i < config.ways; i++)
    {
        Block currBlock = currentSet.ways[i];
        if (currBlock.isValid && currBlock.tag == tag)
        {
            hit = true;
            hitIndex = i;
            oldLRU = currBlock.lru;
            break;
        }
    }

    // if tag is found, update lru valuves
    if (hit)
    {
        for (int i = 0; i < config.ways; i++)
        {
            Block currBlock = currentSet.ways[i];
            if (i == hitIndex)
                currBlock.lru = 0;
            else if (currBlock.lru < oldLRU)
                currBlock.lru++;
        }
    }
    // if tag is not found, update lru valuves
    else
    {
        for (int i = 0; i < config.ways; i++)
        {
            Block currBlock = currentSet.ways[i];
            if (i == hitIndex)
            {
                currBlock.lru = 0;
                currBlock.isValid = true;
                currBlock.tag = tag;
            }
            else if (currBlock.lru < oldLRU)
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
