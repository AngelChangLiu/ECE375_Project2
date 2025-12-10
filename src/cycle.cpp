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
// count of load-use stalls
static uint64_t loadStallCount = 0;
// remaining stall cycles
static uint64_t remainingStallCycles = 0;
static uint64_t PC = 0;

// Exception Address
#define EXC_HANDLER 0x8000

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

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles)
{
    uint64_t fowardValue;
    uint64_t count = 0;
    auto status = SUCCESS;
    PipeState pipeState = {
        0,
    };

    bool dataFetchMiss = dCache->fetchMiss;
    bool instFetchMiss = iCache->fetchMiss;
    uint64_t dataMissStalls = dCache->fetchStalls;
    uint64_t instMissStalls = iCache->fetchStalls;

    while (cycles == 0 || count < cycles)
    {

        pipeState.cycle = cycleCount;

        std::cout << "[DEBUG] Starting cycle " << cycleCount << std::endl;

        // Save Previous Cycle Pipeline State
        Simulator::Instruction prevIFInst = pipelineInfo.ifInst;
        Simulator::Instruction prevIDInst = pipelineInfo.idInst;
        Simulator::Instruction prevEXInst = pipelineInfo.exInst;
        Simulator::Instruction prevMEMInst = pipelineInfo.memInst;
        // Simulator::Instruction prevWBInst = pipelineInfo.wbInst;

        // Speculative Decode
        Simulator::Instruction spec_decode = simulator->simID(prevIFInst);

        bool stall = false;
        // int newStallCycles = 0;
        bool illegalExc = false;
        bool memExc = false;
 
        if (remainingStallCycles > 0)
        {
            stall = true;
            remainingStallCycles--;
        }
        else
        {
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

                else if (isBranchOrJump(spec_decode))
                {

                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }
                    if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }

                    if (stallRequired)
                    {
                        stall = true;
                        remainingStallCycles = 1;
                        loadStallCount++;
                    }
                }

                else
                {

                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }
                    if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }

                    if (stallRequired)
                    {
                        stall = true;
                        loadStallCount++;
                    }
                }
            }

            if (!stall && isBranchOrJump(spec_decode) && isLoad(prevEXInst))
            {
                uint64_t loadRd = prevEXInst.rd;
                bool stallRequired = false;

                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0))
                {
                    stallRequired = true;
                }

                if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0))
                {
                    stallRequired = true;
                }

                if (stallRequired)
                {
                    stall = true;
                    loadStallCount++;
                }
            }

            // Check for data hazards with branches/jumps
            if (!stall && isBranchOrJump(spec_decode))
            {
                if (writesREG(prevIDInst) && !isLoad(prevIDInst))
                {
                    uint64_t aluRd = prevIDInst.rd;
                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == aluRd) && (aluRd != 0))
                    {
                        stallRequired = true;
                    }

                    if ((spec_decode.readsRs2 && spec_decode.rs2 == aluRd) && (aluRd != 0))
                    {
                        stallRequired = true;
                    }

                    if (stallRequired)
                    {
                        stall = true;
                    }
                }
            }
        }

        std::cout << "[DEBUG] Write-Back Stage, Cycle: " << cycleCount << std::endl;
        // WB Stage:
        if (dataFetchMiss)
        {
            pipelineInfo.wbInst = nop(BUBBLE);
        }
        else
        {
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

        // Foward WB -> MEM
        if (isStore(prevEXInst) && writesREG(prevMEMInst) && !dataFetchMiss)
        {
            uint64_t wbRd = prevMEMInst.rd;

            if ((prevEXInst.rs2 == wbRd) && (wbRd != 0))
            {
                if (isLoad(prevMEMInst))
                {
                    prevEXInst.op2Val = prevMEMInst.memResult;
                }

                else
                {
                    prevEXInst.op2Val = prevMEMInst.arithResult;
                }
            }
        }

        // MEM Stage:
        if (dataFetchMiss)
        {
            pipelineInfo.memInst = prevMEMInst;

        }
        else 
        {
            pipelineInfo.memInst = simulator->simMEM(prevEXInst);
        }
        
        if (pipelineInfo.memInst.memException)
        {
            memExc = true;
            pipelineInfo.memInst.status = SQUASHED;
        }

        // Forward from MEM → EX
        if (writesREG(prevMEMInst))
        {
            uint64_t memRd = prevMEMInst.rd;

            if (isLoad(prevMEMInst))
            {
                fowardValue = prevMEMInst.memResult;
            }

            else
            {
                fowardValue = prevMEMInst.arithResult;
            }

            if (prevIDInst.readsRs1 && prevIDInst.rs1 == memRd && memRd != 0)
            {
                prevIDInst.op1Val = fowardValue;
            }
            if (prevIDInst.readsRs2 && prevIDInst.rs2 == memRd && memRd != 0)
            {
                prevIDInst.op2Val = fowardValue;
            }
        }

        // Forward from EX → EX
        if (writesREG(prevEXInst))
        {
            uint64_t exRd = prevEXInst.rd;

            if (prevIDInst.readsRs1 && prevIDInst.rs1 == exRd && exRd != 0)
            {
                prevIDInst.op1Val = prevEXInst.arithResult;
            }
            if (prevIDInst.readsRs2 && prevIDInst.rs2 == exRd && exRd != 0)
            {
                prevIDInst.op2Val = prevEXInst.arithResult;
            }
        }

        std::cout << "[DEBUG] Execute Stage, Cycle: " << cycleCount << std::endl;
        // EX Stage:
        if (dataFetchMiss)
        {
            pipelineInfo.exInst = prevEXInst;
        }
        else if (instFetchMiss) 
        {
            pipelineInfo.exInst = simulator->simEX(prevIDInst);
            pipelineInfo.idInst = nop(BUBBLE);
        }
        else 
        {
            pipelineInfo.exInst = simulator->simEX(prevIDInst);
        }

        std::cout << "[DEBUG] Decode Stage, Cycle: " << cycleCount << std::endl;
        // ID Stage:
        if (dataFetchMiss)
        {
            pipelineInfo.idInst = prevIDInst;
        } 
        else if (stall || instFetchMiss)
        {
            std::cout << "[DEBUG] Stall: " << stall << std::endl;
            std::cout << "[DEBUG] instFetchMiss: " << instFetchMiss << std::endl;
            if (stall) pipelineInfo.idInst = nop(BUBBLE);
        }
        else
        {
            pipelineInfo.idInst = simulator->simID(prevIFInst);
        }                       

        if (!pipelineInfo.idInst.isLegal && !pipelineInfo.idInst.isNop &&
            pipelineInfo.idInst.status != BUBBLE &&
            pipelineInfo.idInst.status != SQUASHED &&
            !pipelineInfo.idInst.isHalt)
        {
            illegalExc = true;
            pipelineInfo.idInst.status = SQUASHED;
        }

        // Check for Branch
        bool taken = false;
        if (!stall && !dataFetchMiss && !instFetchMiss && isBranchOrJump(pipelineInfo.idInst))        {
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4)
            {
                taken = true;
            }
        }

        if (!instFetchMiss && !dataFetchMiss && !iCache->access(PC, CACHE_READ))
        {
            std::cout << "[DEBUG] Cache Miss, Cycle: " << cycleCount << std::endl;
            std::cout << "[DEBUG] Cache Miss, PC: " << PC << std::endl;
            instFetchMiss = true;
            instMissStalls = 0;
        }
        else if (instFetchMiss) {
            instMissStalls += 1;
        } 

        std::cout << "[DEBUG] Fetch Stage, Cycle: " << cycleCount << std::endl;
        // Instruction Fetch Stage
        if (illegalExc)
        {
            std::cout << "[DEBUG] Illegal Exception, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst.status = SQUASHED;
            PC = EXC_HANDLER;
        }

        else if (memExc)
        {
            std::cout << "[DEBUG] Memory Exception, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst.status = SQUASHED;
            pipelineInfo.idInst.status = SQUASHED;
            pipelineInfo.exInst.status = SQUASHED;
            PC = EXC_HANDLER;
        }

        else if (stall)
        {

            // Angel debuggin
            std::cout << "[DEBUG] STALL at cycle " << cycleCount << std::endl;

            pipelineInfo.ifInst = prevIFInst;

            if (isBranchOrJump(spec_decode))
            {
                pipelineInfo.ifInst.status = SPECULATIVE;
            }
        }

        else if (taken)
        {

            // Angel debuggin
            std::cout << "[DEBUG] Branch taken → PC = 0x"
                      << std::hex << pipelineInfo.idInst.nextPC
                      << std::dec << std::endl;

            prevIFInst.status = SQUASHED;
            pipelineInfo.ifInst = prevIFInst;
            PC = pipelineInfo.idInst.nextPC;
        }
        else if (instFetchMiss) {
            pipelineInfo.ifInst = simulator->simIF(PC);
            std::cout << "[DEBUG] IF InstFetchMiss, PC: " << PC << std::endl;
        }  
        else if (dataFetchMiss) 
        {
            pipelineInfo.ifInst = prevIFInst;
            std::cout << "[DEBUG] IF Stall, Cycle: " << cycleCount << std::endl;
        }
        else
        {
            std::cout << "[DEBUG] Fetch Cache Access, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst = simulator->simIF(PC);
            // Angel debuggin
            std::cout << "[DEBUG] PC moves to " << std::hex << PC + 4 << std::dec << std::endl;
            PC += 4;

            if (isBranchOrJump(pipelineInfo.idInst))
            {
                pipelineInfo.ifInst.status = SPECULATIVE;
            }
        }

        CacheOperation operationType = CACHE_READ;
        if (pipelineInfo.memInst.writesMem)
        {
            operationType = CACHE_WRITE;
        }
        if (!dataFetchMiss && !pipelineInfo.memInst.isNop && (pipelineInfo.memInst.readsMem || pipelineInfo.memInst.writesMem) && !dCache->access(pipelineInfo.memInst.memAddress, operationType))        {
            std::cout << "[DEBUG] Cache Miss, Cycle: " << cycleCount << std::endl;
            dataFetchMiss = true;
            dataMissStalls = 0;
        }
        else if (dataFetchMiss) 
        {
            dataMissStalls += 1;
        }

        if (instMissStalls >= iCache->config.missLatency)
            {
                instMissStalls = 0;
                instFetchMiss = false;
                PC += 4;
            }
        if (dataMissStalls >= dCache->config.missLatency)
            {
                dataMissStalls = 0;
                dataFetchMiss = false;
            }

        count++;
        cycleCount++;
    }

    // Dump Cache Status:
    dCache->fetchMiss = dataFetchMiss;
    dCache->fetchStalls = dataMissStalls;
    iCache->fetchMiss = instFetchMiss;
    iCache->fetchStalls = instMissStalls;

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
    // for (int i = 0; i < 15; i++)
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
    SimulationStats stats{
        simulator->getDin(),
        cycleCount,
        iCache->getHits(),
        iCache->getMisses(),
        dCache->getHits(),
        dCache->getMisses(),
        loadStallCount};
    dumpSimStats(stats, output);
    return SUCCESS;
}
