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
    Simulator::Instruction nopInst = {};  // Zero-initialize all fields
    nopInst.instruction = 0x00000013;
    nopInst.isLegal = true;
    nopInst.isNop = true;
    nopInst.isHalt = false;  // Explicitly set to false
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

        // =================================================================
        // D-CACHE HANDLING
        // =================================================================
        bool dCacheStalling = false;
        
        if (dCacheStallCycles > 0)
        {
            // Currently in D-cache miss stall (instruction is stuck in MEM)
            dCacheStallCycles--;
            dCacheStalling = true;
        }
        // Note: We check for new D-cache miss in the MEM stage processing below
        // The miss is detected when instruction enters MEM, stall starts next cycle

        // =================================================================
        // I-CACHE HANDLING
        // =================================================================
        bool iCacheStalling = false;
        
        // I-cache stall counter decrements even during D-cache stall
        // (the cache miss penalty runs in parallel)
        if (iCacheStallCycles > 0)
        {
            iCacheStallCycles--;
            if (!dCacheStalling)  // Only affects pipeline if not D-cache stalling
                iCacheStalling = true;
            else if (iCacheStallCycles == 0)
            {
                // I-cache stall ended during D-cache stall - advance PC
                PC += 4;
            }
        }
        // Note: We check for new I-cache miss AFTER processing current IF->ID
        // This is handled at the end when we try to fetch the next instruction

        // =================================================================
        // HAZARD DETECTION
        // =================================================================
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

        // =================================================================
        // FORWARDING
        // =================================================================
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

        // =================================================================
        // PIPELINE STAGES
        // =================================================================

        // WB Stage
        if (dCacheStalling)
            pipelineInfo.wbInst = nop(BUBBLE);
        else if (prevMEM.status == IDLE)
            pipelineInfo.wbInst = nop(IDLE);  // Preserve IDLE during pipeline fill
        else
            pipelineInfo.wbInst = simulator->simWB(prevMEM);

        // Only return HALT when the actual halt instruction (0xfeedfeed) reaches WB
        // and it's not a NOP/bubble/squashed instruction
        if (pipelineInfo.wbInst.isHalt && !pipelineInfo.wbInst.isNop &&
            pipelineInfo.wbInst.status != BUBBLE && pipelineInfo.wbInst.status != SQUASHED &&
            pipelineInfo.wbInst.status != IDLE && pipelineInfo.wbInst.instruction == 0xfeedfeed) {
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
            pipelineInfo.memInst = prevMEM;  // Keep instruction in MEM during stall
        else if (prevEX.status == IDLE)
            pipelineInfo.memInst = nop(IDLE);  // Preserve IDLE during pipeline fill
        else
        {
            pipelineInfo.memInst = simulator->simMEM(prevEX);
            
            // Check for D-cache miss when memory instruction enters MEM
            if (isMemoryInst(pipelineInfo.memInst))
            {
                CacheOperation op = pipelineInfo.memInst.writesMem ? CACHE_WRITE : CACHE_READ;
                if (!dCache->access(pipelineInfo.memInst.memAddress, op))
                {
                    // Cache miss - stall will start next cycle
                    dCacheStallCycles = dCache->config.missLatency;
                }
            }
        }

        if (pipelineInfo.memInst.memException) {
            memException = true;
            pipelineInfo.memInst.status = SQUASHED;
        }

        // EX Stage
        if (dCacheStalling)
            pipelineInfo.exInst = prevEX;
        else if (prevID.status == IDLE)
            pipelineInfo.exInst = nop(IDLE);  // Preserve IDLE during pipeline fill
        else
            pipelineInfo.exInst = simulator->simEX(prevID);  // prevID always proceeds to EX

        // ID Stage - on hazard stall, insert bubble (instruction in IF can't proceed)
        if (dCacheStalling)
            pipelineInfo.idInst = prevID;  // D-cache stall: keep instruction in ID
        else if (stallPipeline)
            pipelineInfo.idInst = nop(BUBBLE);  // Hazard stall: bubble because IF can't send instruction
        else if (iCacheStalling)
            pipelineInfo.idInst = nop(BUBBLE);
        else if (prevIF.status == IDLE || (prevIF.isNop && prevIF.status == IDLE))
            pipelineInfo.idInst = nop(IDLE);  // Keep IDLE status during pipeline fill
        else
            pipelineInfo.idInst = simulator->simID(prevIF);
        
        // =================================================================
        // FORWARDING FOR BRANCHES in ID
        // =================================================================
        // After decoding, forward values to the newly decoded instruction
        // This is needed for branches which evaluate their condition in ID
        if (!pipelineInfo.idInst.isNop && pipelineInfo.idInst.status != BUBBLE &&
            isBranchOrJump(pipelineInfo.idInst)) {
            // Forward from prevID (just went to EX this cycle)
            if (writesReg(prevID)) {
                uint64_t rd = prevID.rd;
                uint64_t val = prevID.arithResult;
                if (pipelineInfo.idInst.readsRs1 && pipelineInfo.idInst.rs1 == rd && rd != 0) 
                    pipelineInfo.idInst.op1Val = val;
                if (pipelineInfo.idInst.readsRs2 && pipelineInfo.idInst.rs2 == rd && rd != 0) 
                    pipelineInfo.idInst.op2Val = val;
            }
            // Forward from prevEX (now in MEM)
            if (writesReg(prevEX)) {
                uint64_t rd = prevEX.rd;
                uint64_t val = prevEX.arithResult;
                if (pipelineInfo.idInst.readsRs1 && pipelineInfo.idInst.rs1 == rd && rd != 0) 
                    pipelineInfo.idInst.op1Val = val;
                if (pipelineInfo.idInst.readsRs2 && pipelineInfo.idInst.rs2 == rd && rd != 0) 
                    pipelineInfo.idInst.op2Val = val;
            }
            // Forward from prevMEM (now in WB)
            if (writesReg(prevMEM)) {
                uint64_t rd = prevMEM.rd;
                uint64_t val = isLoad(prevMEM) ? prevMEM.memResult : prevMEM.arithResult;
                if (pipelineInfo.idInst.readsRs1 && pipelineInfo.idInst.rs1 == rd && rd != 0) 
                    pipelineInfo.idInst.op1Val = val;
                if (pipelineInfo.idInst.readsRs2 && pipelineInfo.idInst.rs2 == rd && rd != 0) 
                    pipelineInfo.idInst.op2Val = val;
            }
            
            // Re-evaluate branch condition with forwarded values
            if (pipelineInfo.idInst.opcode == OP_BRANCH) {
                // Calculate branch target from instruction encoding
                uint64_t imm5 = pipelineInfo.idInst.rd;
                uint64_t imm7 = pipelineInfo.idInst.funct7;
                uint64_t branchOffset = sext64(
                    extractBits(imm7, 6, 6) << 12 |
                    extractBits(imm7, 5, 0) << 5 |
                    extractBits(imm5, 4, 1) << 1 |
                    extractBits(imm5, 0, 0) << 11,
                    12);
                uint64_t branchTarget = pipelineInfo.idInst.PC + branchOffset;
                
                bool taken = false;
                switch (pipelineInfo.idInst.funct3) {
                    case 0: taken = (pipelineInfo.idInst.op1Val == pipelineInfo.idInst.op2Val); break; // BEQ
                    case 1: taken = (pipelineInfo.idInst.op1Val != pipelineInfo.idInst.op2Val); break; // BNE
                    case 4: taken = ((int64_t)pipelineInfo.idInst.op1Val < (int64_t)pipelineInfo.idInst.op2Val); break; // BLT
                    case 5: taken = ((int64_t)pipelineInfo.idInst.op1Val >= (int64_t)pipelineInfo.idInst.op2Val); break; // BGE
                    case 6: taken = (pipelineInfo.idInst.op1Val < pipelineInfo.idInst.op2Val); break; // BLTU
                    case 7: taken = (pipelineInfo.idInst.op1Val >= pipelineInfo.idInst.op2Val); break; // BGEU
                }
                pipelineInfo.idInst.nextPC = taken ? branchTarget : (pipelineInfo.idInst.PC + 4);
            }
        }

        // Exception check
        if (!pipelineInfo.idInst.isLegal && !pipelineInfo.idInst.isNop &&
            pipelineInfo.idInst.status != BUBBLE && pipelineInfo.idInst.status != SQUASHED &&
            !pipelineInfo.idInst.isHalt) {
            illegalException = true;
            pipelineInfo.idInst.status = SQUASHED;
        }

        if (pipelineInfo.idInst.isHalt && !pipelineInfo.idInst.isNop &&
            pipelineInfo.idInst.status != BUBBLE && pipelineInfo.idInst.status != SQUASHED &&
            pipelineInfo.idInst.status != IDLE && pipelineInfo.idInst.instruction == 0xfeedfeed)
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