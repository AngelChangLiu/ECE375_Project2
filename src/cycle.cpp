#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

static Simulator *simulator = nullptr;
static Cache *iCache = nullptr;
static Cache *dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;
static uint64_t loadStallCount = 0;
static uint64_t remainingStallCycles = 0;
static uint64_t PC = 0;

// I-cache miss: the fetch for PC already happened, waiting for data
static uint64_t iCacheStallCycles = 0;

// D-cache miss: waiting for memory access to complete
static uint64_t dCacheStallCycles = 0;

static bool haltInPipeline = false;

#define EXC_HANDLER 0x8000

Simulator::Instruction nop(StageStatus status)
{
    Simulator::Instruction nopInst;
    nopInst.instruction = 0x00000013;
    nopInst.isLegal = true;
    nopInst.isNop = true;
    nopInst.status = status;
    return nopInst;
}

static struct PipelineInfo
{
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;

Status initSimulator(CacheConfig &iCacheConfig, CacheConfig &dCacheConfig, MemoryStore *mem,
                     const std::string &output_name)
{
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    
    cycleCount = 0;
    loadStallCount = 0;
    remainingStallCycles = 0;
    PC = 0;
    iCacheStallCycles = 0;
    dCacheStallCycles = 0;
    haltInPipeline = false;
    
    pipelineInfo.ifInst = nop(IDLE);
    pipelineInfo.idInst = nop(IDLE);
    pipelineInfo.exInst = nop(IDLE);
    pipelineInfo.memInst = nop(IDLE);
    pipelineInfo.wbInst = nop(IDLE);
    
    return SUCCESS;
}

static bool isValidInst(const Simulator::Instruction &inst) {
    return inst.isLegal && !inst.isNop && !inst.isHalt && 
           inst.status != BUBBLE && inst.status != SQUASHED && inst.status != IDLE;
}
static bool isLoad(const Simulator::Instruction &inst)   { return inst.readsMem && isValidInst(inst); }
static bool isStore(const Simulator::Instruction &inst)  { return inst.writesMem && isValidInst(inst); }
static bool writesReg(const Simulator::Instruction &inst){ return inst.writesRd && inst.rd != 0 && isValidInst(inst); }
static bool isBranchOrJump(const Simulator::Instruction &inst) {
    return (inst.opcode == OP_BRANCH || inst.opcode == OP_JAL || inst.opcode == OP_JALR) && isValidInst(inst);
}
static bool isMemoryInst(const Simulator::Instruction &inst) {
    return (inst.readsMem || inst.writesMem) && isValidInst(inst);
}

Status runCycles(uint64_t cycles)
{
    uint64_t count = 0;
    auto status = SUCCESS;
    PipeState pipeState = {};

    while (cycles == 0 || count < cycles)
    {
        pipeState.cycle = cycleCount;

        Simulator::Instruction prevIF = pipelineInfo.ifInst;
        Simulator::Instruction prevID = pipelineInfo.idInst;
        Simulator::Instruction prevEX = pipelineInfo.exInst;
        Simulator::Instruction prevMEM = pipelineInfo.memInst;

        bool stallPipeline = false;
        bool branchTaken = false;
        bool illegalException = false;
        bool memException = false;
        uint64_t branchTarget = 0;

        // D-CACHE HANDLING
        bool dCacheStalling = false;
        
        if (dCacheStallCycles > 0)
        {
            // Currently in D-cache miss stall
            dCacheStallCycles--;
            dCacheStalling = true;
        }
        else if (isMemoryInst(prevEX))
        {
            // Check if new memory access misses
            CacheOperation op = prevEX.writesMem ? CACHE_WRITE : CACHE_READ;
            if (!dCache->access(prevEX.memAddress, op))
            {
                dCacheStallCycles = dCache->config.missLatency;
                if (dCacheStallCycles > 0) {
                    dCacheStallCycles--;  // This cycle counts as first stall
                    dCacheStalling = true;
                }
            }
        }

        // I-CACHE HANDLING
        bool iCacheStalling = false;
        
        if (!dCacheStalling)
        {
            if (iCacheStallCycles > 0)
            {
                iCacheStallCycles--;
                iCacheStalling = true;
            }
        }

        // HAZARD DETECTION
        Simulator::Instruction specDecode;
        // Only check hazards when instruction can actually proceed from IF to ID
        bool canCheckHazards = !dCacheStalling && !iCacheStalling && 
                               prevIF.status != IDLE && prevIF.status != BUBBLE &&
                               prevIF.status != SQUASHED && !prevIF.isNop;
        
        if (canCheckHazards)
            specDecode = simulator->simID(prevIF);
        else
            specDecode = nop(BUBBLE);

        if (remainingStallCycles > 0) {
            stallPipeline = true;
            remainingStallCycles--;
        }
        else if (canCheckHazards)
        {
            if (isLoad(prevID))
            {
                uint64_t loadRd = prevID.rd;
                
                if (isBranchOrJump(specDecode)) {
                    bool depends = (specDecode.readsRs1 && specDecode.rs1 == loadRd && loadRd != 0) ||
                                   (specDecode.readsRs2 && specDecode.rs2 == loadRd && loadRd != 0);
                    if (depends) { stallPipeline = true; remainingStallCycles = 1; loadStallCount++; }
                }
                else if (isStore(specDecode)) {
                    if (specDecode.readsRs1 && specDecode.rs1 == loadRd && loadRd != 0)
                    { stallPipeline = true; loadStallCount++; }
                }
                else if (!specDecode.isHalt && !specDecode.isNop) {
                    bool depends = (specDecode.readsRs1 && specDecode.rs1 == loadRd && loadRd != 0) ||
                                   (specDecode.readsRs2 && specDecode.rs2 == loadRd && loadRd != 0);
                    if (depends) { stallPipeline = true; loadStallCount++; }
                }
            }

            if (!stallPipeline && isBranchOrJump(specDecode) && isLoad(prevEX)) {
                uint64_t loadRd = prevEX.rd;
                bool depends = (specDecode.readsRs1 && specDecode.rs1 == loadRd && loadRd != 0) ||
                               (specDecode.readsRs2 && specDecode.rs2 == loadRd && loadRd != 0);
                if (depends) { stallPipeline = true; loadStallCount++; }
            }

            if (!stallPipeline && isBranchOrJump(specDecode) && writesReg(prevID) && !isLoad(prevID)) {
                uint64_t aluRd = prevID.rd;
                bool depends = (specDecode.readsRs1 && specDecode.rs1 == aluRd && aluRd != 0) ||
                               (specDecode.readsRs2 && specDecode.rs2 == aluRd && aluRd != 0);
                if (depends) { stallPipeline = true; }
            }
        }

        // FORWARDING
        if (writesReg(prevMEM)) {
            uint64_t rd = prevMEM.rd;
            uint64_t val = isLoad(prevMEM) ? prevMEM.memResult : prevMEM.arithResult;
            if (prevID.readsRs1 && prevID.rs1 == rd && rd != 0) prevID.op1Val = val;
            if (prevID.readsRs2 && prevID.rs2 == rd && rd != 0) prevID.op2Val = val;
        }
        if (writesReg(prevEX)) {
            uint64_t rd = prevEX.rd;
            if (prevID.readsRs1 && prevID.rs1 == rd && rd != 0) prevID.op1Val = prevEX.arithResult;
            if (prevID.readsRs2 && prevID.rs2 == rd && rd != 0) prevID.op2Val = prevEX.arithResult;
        }
        if (isStore(prevEX) && writesReg(prevMEM)) {
            uint64_t rd = prevMEM.rd;
            if (prevEX.rs2 == rd && rd != 0)
                prevEX.op2Val = isLoad(prevMEM) ? prevMEM.memResult : prevMEM.arithResult;
        }

        // PIPELINE STAGES

        // WB Stage
        if (dCacheStalling)
            pipelineInfo.wbInst = nop(BUBBLE);
        else
            pipelineInfo.wbInst = simulator->simWB(prevMEM);

        if (pipelineInfo.wbInst.isHalt) {
            pipeState.ifPC = pipelineInfo.ifInst.PC;
            pipeState.ifStatus = pipelineInfo.ifInst.status;
            pipeState.idInstr = pipelineInfo.idInst.instruction;
            pipeState.idStatus = pipelineInfo.idInst.status;
            pipeState.exInstr = pipelineInfo.exInst.instruction;
            pipeState.exStatus = pipelineInfo.exInst.status;
            pipeState.memInstr = pipelineInfo.memInst.instruction;
            pipeState.memStatus = pipelineInfo.memInst.status;
            pipeState.wbInstr = pipelineInfo.wbInst.instruction;
            pipeState.wbStatus = pipelineInfo.wbInst.status;
            dumpPipeState(pipeState, output);
            cycleCount++;
            return HALT;
        }

        // MEM Stage
        if (dCacheStalling)
            pipelineInfo.memInst = prevMEM;
        else
            pipelineInfo.memInst = simulator->simMEM(prevEX);

        if (pipelineInfo.memInst.memException) {
            memException = true;
            pipelineInfo.memInst.status = SQUASHED;
        }

        // EX Stage
        if (dCacheStalling)
            pipelineInfo.exInst = prevEX;
        else
            pipelineInfo.exInst = simulator->simEX(prevID);  // prevID always proceeds to EX

        // ID Stage - on hazard stall, insert bubble (instruction in IF can't proceed)
        if (dCacheStalling)
            pipelineInfo.idInst = prevID;
        else if (stallPipeline)
            pipelineInfo.idInst = nop(BUBBLE);  // Bubble because IF can't send instruction
        else if (iCacheStalling)
            pipelineInfo.idInst = nop(BUBBLE);
        else
            pipelineInfo.idInst = simulator->simID(prevIF);

        // Exception check
        if (!pipelineInfo.idInst.isLegal && !pipelineInfo.idInst.isNop &&
            pipelineInfo.idInst.status != BUBBLE && pipelineInfo.idInst.status != SQUASHED &&
            !pipelineInfo.idInst.isHalt) {
            illegalException = true;
            pipelineInfo.idInst.status = SQUASHED;
        }

        if (pipelineInfo.idInst.isHalt)
            haltInPipeline = true;

        // Branch resolution
        if (!stallPipeline && !dCacheStalling && !iCacheStalling && isBranchOrJump(pipelineInfo.idInst)) {
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4) {
                branchTaken = true;
                branchTarget = pipelineInfo.idInst.nextPC;
            }
        }

        // IF Stage
        if (illegalException) {
            pipelineInfo.ifInst.status = SQUASHED;
            PC = EXC_HANDLER;
            haltInPipeline = false;
        }
        else if (memException) {
            pipelineInfo.ifInst.status = SQUASHED;
            pipelineInfo.idInst.status = SQUASHED;
            pipelineInfo.exInst.status = SQUASHED;
            PC = EXC_HANDLER;
            haltInPipeline = false;
        }
        else if (dCacheStalling) {
            pipelineInfo.ifInst = prevIF;
        }
        else if (iCacheStalling) {
            // Keep showing same instruction while waiting
            pipelineInfo.ifInst = simulator->simIF(PC);
            // When stall ends (iCacheStallCycles just became 0), advance PC for next cycle
            if (iCacheStallCycles == 0) {
                PC += 4;
            }
        }
        else if (stallPipeline) {
            pipelineInfo.ifInst = prevIF;
            if (isBranchOrJump(specDecode))
                pipelineInfo.ifInst.status = SPECULATIVE;
        }
        else if (branchTaken) {
            pipelineInfo.ifInst = prevIF;
            pipelineInfo.ifInst.status = SQUASHED;
            PC = branchTarget;
        }
        else if (haltInPipeline) {
            pipelineInfo.ifInst = nop(BUBBLE);
        }
        else {
            // Normal fetch
            // First check if PC access hits cache
            bool iMiss = !iCache->access(PC, CACHE_READ);
            if (iMiss && iCache->config.missLatency > 0) {
                // Miss with latency - will stall starting next cycle
                iCacheStallCycles = iCache->config.missLatency;
            }
            
            // Fetch the instruction at current PC
            pipelineInfo.ifInst = simulator->simIF(PC);
            
            // Only advance PC if not starting a stall
            // (instruction is fetched but not available until stall ends)
            if (!iMiss || iCache->config.missLatency == 0) {
                PC += 4;
            }
            
            if (isBranchOrJump(pipelineInfo.idInst))
                pipelineInfo.ifInst.status = SPECULATIVE;
        }

        // Dump state
        pipeState.ifPC = pipelineInfo.ifInst.PC;
        pipeState.ifStatus = pipelineInfo.ifInst.status;
        pipeState.idInstr = pipelineInfo.idInst.instruction;
        pipeState.idStatus = pipelineInfo.idInst.status;
        pipeState.exInstr = pipelineInfo.exInst.instruction;
        pipeState.exStatus = pipelineInfo.exInst.status;
        pipeState.memInstr = pipelineInfo.memInst.instruction;
        pipeState.memStatus = pipelineInfo.memInst.status;
        pipeState.wbInstr = pipelineInfo.wbInst.instruction;
        pipeState.wbStatus = pipelineInfo.wbInst.status;
        dumpPipeState(pipeState, output);

        count++;
        cycleCount++;
    }

    return status;
}

Status runTillHalt()
{
    Status status;
    while (true) {
        status = runCycles(1);
        if (status == HALT || status == ERROR) break;
    }
    return status;
}

Status finalizeSimulator()
{
    simulator->dumpRegMem(output);
    SimulationStats stats{
        simulator->getDin(),
        cycleCount,
        iCache->getHits(),
        iCache->getMisses(),
        dCache->getHits(),
        dCache->getMisses(),
        loadStallCount
    };
    dumpSimStats(stats, output);
    return SUCCESS;
}