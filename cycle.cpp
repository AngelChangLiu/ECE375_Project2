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

static uint64_t PC = 0;

/**TODO: Implement pipeline simulation for the RISCV machine in this file.
 * A basic template is provided below that doesn't account for any hazards.
 */

Simulator::Instruction nop(StageStatus status)
{
    Simulator::Instruction nop;
    nop.instruction = 0x00000013;
    nop.isLegal = true;
    nop.isNop = true;
    nop.status = status;
    return nop;
}

static struct PipelineInfo
{
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;

// initialize the simulator
Status initSimulator(CacheConfig &iCacheConfig, CacheConfig &dCacheConfig, MemoryStore *mem,
                     const std::string &output_name)
{
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    return SUCCESS;
}

// return true if the instruction is a memory access
static bool isLoad(const Simulator::Instruction &inst)
{
    return inst.readsMem && inst.isLegal && !inst.isNop && inst.status != BUBBLE && inst.status != SQUASHED;
}

// return true if the instruction is a memory access
static bool isStore(const Simulator::Instruction &inst)
{
    return inst.writesMem && inst.isLegal && !inst.isNop && inst.status != BUBBLE && inst.status != SQUASHED;
}

// return true if the instruction writes to a register
static bool writesREG(const Simulator::Instruction &inst)
{
    return inst.writesRd && inst.rd != 0 && inst.isLegal && !inst.isNop && inst.status != BUBBLE && inst.status != SQUASHED;
}

static bool isBranchOrJump(const Simulator::Instruction &inst)
{
    return (inst.opcode == OP_BRANCH || inst.opcode == OP_JAL || inst.opcode == OP_JALR) && inst.isLegal && !inst.isNop && inst.status != BUBBLE && inst.status != SQUASHED;
}

// count of load-use stalls
static uint64_t loadStallCount = 0;

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles)
{
    uint64_t count = 0;

    auto status = SUCCESS;

    uint64_t const dCacheStalls = dCache->config.missLatency;
    uint64_t const iCacheStalls = iCache->config.missLatency;

    bool instFetchMiss = false;
    bool dataFetchMiss = false;

    uint64_t instMissStalls = 0;
    uint64_t dataMissStalls = 0;
    PipeState pipeState = {
        0,
    };

    while (cycles == 0 || count < cycles)
    {

        pipeState.cycle = cycleCount;
        count++;
        cycleCount++;

        // Save Previous Cycle Pipeline State
        Simulator::Instruction prevIFInst = pipelineInfo.ifInst;
        Simulator::Instruction prevIDInst = pipelineInfo.idInst;
        Simulator::Instruction prevEXInst = pipelineInfo.exInst;
        Simulator::Instruction prevMEMInst = pipelineInfo.memInst;

        // Speculative Decode
        Simulator::Instruction spec_decode = simulator->simID(prevIFInst);

        bool stall = false;
        bool cacheMiss = false;

        // Check for data hazards
        if (isLoad(prevIDInst))
        {
            uint64_t loadRd = prevIDInst.rd;

            if (isStore(spec_decode))
            {
                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }
            }

            else
            {
                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }
                if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }
            }
        }

        // Check for data hazards with branches/jumps
        if (!stall && isBranchOrJump(spec_decode))
        {
            if (writesREG(prevIDInst) && !isLoad(prevIDInst))
            {
                uint64_t aluRd = prevIDInst.rd;

                if ((spec_decode.readsRs1 && spec_decode.rs1 == aluRd) && (aluRd != 0))
                {
                    stall = true;
                }

                if ((spec_decode.readsRs2 && spec_decode.rs2 == aluRd) && (aluRd != 0))
                {
                    stall = true;
                }
            }
        }

        // Check for data hazards with stores
        if (!stall && isBranchOrJump(spec_decode))
        {
            // Check EX stage
            if (isLoad(prevIDInst))
            {
                uint64_t loadRd = prevIDInst.rd;

                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }

                if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }
            }

            // Check MEM stage
            else if (isLoad(prevEXInst))
            {
                uint64_t loadRd = prevEXInst.rd;

                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }

                if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                {
                    stall = true;
                    loadStallCount++;
                }
            }
        }

        // Memory Stage
        // Check MEM stages for Cache Miss:
        CacheOperation writeToCache = CACHE_READ;
        if (prevEXInst.writesMem)
        {
            writeToCache = CACHE_WRITE;
        }

        if (!prevEXInst.isNop and (prevEXInst.readsMem or prevEXInst.writesMem) and !dCache->access(prevEXInst.memAddress, writeToCache))
        {
            dataFetchMiss = true;
            dataMissStalls = 0;
            // Trigger
            dCache->incrementMisses();
        }
        else
        {
            dCache->incrementHits();
        }

        if (dataFetchMiss)
        {
            pipelineInfo.wbInst = nop(BUBBLE);
        }
        else
        {
            // Write Back Stage
            pipelineInfo.wbInst = simulator->simWB(prevMEMInst);
        }

        // Halt check
        if (pipelineInfo.wbInst.isHalt)
        {
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
            status = HALT;
            break;
        }

        if (dataFetchMiss)
        {
            dataMissStalls += 1;
            if (dataMissStalls == dCacheStalls)
            {
                dataFetchMiss = false;
            }
        }
        else
        {
            pipelineInfo.memInst = simulator->simMEM(prevEXInst);
        }

        // Execute Stage
        if (!dataFetchMiss)
        {
            pipelineInfo.exInst = simulator->simEX(prevIDInst);
        }

        // Instruction Decode Stage
        if (!dataFetchMiss)
        {
            pipelineInfo.idInst = spec_decode;
        }
        else if (stall)
        {
            pipelineInfo.idInst = nop(BUBBLE);
        }

        // Check for Branch
        bool taken = false;
        if (!stall && isBranchOrJump(pipelineInfo.idInst))
        {
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4)
            {
                taken = true;
            }
        }

        // Instruction Fetch Stage
        if (stall or dataFetchMiss)
        {
            pipelineInfo.ifInst = prevIFInst;
        }
        else if (taken)
        {
            pipelineInfo.ifInst.status = SQUASHED;
            PC = pipelineInfo.idInst.nextPC;
            // Check IF stages for Cache Miss:
            if (!iCache->access(PC, CACHE_READ))
            {
                instFetchMiss = true;
                instMissStalls = 0;
                // Trigger Stalls:
                iCache->incrementMisses();
            }
            else
            {
                iCache->incrementHits();
            }

            if (instFetchMiss)
            {
                pipelineInfo.ifInst = nop(BUBBLE);
                instMissStalls += 1;
                if (instMissStalls == iCacheStalls)
                {
                    instFetchMiss = false;
                }
            }
            else
            {
                pipelineInfo.ifInst = simulator->simIF(PC);
                PC += 4;
            }
        }
        else
        {
            // Check IF stages for Cache Miss:
            if (!iCache->access(PC, CACHE_READ))
            {
                instFetchMiss = true;
                instMissStalls = 0;
                // Trigger Stalls:
                iCache->incrementMisses();
            }
            else
            {
                iCache->incrementHits();
            }

            if (instFetchMiss)
            {
                pipelineInfo.ifInst = nop(BUBBLE);
                instMissStalls += 1;
                if (instMissStalls == iCacheStalls)
                {
                    instFetchMiss = false;
                }
            }
            else
            {
                pipelineInfo.ifInst = simulator->simIF(PC);
                PC += 4;
            }
        }
    }

    // Dump Pipeline
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

    if (status != HALT)
    {
        dumpPipeState(pipeState, output);
    }
    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt()
{
    Status status;
    while (true)
    {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT)
            break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator()
{
    simulator->dumpRegMem(output);
    SimulationStats stats{simulator->getDin(), cycleCount, 0, 0, 0, 0, 0}; // TODO incomplete implementation
    dumpSimStats(stats, output);
    return SUCCESS;
}
