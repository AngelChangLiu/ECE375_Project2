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
// completed instruction count
static uint64_t completedCount = 0;
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
        // Simulator::Instruction spec_decode = simulator->simID(prevIFInst);

        bool stall = false;
        // int newStallCycles = 0;
        bool illegalExc = false;
        bool memExc = false;

        if (!prevIDInst.isLegal && !prevIDInst.isNop &&
            prevIDInst.status != BUBBLE &&
            prevIDInst.status != SQUASHED &&
            !prevIDInst.isHalt)
        {
            std::cout << "[DEBUG] illegalexc: " << cycleCount << std::endl;
            illegalExc = true;
        }

        // if (prevIDInst.isLegal && !prevIDInst.isNop &&
        //     prevIDInst.status != BUBBLE &&
        //     prevIDInst.status != SQUASHED &&
        //     prevIDInst.isHalt) {
        //         std::cout << "[DEBUG] HALT: " << cycleCount << std::endl;
        //     }

        CacheOperation operationType = CACHE_READ;
        if (prevMEMInst.writesMem)
        {
            operationType = CACHE_WRITE;
        }
        if (!dataFetchMiss && !prevMEMInst.isNop && (prevMEMInst.readsMem || prevMEMInst.writesMem) && !dCache->access(prevMEMInst.memAddress, operationType))
        {
            std::cout << "[DEBUG] Cache Miss, Cycle: " << cycleCount << std::endl;
            dataFetchMiss = true;
            dataMissStalls = 0;
        }
        else if (dataFetchMiss)
        {
            dataMissStalls += 1;
        }

        if (!instFetchMiss && !prevIFInst.isNop && !iCache->access(prevIFInst.PC, CACHE_READ))
        {
            std::cout << "[DEBUG] Instuction Cache Miss, Cycle: " << cycleCount << std::endl;
            std::cout << "[DEBUG] Instuction Cache Miss, PC: " << prevIFInst.PC << std::endl;
            instFetchMiss = true;
            instMissStalls = 0;
        }
        else if (instFetchMiss)
        {
            instMissStalls += 1;
        }

        if (remainingStallCycles > 0)
        {
            stall = true;
            remainingStallCycles--;
        }
        else
        {
            // Check for data hazards
            // LOAD-USE:
            if (isLoad(prevEXInst))
            {
                uint64_t loadRd = prevEXInst.rd;

                if (isStore(prevIDInst))
                {
                    if ((prevIDInst.readsRs1 && prevIDInst.rs1 == loadRd) && (loadRd != 0))
                    {
                        stall = true;
                        loadStallCount++;
                    }
                }

                // LOAD_BRANCH:
                else if (isBranchOrJump(prevIDInst))
                {

                    bool stallRequired = false;

                    if ((prevIDInst.readsRs1 && prevIDInst.rs1 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }
                    if ((prevIDInst.readsRs2 && prevIDInst.rs2 == loadRd) && (loadRd != 0))
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

                    if ((prevIDInst.readsRs1 && prevIDInst.rs1 == loadRd) && (loadRd != 0))
                    {
                        stallRequired = true;
                    }
                    if ((prevIDInst.readsRs2 && prevIDInst.rs2 == loadRd) && (loadRd != 0))
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

            // ARITHMETIC-BRANCH
            if (!stall && isBranchOrJump(prevIDInst))
            {
                if (writesREG(prevEXInst) && !isLoad(prevEXInst))
                {
                    uint64_t aluRd = prevEXInst.rd;
                    bool stallRequired = false;

                    if ((prevIDInst.readsRs1 && prevIDInst.rs1 == aluRd) && (aluRd != 0))
                    {
                        stallRequired = true;
                    }

                    if ((prevIDInst.readsRs2 && prevIDInst.rs2 == aluRd) && (aluRd != 0))
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

        if (!pipelineInfo.wbInst.isNop && 
            pipelineInfo.wbInst.isLegalpipelineInfo.wbInst.status != BUBBLE &&
            pipelineInfo.wbInst.status != SQUASHED && 
            !pipelineInfo.wbInst.isHalt) {
                 completedCount++;
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

        // Forward from EX → ID
        if (writesREG(prevEXInst) && isBranchOrJump(prevIDInst))
        {
            uint64_t exRd = prevEXInst.rd;
            // if the prev ID is a branch and the mem instruction reads or writes to a reg that the branch needs, forward it to ID
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
        if (illegalExc)
        {
            std::cout << "[DEBUG] IllegalExc (EX): " << cycleCount << std::endl;
            pipelineInfo.exInst = nop(SQUASHED);
        }
        else if (dataFetchMiss)
        {
            pipelineInfo.exInst = prevEXInst;
        }
        else if (stall)
        {
            pipelineInfo.exInst = nop(BUBBLE);
        }
        else
        {
            pipelineInfo.exInst = simulator->simEX(prevIDInst);
        }
        // else if (instFetchMiss)
        // {
        //     pipelineInfo.exInst = simulator->simEX(prevIDInst);
        //     pipelineInfo.idInst = nop(BUBBLE);
        // }

        std::cout << "[DEBUG] Decode Stage, Cycle: " << cycleCount << std::endl;
        // ID Stage:
        if (illegalExc)
        {
            std::cout << "[DEBUG] Illegal Exc: " << illegalExc << std::endl;
            pipelineInfo.idInst = nop(SQUASHED);
        }
        else if (dataFetchMiss)
        {
            std::cout << "[DEBUG] dataFetchMiss: " << dataFetchMiss << std::endl;
            pipelineInfo.idInst = prevIDInst;
        }
        else if (stall)
        {
            std::cout << "[DEBUG] Stall: " << stall << std::endl;
            if (isBranchOrJump(prevIDInst))
            {
                pipelineInfo.idInst = simulator->simNextPCResolution(prevIDInst);
            }
            else
            {
                pipelineInfo.idInst = prevIDInst;
            }
            // if whati s being stalled is a branch instruction, re compute outcome
        }
        else if (instFetchMiss)
        {
            std::cout << "[DEBUG] instFetchMiss: " << instFetchMiss << std::endl;
            pipelineInfo.idInst = nop(BUBBLE);
        }
        else if (prevIFInst.status == SPECULATIVE && prevIFInst.PC != (PC - 4))
        {
            pipelineInfo.idInst = nop(SQUASHED);
        }
        else
        {
            if (!prevIFInst.isNop && !(prevIFInst.status == BUBBLE) && !(prevIFInst.status == SQUASHED) && !(prevIFInst.status == IDLE))
            {
                prevIFInst.status = NORMAL;
            }
            pipelineInfo.idInst = simulator->simID(prevIFInst);
        }

        // Check for Branch
        bool branchInId = false;
        bool taken = false;
        // REMOVED PART '!stall &&' from conditional below:
        // Determine whether other conditionals beside isBranchOrJump are needed.
        if (!dataFetchMiss && !instFetchMiss && isBranchOrJump(pipelineInfo.idInst))
        {
            std::cout << "[DEBUG] Branch Resolution, Cycle: " << cycleCount << std::endl;
            branchInId = true;
            // simulator->simNextPCResolution(prevIDInst);
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4)
            {
                taken = true;
                std::cout << "[DEBUG] Branch Taken? " << taken << std::endl;
            }
        }

        std::cout << "[DEBUG] Fetch Stage, Cycle: " << cycleCount << std::endl;
        // Instruction Fetch Stage
        if (illegalExc)
        {
            std::cout << "[DEBUG] Illegal Exception (IF): " << cycleCount << std::endl;
            instFetchMiss = false;
            instMissStalls = 0;
            PC = EXC_HANDLER;
            pipelineInfo.ifInst = simulator->simIF(PC);
        }

        else if (memExc)
        {
            std::cout << "[DEBUG] Memory Exception, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst.status = SQUASHED;
            pipelineInfo.idInst.status = SQUASHED;
            pipelineInfo.exInst.status = SQUASHED;

            instFetchMiss = false;
            instMissStalls = 0;
            PC = EXC_HANDLER;
        }

        else if (stall)
        {

            // Angel debuggin
            std::cout << "[DEBUG] STALL at cycle " << cycleCount << std::endl;

            pipelineInfo.ifInst = prevIFInst;

            // if (isBranchOrJump(pipelineInfo.idInst))
            // {
            //     pipelineInfo.ifInst.status = SPECULATIVE;
            // }

            if (branchInId)
            {
                std::cout << "[DEBUG] Re-Resolve Branch: " << cycleCount << std::endl;
                PC = pipelineInfo.idInst.nextPC;
                if (taken)
                {
                    std::cout << "[DEBUG] Branch taken → PC = 0x"
                              << std::hex << pipelineInfo.idInst.nextPC
                              << std::dec << std::endl;

                    instFetchMiss = false;
                    instMissStalls = 0;
                }
                else
                {
                    PC += 4;
                }
            }
        }

        else if (branchInId)
        {
            std::cout << "[DEBUG] Fetch Cache Access, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst = simulator->simIF(PC);
            pipelineInfo.ifInst.status = SPECULATIVE;
            PC = pipelineInfo.idInst.nextPC;
            if (taken)
            {
                std::cout << "[DEBUG] Branch taken → PC = 0x"
                          << std::hex << pipelineInfo.idInst.nextPC
                          << std::dec << std::endl;

                instFetchMiss = false;
                instMissStalls = 0;
            }
        }
        else if (instFetchMiss)
        {
            std::cout << "[DEBUG] IF InstFetchMiss, PC: " << prevIFInst.PC << std::endl;
            pipelineInfo.ifInst = prevIFInst;
        }
        else if (dataFetchMiss)
        {
            std::cout << "[DEBUG] IF Stall, Cycle: " << cycleCount << std::endl;
            pipelineInfo.ifInst = prevIFInst;
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

        if (instMissStalls >= iCache->config.missLatency - 1)
        {
            instMissStalls = 0;
            instFetchMiss = false;
        }
        if (dataMissStalls >= dCache->config.missLatency - 1)
        {
            dataMissStalls = 0;
            dataFetchMiss = false;
        }

        count++;
        cycleCount++;

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
    // for (int i = 0; i < 50; i++)
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
    SimulationStats stats{
        completedCount,
        cycleCount,
        iCache->getHits(),
        iCache->getMisses(),
        dCache->getHits(),
        dCache->getMisses(),
        loadStallCount};
    dumpSimStats(stats, output);
    return SUCCESS;
}